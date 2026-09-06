#!/bin/bash
# Native Mac tests using the repository's pinned ESP-IDF cJSON, no installs.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /private/tmp/jk-host-tests.XXXXXX)
idf_root=${IDF_PATH:-/Users/dw/esp/esp-idf}
json_dir="$idf_root/components/json/cJSON"
if [[ ! -f "$json_dir/cJSON.c" ]]; then
    echo "Set IDF_PATH to the installed ESP-IDF tree (cJSON source required)." >&2
    exit 1
fi
flags=(-DJK_ENABLE_WRITES=1 -fsanitize=address,undefined -g -Wall -Wextra
       -I components/common/include -I components/jk_proto/include -I test_board/main)
cc "${flags[@]}" tools/host_test_jk_proto.c components/jk_proto/jk_proto.c \
   test_board/main/synth_frames.c -o "$test_dir/proto"
"$test_dir/proto"
cc "${flags[@]}" tools/host_test_reasm.c components/jk_proto/jk_proto.c \
   test_board/main/synth_frames.c -o "$test_dir/reasm"
"$test_dir/reasm"
cc "${flags[@]}" -I "$json_dir" -I node_a/main tools/host_test_inputs.c \
   node_a/main/command_validation.c "$json_dir/cJSON.c" \
   components/jk_proto/jk_proto.c test_board/main/synth_frames.c -o "$test_dir/inputs"
"$test_dir/inputs"
cc "${flags[@]}" -Wno-unused-parameter -pthread -I tools/host_stubs -I node_a/main \
   -I "$json_dir" tools/host_test_app_edges.c node_a/main/state_cache.c \
   node_a/main/command_validation.c components/jk_proto/jk_proto.c \
   "$json_dir/cJSON.c" -o "$test_dir/app_edges"
"$test_dir/app_edges"
cc "${flags[@]}" -pthread -I tools/host_stubs -I node_a/main \
   tools/host_test_state.c node_a/main/state_cache.c -o "$test_dir/state"
"$test_dir/state"
# ThreadSanitizer and ASAN cannot be combined: run the same concurrent tests
# separately so incorrect locking cannot hide behind passing value assertions.
cc -fsanitize=thread -g -Wall -Wextra -pthread -I tools/host_stubs \
   -I components/common/include -I components/jk_proto/include -I node_a/main \
   tools/host_test_state.c node_a/main/state_cache.c -o "$test_dir/state_tsan"
"$test_dir/state_tsan"
cc "${flags[@]}" -Wno-unused-parameter -pthread -ffunction-sections -Wl,-dead_strip \
   -I tools/host_stubs -I node_a/main -I components/net_util/include -I components/ota/include \
   tools/host_test_verify.c node_a/main/state_cache.c -o "$test_dir/verify"
"$test_dir/verify"
cc "${flags[@]}" -Wno-unused-parameter -ffunction-sections -Wl,-dead_strip \
   -I tools/host_stubs -I node_a/main -I components/net_util/include \
   tools/host_test_discovery.c components/jk_proto/jk_proto.c \
   test_board/main/synth_frames.c -o "$test_dir/discovery"
"$test_dir/discovery"
cc -DJK_ENABLE_WRITES=1 -fsanitize=thread -g -Wall -Wextra -Wno-unused-parameter -pthread \
   -ffunction-sections -Wl,-dead_strip -I tools/host_stubs -I node_a/main \
   -I components/common/include -I components/jk_proto/include \
   -I components/net_util/include -I test_board/main tools/host_test_discovery.c \
   components/jk_proto/jk_proto.c test_board/main/synth_frames.c -o "$test_dir/discovery_tsan"
"$test_dir/discovery_tsan"
python3 -B -m unittest discover -s tools
cc "${flags[@]}" -Wno-unused-parameter -ffunction-sections -Wl,-dead_strip \
   -I tools/host_stubs -I node_b/main tools/host_test_b_notify.c -o "$test_dir/b_notify"
"$test_dir/b_notify"
cc "${flags[@]}" -pthread -I tools/host_stubs -I node_b/main \
   tools/host_test_b_cache.c -o "$test_dir/b_cache"
"$test_dir/b_cache"
echo "Test executables preserved at $test_dir"
