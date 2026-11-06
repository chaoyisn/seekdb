/* seekdb_prune.cpp — [V5-P4-S2] official prune entry for the custom pipeline.
 *
 * Calls the frozen library's public cagra::helpers::optimize (host knn_graph
 * [n x 256] -> host new_graph [n x 128]) — the same optimize step the frozen
 * baseline runs internally after GNND (guarantee_connectivity=false). The
 * raft headers come from the vendored copy under third_party/ (pinned to the
 * hipVS fork commit recorded in third_party/MANIFEST) because the hipVS
 * install ships no raft headers of its own.
 */
#include <raft/core/host_mdspan.hpp>
#include <raft/core/resources.hpp>
#include <cuvs/neighbors/cagra_optimize.hpp>

#include "seekdb_refine.h"

int seekdb_prune256to128(uint32_t* in_graph, long n, uint32_t* out_graph)
{
  if (!in_graph || !out_graph || n <= 0) return 1;
  try {
    raft::resources res;
    auto in_v =
      raft::make_host_matrix_view<uint32_t, int64_t>(in_graph, n, SEEKDB_REFINE_GRAPH_DEGREE);
    auto out_v =
      raft::make_host_matrix_view<uint32_t, int64_t>(out_graph, n, 128);
    cuvs::neighbors::cagra::helpers::optimize(res, in_v, out_v);
  } catch (const std::exception& e) {
    fprintf(stderr, "[seekdb_refine] prune threw: %s\n", e.what());
    return 2;
  } catch (...) {
    fprintf(stderr, "[seekdb_refine] prune threw unknown exception\n");
    return 2;
  }
  return 0;
}
