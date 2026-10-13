# ORZ format

.orz is a lossless compression format to store osu replay frames.

Replay set: https://drive.google.com/file/d/1i2oe929loEzhvCfRYG-DeZZQ_YouaBYD/view

## LZMA SDK and reconstruction

The vendored C/C++ encoder and decoder are LZMA SDK **4.57**. Both OSR
reconstruction and ORZ's compressed internal streams use this SDK. The ORZ
container version and LZMA stream format are unchanged; existing ORZ files
remain readable. Encoder versions can produce different compressed bytes and
compression ratios for the same text.

API callers can pass `DECODE_OSU_STABLE_LZMA` to `orz_to_osr` to reconstruct
historical osu!stable payloads using a 2 MiB dictionary, lc=3, lp=0, pb=2,
normal mode, BT4, fastBytes=255, one thread, and no end marker. The API and CLI
retain fast reconstruction as their default. `ENCODE_CHECK_ROUND_TRIP` is a
separate, opt-in frame-text check; it remains disabled by default.

Stable settings reproduce payloads encoded with the matching historical
encoder and configuration. They do not guarantee byte identity for every OSR:
compressed-payload suffixes are not preserved by the structured representation,
and an original encoder may have used other settings. Compare the complete
original and recovered files when testing byte-identical recovery.

### Build and test

The Visual Studio solution builds the SDK into `orz` without external libraries.
CMake also builds the CLI and regression tests on x86/x64:

```sh
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

On non-x86 hosts, use `-DORZ_BUILD_REPLAY_TOOLS=OFF` to build and test only the
LZMA adapter. The replay codec still requires x86 intrinsics. See
[SDK provenance and license](Lib/README.md) and [test fixtures](tests/fixtures/README.md).

## Reference compression results

The figures below are from the published replay set using SDK 21.02.

# osu!
31.681% avg

226602464 -> 73102000

# taiko
52.790% avg

2794427 -> 1483584

# mania
51.211% avg

14573725 -> 7448010

# fruit
46.620% avg

10419674 -> 4895937

# Total
34.1717174032% of the original size

254.39029 MB -> 86.929531 MB