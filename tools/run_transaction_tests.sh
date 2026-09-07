#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
txn_dir=$(mktemp -d /private/tmp/jk-transaction-tests.XXXXXX)
for sanitizer in address,undefined thread; do
  cc -DJK_ENABLE_WRITES=1 -fsanitize="$sanitizer" -g -Wall -Wextra \
    -Wno-unused-parameter -pthread -ffunction-sections -Wl,-dead_strip \
    -I tools/host_stubs -I node_a/main -I components/common/include \
    -I components/jk_proto/include -I components/net_util/include -I test_board/main \
    tools/host_test_transactions.c components/jk_proto/jk_proto.c \
    test_board/main/synth_frames.c -o "$txn_dir/transactions-$sanitizer"
  "$txn_dir/transactions-$sanitizer"
done
echo "Transaction test executables preserved at $txn_dir"
