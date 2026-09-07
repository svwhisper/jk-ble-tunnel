# Flow-control review evidence

These four hardware-free harnesses reproduce eleven undesirable behaviors in
production code at `20d8ca1`. They reuse the existing host adapters, exposing
static production functions through the adapters' source includes. No network,
USB, firmware deployment or BMS operation is performed.

**Passing means the defect was reproduced, not that the implementation is safe.**
These are focused counterexamples, not an end-to-end phone/radio simulation.
Some scenarios deliberately inject an event or queue state to isolate the
missing invariant; they do not establish its frequency on hardware.

Run on macOS with the existing compiler and pinned ESP-IDF checkout:

```sh
bash docs/review-evidence/20260907/run.sh
```

`IDF_PATH` may override `/Users/dw/esp/esp-idf`. Binaries and sanitizer artifacts
go into a fresh temporary directory. `results.txt` and `build-stderr.txt` retain
the review run's output. ASAN and UBSAN were enabled; legacy renamed-main and
cJSON compiler warnings are not sanitizer failures. No packages were installed.

After remediation, convert the affected reproductions to assert the desired
invariant instead of continuing to expect the defective behavior.
