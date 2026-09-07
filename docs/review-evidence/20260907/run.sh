#!/bin/bash
# Diagnostic reproductions: success means the documented defects are present.
set -euo pipefail
review_dir="$(cd "$(dirname "$0")" && pwd)"
cd "$review_dir/../../.."
review_build="$(mktemp -d /private/tmp/jk-flow-review-rerun.XXXXXX)"
review_idf="${IDF_PATH:-/Users/dw/esp/esp-idf}"
common=(-DJK_ENABLE_WRITES=1 -fsanitize=address,undefined -g -Wall -Wextra
  -Wno-unused-parameter -pthread -ffunction-sections -Wl,-dead_strip
  -I tools/host_stubs -I node_a/main -I components/common/include
  -I components/jk_proto/include)
cc "${common[@]}" -I "$review_idf/components/json/cJSON" \
  "$review_dir/arbiter_repro.c" node_a/main/state_cache.c \
  node_a/main/command_validation.c components/jk_proto/jk_proto.c \
  "$review_idf/components/json/cJSON/cJSON.c" -o "$review_build/arbiter_repro"
cc "${common[@]}" -I components/net_util/include -I test_board/main \
  "$review_dir/ble_repro.c" components/jk_proto/jk_proto.c \
  test_board/main/synth_frames.c -o "$review_build/ble_repro"
cc "${common[@]}" -I components/net_util/include -I components/ota/include \
  "$review_dir/supervisor_repro.c" node_a/main/state_cache.c \
  -o "$review_build/supervisor_repro"
cc -fsanitize=address,undefined -g -Wall -Wextra -pthread \
  -I tools/host_stubs -I components/common/include -I node_b/main \
  "$review_dir/replay_repro.c" -o "$review_build/replay_repro"
for repro in arbiter ble supervisor replay; do
  "$review_build/${repro}_repro"
done
