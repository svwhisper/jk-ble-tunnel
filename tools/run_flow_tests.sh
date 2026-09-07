#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
flow_dir=$(mktemp -d /private/tmp/jk-flow-tests.XXXXXX)
flow_idf=${IDF_PATH:-/Users/dw/esp/esp-idf}
# R0 explicitly records defects; each isolated R1 checkpoint removes its define.
flow_expectations=(-DFLOW_LEGACY_FIFO)
cc -DJK_ENABLE_WRITES=1 "${flow_expectations[@]}" \
  -fsanitize=address,undefined -g -Wall -Wextra -Wno-unused-parameter -pthread \
  -I tools/host_stubs -I node_a/main -I components/common/include \
  -I components/jk_proto/include -I "$flow_idf/components/json/cJSON" \
  tools/host_test_flow.c node_a/main/state_cache.c node_a/main/command_validation.c \
  components/jk_proto/jk_proto.c "$flow_idf/components/json/cJSON/cJSON.c" \
  -o "$flow_dir/flow"
"$flow_dir/flow"
echo "Flow test executable preserved at $flow_dir"
