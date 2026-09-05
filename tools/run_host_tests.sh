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
cc "${flags[@]}" -I "$json_dir" -I node_a/main tools/host_test_inputs.c \
   node_a/main/command_validation.c "$json_dir/cJSON.c" \
   components/jk_proto/jk_proto.c test_board/main/synth_frames.c -o "$test_dir/inputs"
"$test_dir/inputs"
python3 -B -m unittest discover -s tools
echo "Test executables preserved at $test_dir"
