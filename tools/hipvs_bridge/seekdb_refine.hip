/* seekdb_refine.hip — [V5-P4-S2] custom GPU refine round (one HIP TU).
 *
 * Per row i of the GNND 256-wide graph:
 *   1. candidates  = 0-hop row i (priority, self excluded)
 *                  + 1-hop of the first 32 columns of row i
 *   2. dedup       = per-row LDS bitmap over all n ids; stash capped at CAP
 *                    (0-hop fills first), ids >= n and == i dropped
 *   3. exact rank  = fp32 L2^2 for every stashed candidate against the fp32
 *                    dataset (design note: preflight probe3 measured the fp32
 *                    path at 0.51 us/candidate vs 1.82 us/candidate for the
 *                    fp16-screen variant, so the fp16 screening level from the
 *                    earlier design was removed entirely)
 *   4. output      = top-256 sorted ascending (iterative block argmin, ties
 *                    broken by smaller id); rows short of 256 valid candidates
 *                    are topped up deterministically from (i+1)%n upward so no
 *                    slot ever carries an illegal or duplicate id
 *
 * Device memory (cuvsRMMAlloc, RMM pool shared with the GNND stage):
 *   d_base n*dim*4 (128 MB) + d_stage n*256*4 (256 MB) + d_tile rows/2*256*4
 *   (128 MB) ~ 512 MB at SIFT250K/128-dim; freed before the index injection.
 * The round writes tiles into d_tile (rows never read what other rows write),
 * so the round is deterministic apart from the CAP-boundary stash order, and
 * in_graph stays intact (multi-round re-reads it via a fresh H2D).
 *
 * Wave-size policy: block-wide reductions run through LDS/shared arrays with
 * no wave-width assumptions; the same source compiles for gfx942 and gfx1100.
 */
#include "seekdb_refine.h"

#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define REFINE_GD       (SEEKDB_REFINE_GRAPH_DEGREE)
#define REFINE_NEXPAND  32    /* 1-hop expansion depth */
#define REFINE_CAP      4096  /* unique-candidate stash cap */
#define REFINE_TPB      256   /* threads per block */
#define REFINE_TOPPOST  4     /* per-thread survivors posted for selection */
#define REFINE_POSTS    (REFINE_TPB * REFINE_TOPPOST)

#define HIPCHECK(expr) do { hipError_t e_ = (expr); if (e_ != hipSuccess) { \
  fprintf(stderr, "[seekdb_refine] HIP fail %s @%d: %s\n", #expr, __LINE__, \
          hipGetErrorString(e_)); return 3; } } while (0)

__global__ void seekdb_refine_kernel(const float* __restrict__ base,
                                     const uint32_t* __restrict__ stage,
                                     uint32_t* __restrict__ out_tile,
                                     long n, long dim, int bitwords,
                                     long row0)
{
  extern __shared__ unsigned char smem_raw[];
  uint32_t* bits  = (uint32_t*)smem_raw;               /* bitwords */
  uint32_t* stash = bits + bitwords;                   /* REFINE_CAP */
  uint32_t* posts = stash + REFINE_CAP;                /* POSTS x (id,dist) */
  float*    sq    = (float*)(posts + REFINE_POSTS * 2);/* dim floats */
  __shared__ uint32_t scnt;

  const long i   = row0 + (long)blockIdx.x;
  const uint32_t ui = (uint32_t)i;

  for (int w = threadIdx.x; w < bitwords; w += REFINE_TPB) bits[w] = 0u;
  if (threadIdx.x == 0) scnt = 0;
  for (int d = threadIdx.x; d < dim; d += REFINE_TPB)
    sq[d] = base[i * dim + d];
  __syncthreads();

  /* ---- gather: 0-hop priority, then 1-hop of the first NEXPAND columns ---- */
  {
    const uint32_t* row = stage + i * REFINE_GD;
    for (int j = threadIdx.x; j < REFINE_GD; j += REFINE_TPB) {
      uint32_t id = row[j];
      if (id < (uint32_t)n && id != ui &&
          !(atomicOr(&bits[id >> 5], 1u << (id & 31)) & (1u << (id & 31)))) {
        int s = atomicAdd(&scnt, 1u);
        if (s < REFINE_CAP) stash[s] = id;
      }
    }
  }
  __syncthreads();
  for (int k = 0; k < REFINE_NEXPAND; k++) {
    uint32_t j = stage[i * REFINE_GD + k];
    if (j < (uint32_t)n && j != ui) {
      const uint32_t* row = stage + (long)j * REFINE_GD;
      for (int t = threadIdx.x; t < REFINE_GD; t += REFINE_TPB) {
        uint32_t id = row[t];
        if (id < (uint32_t)n && id != ui &&
            !(atomicOr(&bits[id >> 5], 1u << (id & 31)) & (1u << (id & 31)))) {
          int s = atomicAdd(&scnt, 1u);
          if (s < REFINE_CAP) stash[s] = id;
        }
      }
    }
  }
  __syncthreads();
  /* Deterministic top-up for degenerate rows with fewer than GD candidates:
   * scan ids (i+1)%n upward, bitmap-guarded, so the row still yields GD
   * valid, duplicate-free entries. Real converged rows never enter this. */
  if (scnt < REFINE_GD) {
    __syncthreads();
    for (uint32_t step = 0; scnt < REFINE_GD && step < (uint32_t)n; step += REFINE_TPB) {
      uint32_t id = (ui + 1u + step + threadIdx.x) % (uint32_t)n;
      if (!(atomicOr(&bits[id >> 5], 1u << (id & 31)) & (1u << (id & 31)))) {
        int s = atomicAdd(&scnt, 1u);
        if (s < REFINE_CAP) stash[s] = id;
      }
    }
    __syncthreads();
  }

  /* ---- exact fp32 ranking of every stashed candidate ---- */
  float    bd[REFINE_TOPPOST];
  uint32_t bi[REFINE_TOPPOST];
#pragma unroll
  for (int t = 0; t < REFINE_TOPPOST; t++) { bd[t] = 3.4e38f; bi[t] = 0xFFFFFFFFu; }
  int cnt = (int)scnt;
  if (cnt > REFINE_CAP) cnt = REFINE_CAP;
  for (int c = threadIdx.x; c < cnt; c += REFINE_TPB) {
    uint32_t id = stash[c];
    const float* r = base + (long)id * dim;
    float acc = 0.f;
    for (int d = 0; d < dim; d++) {
      float t = sq[d] - r[d];
      acc += t * t;
    }
    if (acc < bd[REFINE_TOPPOST - 1]) {
      int p = REFINE_TOPPOST - 1;
      while (p > 0 && bd[p - 1] > acc) { bd[p] = bd[p - 1]; bi[p] = bi[p - 1]; p--; }
      bd[p] = acc; bi[p] = id;
    }
  }
#pragma unroll
  for (int t = 0; t < REFINE_TOPPOST; t++) {
    posts[(threadIdx.x * REFINE_TOPPOST + t) * 2]     = bi[t];
    posts[(threadIdx.x * REFINE_TOPPOST + t) * 2 + 1] = __float_as_uint(bd[t]);
  }
  __syncthreads();

  /* ---- one bitonic sort of the posts (ascending by (dist,id)), then the
   * first GD slots are the sorted output row. 55 stages / ~55 barriers vs
   * the 256-round iterative argmin (~3000 barriers) this replaces: the
   * argmin variant measured 8.3 s/round at 250K on gfx942, selection-bound. */
  for (int k = 2; k <= REFINE_POSTS; k <<= 1) {
    for (int j = k >> 1; j > 0; j >>= 1) {
      __syncthreads();
      for (int p = threadIdx.x; p < REFINE_POSTS; p += REFINE_TPB) {
        int partner = p ^ j;
        if (partner > p) {
          bool up = ((p & k) == 0);
          float da = __uint_as_float(posts[p * 2 + 1]);
          uint32_t ia = posts[p * 2];
          float db = __uint_as_float(posts[partner * 2 + 1]);
          uint32_t ib = posts[partner * 2];
          bool a_gt_b = (da > db) || (da == db && ia > ib);
          if (a_gt_b == up) {
            posts[p * 2] = ib;
            posts[p * 2 + 1] = posts[partner * 2 + 1];
            posts[partner * 2] = ia;
            posts[partner * 2 + 1] = __float_as_uint(da);
          }
        }
      }
    }
  }
  __syncthreads();
  for (int p = threadIdx.x; p < REFINE_GD; p += REFINE_TPB)
    out_tile[(long)blockIdx.x * REFINE_GD + p] = posts[p * 2];
}

int seekdb_refine_round(cuvsResources_t res, const float* base, long n,
                        long dim, const uint32_t* in_graph,
                        uint32_t* out_graph)
{
  if (!res || !base || !in_graph || !out_graph || n <= 0 || dim <= 0) return 1;
  const long tile_rows = n / 2 > 0 ? n / 2 : n;   /* two tiles per round */
  const int  bitwords  = (int)((n + 31) / 32);
  const size_t smem    = (size_t)bitwords * 4 + (size_t)REFINE_CAP * 4 +
                         (size_t)REFINE_POSTS * 2 * 4 + (size_t)dim * 4;

  /* VRAM reconciliation log (ops diagnostics, mirrors bridge stderr style) */
  const char* vlog = getenv("SEEKDB_REFINE_VRAM_LOG");
  size_t vf = 0, vt = 0;
  hipEvent_t ev0 = NULL, ev1 = NULL;
  float ms_h2d = 0.f, ms_k = 0.f, ms_d2h = 0.f;
  if (vlog) { hipEventCreate(&ev0); hipEventCreate(&ev1); }

  float*    d_base  = NULL;
  uint32_t* d_stage = NULL;
  uint32_t* d_tile  = NULL;
  HIPCHECK(hipMemGetInfo(&vf, &vt));
  if (vlog) fprintf(stderr, "[seekdb_refine] vram before alloc: free=%.1f MB total=%.1f MB\n",
                    vf / 1048576.0, vt / 1048576.0);

  if (cuvsRMMAlloc(res, (void**)&d_base, (size_t)n * dim * 4) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_refine] rmm d_base failed: %s\n", cuvsGetLastErrorText());
    return 2;
  }
  if (cuvsRMMAlloc(res, (void**)&d_stage, (size_t)n * REFINE_GD * 4) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_refine] rmm d_stage failed: %s\n", cuvsGetLastErrorText());
    cuvsRMMFree(res, d_base, (size_t)n * dim * 4);
    return 2;
  }
  if (cuvsRMMAlloc(res, (void**)&d_tile, (size_t)tile_rows * REFINE_GD * 4) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_refine] rmm d_tile failed: %s\n", cuvsGetLastErrorText());
    cuvsRMMFree(res, d_stage, (size_t)n * REFINE_GD * 4);
    cuvsRMMFree(res, d_base, (size_t)n * dim * 4);
    return 2;
  }
  if (vlog && hipMemGetInfo(&vf, &vt) == hipSuccess)
    fprintf(stderr, "[seekdb_refine] vram after alloc (base+stage+tile=%.0f MB): free=%.1f MB\n",
            ((double)n * dim * 4 + (double)n * REFINE_GD * 4 + (double)tile_rows * REFINE_GD * 4) / 1048576.0,
            vf / 1048576.0);

  if (vlog) hipEventRecord(ev0, 0);
  HIPCHECK(hipMemcpy(d_base, base, (size_t)n * dim * 4, hipMemcpyHostToDevice));
  HIPCHECK(hipMemcpy(d_stage, in_graph, (size_t)n * REFINE_GD * 4, hipMemcpyHostToDevice));
  if (vlog) {
    hipEventRecord(ev1, 0);
    hipEventSynchronize(ev1);
    hipEventElapsedTime(&ms_h2d, ev0, ev1);
  }

  long done = 0;
  HIPCHECK(hipFuncSetAttribute((const void*)seekdb_refine_kernel,
                               hipFuncAttributeMaxDynamicSharedMemorySize,
                               (int)smem));
  while (done < n) {
    long rows = n - done < tile_rows ? n - done : tile_rows;
    dim3 grid((unsigned)rows), blk(REFINE_TPB);
    if (vlog) hipEventRecord(ev0, 0);
    hipLaunchKernelGGL(seekdb_refine_kernel, grid, blk, smem, 0,
                       d_base, d_stage, d_tile,
                       n, dim, bitwords, done);
    hipError_t kerr = hipGetLastError();
    if (kerr != hipSuccess) {
      fprintf(stderr, "[seekdb_refine] kernel launch fail: %s\n", hipGetErrorString(kerr));
      cuvsRMMFree(res, d_tile, (size_t)tile_rows * REFINE_GD * 4);
      cuvsRMMFree(res, d_stage, (size_t)n * REFINE_GD * 4);
      cuvsRMMFree(res, d_base, (size_t)n * dim * 4);
      return 3;
    }
    HIPCHECK(hipDeviceSynchronize());
    if (vlog) {
      hipEventRecord(ev1, 0);
      hipEventSynchronize(ev1);
      float ms = 0.f;
      hipEventElapsedTime(&ms, ev0, ev1);
      ms_k += ms;
    }
    /* d_tile is indexed by (i - row0) inside the kernel: block b writes slot b.
     * Copy the tile back to out_graph rows [done, done+rows). */
    if (vlog) hipEventRecord(ev0, 0);
    HIPCHECK(hipMemcpy(out_graph + done * REFINE_GD, d_tile,
                       (size_t)rows * REFINE_GD * 4, hipMemcpyDeviceToHost));
    if (vlog) {
      hipEventRecord(ev1, 0);
      hipEventSynchronize(ev1);
      float ms = 0.f;
      hipEventElapsedTime(&ms, ev0, ev1);
      ms_d2h += ms;
    }
    done += rows;
  }
  if (vlog)
    fprintf(stderr, "[seekdb_refine] phases: kernel=%.0f ms d2h=%.0f ms "
                    "(h2d base+stage=%.0f ms incl.)\n", ms_k, ms_d2h,
            ms_h2d);

  cuvsRMMFree(res, d_tile, (size_t)tile_rows * REFINE_GD * 4);
  cuvsRMMFree(res, d_stage, (size_t)n * REFINE_GD * 4);
  cuvsRMMFree(res, d_base, (size_t)n * dim * 4);
  if (vlog && hipMemGetInfo(&vf, &vt) == hipSuccess)
    fprintf(stderr, "[seekdb_refine] vram after free: free=%.1f MB\n", vf / 1048576.0);
  return 0;
}
