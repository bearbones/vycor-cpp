# Index freshness and provenance

Status: implemented on `main` (snapshot format v10). Owner: package A of
`docs/plans/2026-09-next/`. This page is the contract the warm start
keeps and the interface the shared result contract (package C) and the
selective control-flow storage (package F) build on.

Modules: `include/vycor/callgraph/InputFingerprint.h` (fingerprints),
`include/vycor/callgraph/TuOutcome.h` (per-TU outcomes),
`include/vycor/callgraph/Snapshot.h` (`SnapshotMeta`, `IndexProvenance`,
`IndexCoverage`, `SnapshotIO::dirtyTUs`, `markUnstableStamps`,
`recordOutcomes`, `outcomesOf`, `coverageOf`), the bake in
`src/callgraph/ControlFlowContextVisitor.cpp` and
`src/callgraph/WorkerPool.cpp` (outcome production), and the warm start
in `src/main.cpp`.

## The problem this fixes

Before v10 a warm `megascope index` re-parsed a TU only when its own
mtime/size stamp or the stamp of a file its parse had opened changed.
Everything else that decides what a parse produces was invisible: the
compile command, the working directory, `--extra-arg`, the sysroot, the
PCH cache, the toolchain, the analyzer itself. Changing a compile command
from no definition to `-DNEW_TARGET` reported zero refreshed TUs and kept
the old callee (2026-09-08). And a TU whose parse failed was cached like a
healthy one: nothing recorded that its facts were partial or absent, and
nothing ever retried it.

## Contract

A TU selected for a warm start is **re-indexed** when any of these holds,
checked in this order (each TU is counted once, under the first reason,
in `SnapshotIO::DirtyReport`):

1. it is not recorded in the index, or its own stamp (mtime, size)
   differs from the recorded one, or the recorded stamp is unknown
   (see *Unstable stamps*);
2. its recorded effective-input fingerprint differs from the one
   computed now (`viaInputs`);
3. a file its parse opened has a different stamp (`viaDeps`);
4. its last recorded parse did not end `Indexed` (`retried`).

A TU recorded in the index but no longer selected is dropped. Selection
semantics are unchanged: `--source`/`--source-list` union, `--source-re`
and `--skip-paths` narrow, no selection flags means the recorded set.
`--force` rebuilds regardless.

Two whole-index rules come first:

- **Configuration mismatch** (collapse paths, lock/channel types) still
  rebuilds everything, as before.
- **Environment mismatch**: when the recorded environment fingerprint
  differs from the current one, every TU's fingerprint differs, so the
  warm start says why (`bake environment differs (index: <analyzer>,
  <toolchain>; now: ...)`) and rebuilds.

Past half the selection **changed** (reasons 1 to 3), the cold bake runs
instead of the incremental path, as before. Retries (reason 4) do not
count toward that threshold: a project whose broken TUs outnumber its
healthy ones re-parses the broken ones, never the healthy ones.

Retries are also the one reason the warm start may decline: when
nothing changed and nothing is dropped, the failed TUs are left as
recorded and the refresh stays meta-only (`N TU(s) whose last parse
failed are left as recorded (pass --retry-failed to re-parse them)`).
See *Per-TU outcomes and retry* for why.

The refreshed index must equal a clean rebuild. `scripts/warm-refresh-check.py`
(ctest `warm_refresh`) enforces that for a TU edit, a header edit, a
compile-flag change, a working-directory change, an environment change,
a selection change, a failed-then-recovered TU, and a chronically failing
majority, each in-process and under `--isolate-workers`, by comparing
`megascope dump` of the refreshed index against `--force` and the
`info` coverage of both.

## Effective-input fingerprint

`fingerprintTUs(compDb, files, envFingerprint)` returns one opaque hex
SHA-1 digest per selected TU. Compare for equality; never parse.

Two layers:

**Bake environment** (`environmentFingerprint(BakeEnvironment)`), one per
bake, recorded as `IndexProvenance::environment`:

| Field | Source |
|---|---|
| schema constant | changes when the hashed field set changes |
| analyzer identity | `vycor-cpp <version>, index format <N>` (`analyzerIdentity()`) |
| toolchain identity | `LLVM <version>` (`toolchainIdentity()`) |
| compiled-in resource directory | `VYCOR_CLANG_RESOURCE_DIR` |
| compiled-in default sysroot | `VYCOR_DEFAULT_SYSROOT` |
| detected GCC installation | `detectUsableGccInstallDir()` (the dir the tool would inject) |
| `--sysroot` | as given (empty = compiled default) |
| `--extra-arg` list | every argument, in order, exactly as given (`VYCOR_EXTRA_ARGS` included) |
| `--pch-dir` | as given (empty = off) |

**Per TU**: the environment fingerprint plus every `CompileCommand` the
database returns for the file, in database order, each as its working
directory, file name, output, and argument list verbatim. Argument order
matters. A file with several compile commands is one TU with several
inputs: the variants are hashed in sequence and never merged, so adding,
removing, or reordering a variant changes the fingerprint. A file the
database has no command for hashes to the environment alone.

Every field is length-prefixed before hashing, so `-DX=1 -DY` as one
argument and as two are different inputs, and an empty trailing argument
is not the same as none.

### Ambient inputs: what is covered and what needs explicit invalidation

Covered without any action (a change re-indexes the affected TUs, or
rebuilds when it is bake-wide):

- source and header edits, including system headers, because the bake
  records every file the frontend opened with the frontend's own stat,
  spelled by the real path the file system resolved when it opened the
  file (not a lexical `..` removal, which crosses symlinks wrongly: a
  toolchain found through `/../lib/gcc` on a merged-`/usr` system spells
  its headers as `/include/c++/N` on paper and `/usr/include/c++/N` on
  disk, and a dependency recorded on paper is never found again, so
  every warm start rebuilds cold);
- the compile command, working directory, extra args, sysroot, PCH
  directory, GCC installation, LLVM version, vycor-cpp version, index
  format;
- collapse paths and lock/channel types (configuration match);
- the set of selected TUs.

Not covered, `--force` (or a version bump) is required:

- a rebuilt `vycor-cpp` with the same version string whose analysis
  changed; the analyzer identity is the version, not a binary hash;
- environment variables the Clang driver reads on its own (`CPATH`,
  `C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`, `SDKROOT` when no sysroot is
  given), and anything a compiler wrapper named in the compile command
  would do; the command line is hashed, the wrapper's behavior is not;
- the contents of a PCH file: its directory is part of the fingerprint
  and a PCH the frontend opens is a recorded dependency, but a PCH
  regenerated into the same path with the same stamp is not seen;
- organization extension registrars compiled into the binary
  (`ext/*.cpp`) beyond the lock/channel types they register, which the
  configuration match does cover.

Content hashing was deliberately not used for freshness: hashing the
938-TU LLVM testbed's 1,768 recorded dependencies on every refresh would
cost more than the stat-based check by orders of magnitude, and the
dependency list already includes every opened file. The fingerprint
hashes the *inputs that are not files* (a few KB per TU) instead, and the
measured cost is in *Measurements* below.

## Unstable stamps: same-size edits within the stamp resolution

Stamps are nanosecond mtime plus size, but the frontend's stat of an
opened dependency is recorded at one-second resolution, and a file system
may store coarser mtimes than it reports. A file edited to the same size
within the same second as the parse that read it would look unchanged.

The strategy is git's "racy" rule. Every bake records its start time
(`IndexProvenance::bakeStartNs`). At save, `SnapshotIO::markUnstableStamps`
finds every TU and dependency whose mtime is at or after the floor-second
of that start and records its mtime as 0 (unknown). An unknown stamp
never matches, so that file's TUs are re-indexed once on the next warm
start, which then records a real stamp. The cost is one extra parse for
files written during a bake; the benefit is that no edit made while (or
just before) a bake ran can be lost to timestamp resolution. The
`unstable_stamps` count in `--stats-json` says how many were marked.

This does not claim content correctness from timestamps: an edit that
lands after the bake with an older-than-bake mtime (a `touch -d`, a
restore from backup, `git checkout` of a file with a preserved time) is
only seen if the size differs. That is the same limit build systems
accept. The converse costs time, not correctness: a file whose mtime
is ahead of the bake's clock (clock skew on a network file system) is
marked unstable at every save and re-indexed on every refresh that
rewrites the index.

## Per-TU outcomes and retry

`TuOutcome {status, detail}` per requested TU, `TuStatus`:

| Status | Meaning | Produced by |
|---|---|---|
| `indexed` | clean parse: facts complete | `ClangTool::run` returned 0 |
| `partial` | parse reported errors: facts from a partial AST | run returned 1 (`detail` = `parse errors`) |
| `crashed` | the in-process crash guard fired: no facts (the TU's partial facts are discarded) | signal in the guard (`detail` = `signal N`) |
| `poisoned` | its worker died under worker isolation: no facts | worker poison marker (`detail` = `worker crashed`) |
| `timeout` | its worker made no progress for `--worker-timeout` seconds and was killed: no facts | worker poison marker after the kill (`detail` = `worker timed out`) |
| `skipped` | never parsed | run returned 2 (`no compile command`), or nothing reported (`no outcome recorded`) |

Outcomes travel every bake path: the serial and pooled in-process bakes
report per TU through the bake's outcome sink, isolated workers record
them in their shard meta and the parent absorbs them with the shard,
poisoned TUs are set by the dispatcher, and the incremental path merges
the fresh outcomes over the loaded ones for the re-indexed set.
`SnapshotIO::recordOutcomes` fills the meta column parallel to `files`;
a requested TU with no reported outcome is recorded `skipped`, never
`indexed`. **A failed parse is never a healthy cache entry.**

Retry policy: a TU whose recorded status is not `indexed` is dirty
(reason 4), and is re-parsed

- alongside any refresh that rewrites the index anyway, i.e. one that
  re-indexes at least one changed TU or drops one; the retry then costs
  its own parse and nothing else;
- on `--retry-failed`, which forces the retries even when nothing else
  changed;
- on `--force`, which rebuilds everything.

A refresh that would otherwise touch nothing leaves failed TUs as
recorded and stays meta-only. The reason is cost: the incremental path
pays the full mutable load and save of the index for any re-indexed
set, and on the 938-TU LLVM testbed that is about 7 s (2.3 s load,
1.1 s save, plus the parses) against 0.03 s for the meta-only check. A
project with one chronically broken TU must not pay that on every
refresh. The first policy tried, retry on every warm start like a
failed edge in a build system, did exactly that; the measurements
below record both.

A missing header that later appears changes no recorded stamp (the
header never existed to be recorded), so the recovery needs a
piggybacked retry or `--retry-failed`. `coverage.complete == false` in
`info` or the `index` summary is the signal an orchestrator can act on.
The warning line names the count (`K of N TU(s) indexed cleanly (P
partial, F failed); the rest are retried with the next refresh that
rewrites the index, or --retry-failed`). To stop paying for chronic
failures, fix the inputs (`--extra-arg`) or narrow the selection.

The ephemeral query mode (`--source ...` without an index) prints the
same coverage warning so a partial in-memory bake is visible, but it
persists nothing.

`megascope serve`'s `reindex_tu` re-parses one TU in memory; it does not
update the fingerprints or outcomes of a saved index (serve does not save).

## Provenance and coverage: the interface for package C

C++ (all in `Snapshot.h`, immutable value types):

```cpp
struct IndexProvenance {
  std::string analyzer;     // "vycor-cpp 0.2.0, index format 10"
  std::string toolchain;    // "LLVM 20.1.2"
  std::string environment;  // environmentFingerprint(...), opaque hex
  uint64_t bakeStartNs;     // wall clock at the start of the writing bake
};
struct IndexCoverage {
  uint64_t requested, indexed, partial, failed;
  bool complete() const;    // requested == indexed
};
IndexCoverage coverageOf(const SnapshotMeta &);   // from meta.outcomes
TuOutcomes SnapshotIO::outcomesOf(const SnapshotMeta &); // per TU path
```

`failed` counts `crashed` + `poisoned` + `timeout` + `skipped` (no facts
at all; `timeout` is a worker killed by `--worker-timeout`);
`partial` has facts from an errored parse. `requested` is the recorded
TU set, i.e. the selection at the last save. Invariant: `requested ==
indexed + partial + failed`.

All of it lives in the meta section, so a consumer can answer freshness
and coverage questions with `SnapshotIO::load(path, stats,
LoadMode::ReadOnly, /*needs=*/0)` and never decode the graph. The query
verbs already load that way for `info`.

JSON, as exposed today (C owns the common envelope; these are the
producer-side shapes, additive to the existing payloads):

`megascope info`:

```json
"provenance": {"analyzer": "vycor-cpp 0.2.0, index format 10",
               "toolchain": "LLVM 20.1.2",
               "environment": "<40 hex>", "bake_start_ns": 1788...},
"coverage": {"requested": 938, "indexed": 930, "partial": 8,
             "failed": 0, "complete": false}
```

`megascope info --files` rows add `status` (the lowercase names above),
`detail` (only when non-empty), and `fingerprint`.

`megascope index` summary line adds `refreshed_for_inputs`, `retried`
(TUs actually re-parsed for reason 4 this run), `indexed`, `partial`,
`failed`; `--stats-json` adds `snapshot.refreshed_for_inputs`,
`snapshot.retried`, `snapshot.fingerprint_ms`,
`snapshot.unstable_stamps`, and a root `coverage` object.

Package C did that (`docs/result-contract.md`): every tool payload
carries `indexScope` with `bake` = `<environment>@<bake_start_ns>`
(also `provenance.bake` in `info`), `freshness`, and the coverage
counts; the per-TU rows stay behind `info --files`; the exception tools
demote a universal verdict when `coverage.complete` is false.

## Format v10

Meta section tail, after the v9 dependency tables: provenance (three
length-prefixed strings and a u64), then per recorded TU the
length-prefixed fingerprint, a status byte (values above 4 are
corruption), and the length-prefixed detail. `kFormatVersion` is 10; a
v9 or older file is rejected with the usual version message and the
warm start falls back to a full build. Section offsets, the header
summary, and the graph/control-flow/channel sections are untouched, so
package F's storage work starts from this layout. Rejecting older
formats is deliberate: a pre-v10 index has no fingerprints or outcomes
to trust.

Format v11 (package B, `docs/path-analysis.md`) adds one byte per catch
handler record (`rethrows`, after `isCatchAll`) in the control-flow
section; the meta section is unchanged. Format v12 (package F,
`docs/control-flow-access.md`) lays the control-flow section out for
reading in place; the meta section is again unchanged.

## Measurements

Setup: llvm-project at `extern/llvm-project`, the 938-TU `lib/` subset
recorded in the existing v9 benchmark index (`~/.cache/vycor-bench/
llvm938-v9.vycs`, its file list re-used as `--source-list`), LLVM
20.1.2 toolchain, 12 threads, Linux, warm page cache. Baseline binary:
`main` at 88bd0f5 (PR #64). New binary: this branch. Each refresh
timed three times with `/usr/bin/time`; the numbers below are the
range.

| Measurement | Baseline (v9) | This branch (v10) |
|---|---|---|
| unchanged refresh, wall | 0.03–0.04 s | 0.03 s |
| unchanged refresh, peak RSS | 29 MB | 30 MB |
| meta section load (`snapshot.load_ms`) | 2.3–3.6 ms | 1.8 ms |
| fingerprinting 938 TUs (`snapshot.fingerprint_ms`) | — | 7.0–8.0 ms |
| index size | 405,310,273 B | 405,356,088 B (+45,815 B) |
| cold bake, wall / peak RSS | — | 154.8 s / 5.6 GB (94,788 nodes, 352,639 edges, 6,373,754 call sites: identical to v9) |
| refresh that retries the 2 failed TUs (`--retry-failed`) | — | 7.0–7.2 s (load 2.3–2.4 s, parse 2.2–2.3 s, remove 0.2 s, absorb 0.05 s, save 1.1 s) |

The unchanged refresh is unchanged within noise: the fingerprint costs
about 8 ms of a 30 ms process, the meta grows by 49 bytes per TU.

Coverage of that bake, which the old format could not report: 936
indexed, 1 partial (`lib/Analysis/ConstantFolding.cpp`, parse errors
from GCC-only flags in the compile command), 1 crashed
(`lib/Object/IRSymtab.cpp`, signal 11 in the in-process CF visitor;
the crash guard skipped it before too, silently). Both are retried with
`--retry-failed` and fail the same way, which is the 7 s row above.

Content hashing was not measured; the tradeoff is stated under
*Effective-input fingerprint*: the dependency list already names every
opened file, and the fingerprint covers the inputs that are not files.

## Follow-ups

- **anneal checkpoints** (`anneal/Checkpoint.h`) have the same gap this
  package closed for megascope: `annealOptionsFingerprint` covers the
  analysis options and per-TU `FileStamp`s, but neither the compile
  commands nor the bake environment. A compile-flag change replays a
  stale phase-1 record. Reuse `fingerprintTUs` there; rewriting the
  journal is outside this package.
- The in-process bake used `ClangTool`'s default real file system, whose
  per-TU working-directory switch is a process-wide `chdir`; parallel
  parses of TUs with different compile directories raced (the
  working-directory scenario caught it). `makeClangTool` now gives every
  tool its own physical file system. anneal and morph share the helper
  and inherit the fix.
- `reindex_tu` over MCP answers a JSON payload through C's envelope now,
  but still not the TU's new outcome: `bakeTU` reports none, so the
  served `indexScope` keeps describing the bake the server started from.
