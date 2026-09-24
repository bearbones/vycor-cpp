# H — Index write integrity

## Outcome

No sequence of concurrent runs, full disks, or failed bakes can publish a
torn, mixed, or empty index or checkpoint journal, and a corrupted file is
detected on load instead of answering queries with wrong data.

## Evidence and starting points

- `SnapshotIO::save` (`src/callgraph/Snapshot.cpp:631-665`) always writes to
  `path + ".tmp"`, with no lock and no `fsync` before the rename. Two
  `megascope index` runs, or `serve` starting up (it re-saves) next to an
  `index`, open the same temp file with truncation; their writes interleave
  and one rename publishes the mixture. The header has no checksum, and the
  bounds-checked reader (`Snapshot.cpp:69-123`) accepts the result.
- `bakeIsolated` (`src/callgraph/WorkerPool.cpp:378-384`) returns an empty
  `BakedIndexes` when it cannot create the shard directory (full or
  read-only temp dir). `main.cpp` saves it over the existing index. anneal
  falls back to in-process parsing for the same failure
  (`src/anneal/Analyzer.cpp:1330-1337`); megascope does not.
- `AnnealCheckpoint::loadRecords` (`src/anneal/Checkpoint.cpp:662-673`) stops
  at the first bad record, then the journal is reopened with `OF_Append`
  (`:640-642`). New records land after the garbage, so every later resume
  stops at the same point and loses all progress since, including the attempt
  records poison detection depends on. `tests/test_anneal_checkpoint.cpp:251`
  resumes only once, so it does not catch this.
- `stream.need(len + 4)` (`Checkpoint.cpp:667`, `:903`) is computed in
  `uint32_t` and wraps for `len >= 0xFFFFFFFC`; the following `pos += len`
  reads out of bounds on a corrupt journal or shard.
- Write errors: `Snapshot.cpp:658-660` and `Checkpoint.cpp:878-881` return
  `false` without `clear_error()`. LLVM's `raw_fd_ostream` destructor reports a
  fatal error on an uncleared error (documented behavior; not verified against
  source on the review host), so a full disk aborts instead of failing
  cleanly. `appendRecord` (`Checkpoint.cpp:789`) never checks. The `.tmp` file
  is left behind on failure.

## Work

1. Reproduce first: a test that runs two saves to the same path concurrently
   and shows a mixed result; a test that fails shard-directory creation and
   shows the index emptied; a checkpoint test that corrupts a record, resumes
   twice, and shows progress lost.
2. Save through a unique temp file in the target directory
   (`createUniqueFile`), `fsync` the file, rename, then `fsync` the directory.
   Remove the temp file on every failure path.
3. Take an advisory lock on `<index>.lock` for the whole
   load-dirty-check → bake → save sequence of `index` and `serve` startup. A
   second writer waits with a message, or fails fast with `--no-wait`. Readers
   do not lock (rename is atomic for them).
4. Add a CRC32C per section to the header table; verify on load for every
   section decoded. A mismatch is a load error naming the section, and
   `megascope info` reports it. Allocate the next format version at merge
   (coordinate with M).
5. Refuse to save a bake that produced nothing from a non-empty selection.
   Make `bakeIsolated` fall back to in-process like anneal, or return an
   error the caller surfaces; never an empty success.
6. Checkpoint journal: record the offset of the last valid record while
   loading and truncate the file to it before appending. Fix the `need`
   overflow by widening to `size_t` (both call sites). Check every write and
   clear the stream error before returning.

## Ownership and boundaries

Own the save/load framing in `Snapshot.cpp`, the journal framing in
`Checkpoint.cpp`, and the empty-bake guard. I owns worker spawning and
timeouts; this package only decides what a failed bake may overwrite. Do
not change section contents (M owns the context layout).

## Acceptance

- The reproducing tests from step 1 fail before and pass after.
- A flipped byte in any section is rejected on load with the section named;
  a truncated file is rejected; round trips and read-only partial loads
  still pass.
- A simulated full disk (write to a size-limited file or an injected error)
  returns an error, leaves the previous index intact, and leaves no temp file.
- Load-time cost of checksum verification is measured on the largest
  available index and reported; if material, verify lazily per section.
- CLI goldens unchanged; supported LLVM matrix passes.

## Deliverables

Atomic, locked, checksummed saves; journal truncation on resume; the
reproducing tests; the format-version note for `docs/index-provenance.md`.
