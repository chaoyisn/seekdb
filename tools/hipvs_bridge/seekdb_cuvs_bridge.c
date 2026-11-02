/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include "v2_profile.h"

#ifdef OB_V2_PROFILE
/* One pair of process-wide events, created once. Every measured section
 * records on the default (null) stream, which is the stream RAFT/cuVS uses
 * because cuvsStreamSet() is never called on these resources. */
static hipEvent_t g_v2_ev0 = NULL;
static hipEvent_t g_v2_ev1 = NULL;
/* Start timestamp of the section currently being measured.  It is thread
 * local: the adaptor runs every GPU call on its own pthread, and two threads
 * must not overwrite each other's start time.  The measured sections inside
 * one bridge call are strictly sequential, never nested. */
static _Thread_local long g_v2_t0 = 0;
static void v2_prof_events_init(void) {
  if (g_v2_ev0 == NULL) {
    /* Timing must stay ENABLED: hipEventElapsedTime() on events created with
     * hipEventDisableTiming is defined to return 0, which would silently
     * turn every gpu_us into a zero. */
    hipEventCreateWithFlags(&g_v2_ev0, 0);
    hipEventCreateWithFlags(&g_v2_ev1, 0);
  }
}
#define V2_T0() v2_prof_events_init(); g_v2_t0 = v2_prof_now_us(); \
               hipEventRecord(g_v2_ev0, NULL)
#define V2_T1(stage, a, b, note) do {                                        \
    hipEventRecord(g_v2_ev1, NULL);                                         \
    hipEventSynchronize(g_v2_ev1);                                          \
    float _v2_gpu_ms = 0.0f;                                                \
    hipEventElapsedTime(&_v2_gpu_ms, g_v2_ev0, g_v2_ev1);                    \
    long _v2_wall = v2_prof_now_us() - g_v2_t0;                             \
    v2_prof_emit(stage, _v2_wall, (long)(_v2_gpu_ms * 1000.0f),             \
                 (long)(a), (long)(b), (note));                              \
  } while (0)
#else
#define V2_T0() do { } while (0)
#define V2_T1(stage, a, b, note) v2_prof_emit(stage, 0, 0, (a), (b), (note))
#endif

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
} sk_cuvs_index;

/* The single source of truth for the NN-Descent iteration count of the V3.1
 * sweep.  Every candidate is exactly one commit that changes this constant
 * (and nothing else); the runtime log line below echoes the value that the
 * CAGRA build actually used, so a measured round can be tied to its
 * candidate without trusting file names. */
#ifndef SEEKDB_CUVS_NN_NITER
#define SEEKDB_CUVS_NN_NITER 64
#endif
#define SK_STR2(x) #x
#define SK_STR(x) SK_STR2(x)
void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  V2_T0();
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  V2_T1("res_create", n, dim, "cuvsResourcesCreate");

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  V2_T0();
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  V2_T1("ip_create", n, dim, "cuvsCagraIndexParamsCreate");
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  h->ip->nn_descent_niter = SEEKDB_CUVS_NN_NITER;
  fprintf(stderr, "[seekdb_cuvs_bridge] cagra build params: algo=NN_DESCENT "
          "graph_degree=%d intermediate_graph_degree=%d nn_descent_niter=%d "
          "n=%ld dim=%ld\n", h->ip->graph_degree,
          h->ip->intermediate_graph_degree, h->ip->nn_descent_niter, n, dim);
  V2_T0();
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  V2_T1("idx_create", n, dim, "cuvsCagraIndexCreate");
  V2_T0();
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  V2_T1("cagra_build", n, dim, "cuvsCagraBuild NN_DESCENT deg=128 inter=256 "
        "niter=" SK_STR(SEEKDB_CUVS_NN_NITER));
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  V2_T0();
  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  V2_T1("rmm_alloc", sizeof(float)*nq*h->dim + sizeof(unsigned)*nq*topk*2, nq, "3x cuvsRMMAlloc");
  V2_T0();
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  V2_T1("h2d_query", sizeof(float)*nq*h->dim, nq, "cudaMemcpy H2D query");
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    V2_T0();
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    V2_T1("search_params_create", topk, nq, "cuvsCagraSearchParamsCreate");
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      /* hipVS hard-caps itopk_size at 512 (detail/cagra/search_single_cta.cuh:139):
       * a larger value aborts the search with a RAFT failure, and the observer
       * answers a failed cuVS search with a CPU VSAG fallback.  Clamp instead of
       * failing.  For every topk <= 128 this leaves the shipped behaviour
       * byte-for-byte unchanged (topk*4 <= 512 there). */
      if (required_itopk > 512) required_itopk = 512;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    V2_T0();
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    V2_T1("cagra_search", topk, nq, "cuvsCagraSearch");
    V2_T0();
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    V2_T1("d2h_ids", sizeof(unsigned)*nq*topk, nq, "cudaMemcpy D2H ids");
    if (out_dist) {
      V2_T0();
      cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
      V2_T1("d2h_dist", sizeof(float)*nq*topk, nq, "cudaMemcpy D2H dist");
    }
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  V2_T0();
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  V2_T1("rmm_free", topk, nq, "3x cuvsRMMFree");
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  V2_T0();
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  V2_T1("cagra_serialize", index->n, index->dim, "cuvsCagraSerialize include_dataset=true");
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  V2_T0();
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  V2_T1("cagra_deserialize", n, dim, "cuvsCagraDeserialize");
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
