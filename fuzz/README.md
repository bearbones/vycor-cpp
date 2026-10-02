# Fuzz targets

libFuzzer targets for the parsers that read bytes vycor-cpp did not just
write itself: index files on disk, the anneal checkpoint journal, the files
anneal's isolated workers pass to their parent, and `megascope batch`'s
request stream. Built only with `-DVYCOR_FUZZ=ON` under clang, which also
turns on `VYCOR_SANITIZE=address,undefined` unless another sanitizer set
is given (`thread` is refused).

| Target | Parser | What an accepted input is put through |
|---|---|---|
| `fuzz_snapshot` | `SnapshotIO::load` (`src/callgraph/Snapshot.cpp`, the mapped control-flow section in `ControlFlowIndex.cpp`) | Mutable: absorbed into fresh indexes (the isolated bake's shard merge), its TUs removed, saved and reloaded (a warm refresh). ReadOnly: every registered tool, and the `dump` walk over every context and channel site |
| `fuzz_checkpoint` | `AnnealCheckpoint::open` (`src/anneal/Checkpoint.cpp`) | every TU a record names replayed the way a resumed `runAnalysis` does, the index-only checks over the replayed index, then an append and a reopen |
| `fuzz_shard` | `readAnnealIndexShard`, `readAnnealDiagShard`, `readGlobalIndexFile` | payloads applied to a `GlobalIndex` and the index-only checks run; written back out through the shard writers |
| `fuzz_batch` | `runBatch` (`src/cli/MegascopeCli.cpp`) | answered against an `examples/deep_chains` index loaded ReadOnly; every input is also held to the batch contract (one JSON response with `status` and `exit` per request line) |

Input formats: `fuzz_snapshot`, `fuzz_checkpoint` and `fuzz_shard` take one
flags byte before the file image. Its reseal bit (snapshot `0x01`,
checkpoint `0x01`, shard `0x04`) makes the harness recompute the format's
checksums before writing the file, so mutations reach the decoders behind
them; with the bit clear the checksum checks themselves are fuzzed. The
checkpoint harness also takes the journal's options fingerprint from the
image, so a mutated header is not simply discarded. `fuzz_shard` picks the
reader by the image's magic (flags bits 0-1 when the magic is none of the
three). `fuzz_batch` takes the raw request stream.

## Build, seed, run

```bash
cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DVYCOR_FUZZ=ON -DVYCOR_BUILD_TESTS=OFF
cmake --build build-fuzz --target fuzz

# Seed corpora from the test fixtures, at the current on-disk formats
# (regenerate after a format change; nothing here is checked in).
build-fuzz/fuzz/fuzz_seeds fuzz-seeds

# One target, ten minutes; new inputs accumulate in fuzz-corpus/<target>,
# a failing input is written to fuzz-artifacts/.
mkdir -p fuzz-corpus/snapshot fuzz-artifacts
build-fuzz/fuzz/fuzz_snapshot fuzz-corpus/snapshot fuzz-seeds/snapshot \
  -max_total_time=600 -timeout=30 -rss_limit_mb=4096 \
  -artifact_prefix=fuzz-artifacts/snapshot-

# fuzz_batch: point it at the seeded index (otherwise it bakes one at
# startup) and give it the dictionary.
VYCOR_FUZZ_INDEX=$PWD/fuzz-seeds/batch.vycs \
  build-fuzz/fuzz/fuzz_batch fuzz-corpus/batch fuzz-seeds/batch \
  -dict=fuzz-seeds/batch.dict -max_len=2048 -max_total_time=600 \
  -timeout=30 -rss_limit_mb=4096 -artifact_prefix=fuzz-artifacts/batch-

# Reproduce a finding: run the target on the saved input.
build-fuzz/fuzz/fuzz_snapshot fuzz-artifacts/snapshot-crash-<sha1>
```

The libclang_rt fuzzer and sanitizer runtimes must be installed for the
clang in use (Debian/Ubuntu: `libclang-rt-<N>-dev`).

## In CI

`.github/workflows/fuzz.yml`: on a pull request, each target whose parser
the change touches (the path lists in the workflow) runs for 60 seconds;
nightly, every target runs for 20 minutes and the grown corpora are kept in
the Actions cache. A failing input fails the job and is uploaded as an
artifact.

## Findings

Every finding is fixed with a unit test that reproduces it without the
fuzzer (it fails before the fix):

| Target | Finding | Regression test |
|---|---|---|
| `fuzz_snapshot` | a count read after a failed id check skipped its bound and sized an allocation (out of memory) | `test_snapshot.cpp` "a count read after a bad id cannot size an allocation" |
| `fuzz_snapshot` | a string that is not UTF-8 reached `llvm::json` (assertion); real Latin-1 paths and source text do the same. Fixed by storing index text (`callgraph/Utf8.h`, an exact escape; a first fix that replaced bytes with U+FFFD merged distinct paths and USRs) | `test_utf8_strings.cpp`; ctest `utf8_paths` (`scripts/utf8-paths-check.py`) for the CLI |
| `fuzz_snapshot` | a channel site's refcount was merged one `addSite` per count (timeout), and a multi-TU site lost all but its first TU | `test_channel_index.cpp` "ChannelIndex absorb replays each registration under its own TU", "a loaded channel site's refcount is checked and merged in one step" |
| `fuzz_checkpoint` | a diagnostic kind outside `Diagnostic::Kind` was cast into the enum (UBSan) | `test_anneal_checkpoint.cpp` "A diagnostic kind outside the enum is refused and not loaded" |
