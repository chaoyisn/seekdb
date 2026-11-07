#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
OUTPUT=${1:-$SCRIPT_DIR/libseekdb_cuvs_bridge.so}

[[ -x "$HIPCC" ]] || { echo "hipcc is not executable: $HIPCC" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/core/c_api.h" ]] || {
  echo "hipVS headers are missing under $HIPVS_ROOT/include" >&2
  exit 2
}
[[ -f "$HIPVS_ROOT/lib/libcuvs_c.so" ]] || {
  echo "libcuvs_c.so is missing under $HIPVS_ROOT/lib" >&2
  exit 2
}

mkdir -p "$(dirname "$OUTPUT")"

# [V2] Optional extra compile flags, used only by the instrumented build
# (e.g. EXTRA_CFLAGS="-DOB_V2_PROFILE").
EXTRA_CFLAGS=${EXTRA_CFLAGS:-}

# [V5-P4-S2] Vendored raft headers (third_party/, pinned per MANIFEST): the
# prune stage calls the frozen library's public cagra::helpers::optimize, and
# the hipVS install ships no raft headers of its own. GEN holds build-time
# generated headers vendored verbatim.
RAFT_INC="$SCRIPT_DIR/third_party/raft-include"
RAFT_GEN="$SCRIPT_DIR/third_party/raft-gen-include"
LOGGER_INC="$SCRIPT_DIR/third_party/rapids_logger-include"
for d in "$RAFT_INC" "$RAFT_GEN" "$LOGGER_INC"; do
  [[ -d "$d" ]] || { echo "vendored raft include missing: $d" >&2; exit 2; }
done

# Enable S2 here because this script also links the refine and prune objects.
"$HIPCC" \
  -fPIC \
  -O2 \
  -D__HIP_PLATFORM_AMD__ \
  -DSEEKDB_CUVS_ENABLE_S2 \
  $EXTRA_CFLAGS \
  -I"$SCRIPT_DIR" \
  -I"$HIPVS_ROOT/include" \
  -I"$ROCM_ROOT/include" \
  "$SCRIPT_DIR/seekdb_cuvs_bridge.c" \
  -c \
  -o "$SCRIPT_DIR/.bridge.o.tmp"

"$HIPCC" \
  -fPIC \
  -O2 \
  -std=c++17 \
  -D__HIP_PLATFORM_AMD__ \
  $EXTRA_CFLAGS \
  -I"$SCRIPT_DIR" \
  -I"$HIPVS_ROOT/include" \
  -I"$ROCM_ROOT/include" \
  "$SCRIPT_DIR/seekdb_refine.hip" \
  -c \
  -o "$SCRIPT_DIR/.refine.o.tmp"

"$HIPCC" \
  -fPIC \
  -O2 \
  -std=c++17 \
  -D__HIP_PLATFORM_AMD__ \
  $EXTRA_CFLAGS \
  -I"$SCRIPT_DIR" \
  -I"$HIPVS_ROOT/include" \
  -I"$RAFT_INC" \
  -I"$RAFT_GEN" \
  -I"$LOGGER_INC" \
  -I"$ROCM_ROOT/include" \
  "$SCRIPT_DIR/seekdb_prune.cpp" \
  -c \
  -o "$SCRIPT_DIR/.prune.o.tmp"

"$HIPCC" -shared -o "$OUTPUT" \
  "$SCRIPT_DIR/.bridge.o.tmp" \
  "$SCRIPT_DIR/.refine.o.tmp" "$SCRIPT_DIR/.prune.o.tmp" \
  -L"$HIPVS_ROOT/lib" \
  -Wl,-soname,libseekdb_cuvs_bridge.so \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  -lcuvs_c -lcuvs

rm -f "$SCRIPT_DIR/.refine.o.tmp" "$SCRIPT_DIR/.prune.o.tmp"

echo "built $OUTPUT"
