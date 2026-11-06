/* seekdb_refine.h — [V5-P4-S2] custom GPU graph-refine stage, C interface.
 *
 * Pipeline position: official cuvsNNDescentBuild (gd=256) -> THIS module
 * (exact fp32 re-rank over 1-hop-expanded candidates, in-repo HIP kernels,
 * rule-compliant custom GPU build) -> official cagra::helpers::optimize prune
 * 256->128 -> cuvsCagraIndexFromArgs injection. Query/serialize/deserialize
 * stay on the official fixed path.
 */
#ifndef SEEKDB_REFINE_H
#define SEEKDB_REFINE_H

#include <cuvs/core/c_api.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Graph width handed to and returned from the refine round. Must match the
 * GNND stage's params.graph_degree and the prune stage's input width. */
#define SEEKDB_REFINE_GRAPH_DEGREE 256

/* One refine round: in_graph[n][256] -> out_graph[n][256], both host buffers
 * (pinned recommended), rows sorted ascending by exact fp32 L2 over the
 * candidate set (0-hop kept with priority + 1-hop expansion of the first 32
 * columns). No CPU substitute for any GPU step. Returns 0 on success. */
int seekdb_refine_round(cuvsResources_t res, const float* base, long n,
                        long dim, const uint32_t* in_graph,
                        uint32_t* out_graph);

/* Official prune (cagra::helpers::optimize) 256 -> 128, host buffers.
 * Semantics identical to the frozen baseline's internal optimize step
 * (guarantee_connectivity=false). Returns 0 on success. */
int seekdb_prune256to128(uint32_t* in_graph, long n, uint32_t* out_graph);

#ifdef __cplusplus
}
#endif

#endif /* SEEKDB_REFINE_H */
