# AGENTS.md — Developer Guide for AI Assistants

This file is the canonical reference for AI coding assistants working in
this repository. Read it before making changes.

---

## What This Repository Does

`vycor-cpp` is a **Clang LibTooling backend** for C++ static analysis
and source-to-source transformation. It uses Clang's AST (Abstract Syntax
Tree) infrastructure to:

1. **Detect fragile ADL/CTAD resolutions** (`check`) — header-order-sensitive
   name lookups that silently resolve to different declarations depending on
   what is included.
2. **Apply AST matcher transformations** (`morph`) — rule-driven rewrites of
   C++ source code using Clang's dynamic matcher DSL.
3. **Cross-TU call graph index and query tools** (`megascope`) — bakes a
   multi-TU call graph and control flow index into a file, then answers
   queries from the shell (`megascope <tool>`, `batch`, `dump`) for
   LLM-assisted code analysis (security audits, dead code, etc.). Given source selection flags instead of an index, the
   query verbs bake the selected TUs in memory and answer (ephemeral
   mode — the former `prism` subcommand).

It is designed to be called by external tooling (e.g. a Python orchestrator,
or an LLM agent using the megascope verbs for security vulnerability
analysis).

---

## Naming

Glass and optics. **vycor** is the Corning glass family the C++ tool draws
its name from (sibling to Pyrex, more demanding spec); the `-cpp` suffix
leaves the bare `vycor` for a future Python sibling. **check** relieves
latent stress — ADL/CTAD fragility. **morph** reshapes amorphous structure —
AST transforms. **megascope** is the persistent far-seeing instrument — the
cross-TU index and its query verbs (its ephemeral mode absorbed **prism**,
which decomposed one point into its components — call-site control flow).

---

## Feature Layout

The codebase is split into four feature areas:

### `check` — ADL/CTAD Analysis

**Headers:** `include/vycor/check/`
**Sources:** `src/check/`

| File | Purpose |
|---|---|
| `GlobalIndex.h/.cpp` | In-memory database of all function overloads and deduction guides discovered across the project |
| `Indexer.h/.cpp` | Phase-1 AST visitor: walks every translation unit and populates `GlobalIndex` |
| `Analyzer.h/.cpp` | Phase-2 AST visitor: compares each TU's resolved names against `GlobalIndex`; emits `Diagnostic` entries for fragile resolutions |
| `Checkpoint.h/.cpp` | `--checkpoint` journal: append-only per-TU record of phase-1 index contributions and phase-2 diagnostics, so a killed run resumes without re-parsing finished TUs |
| `Report.h/.cpp` | The CI-gate report: `buildFindings` (check name, severity, root-relative location, fingerprint, duplicate merge), inline suppressions, baselines, the changed-lines filter, `renderText`/`renderJson`/`renderSarif`/`renderSummary`, and `checkExitCode` |

The main entry point is `vycor::runAnalysis(compDb, files)` defined in
`Analyzer.h`. It orchestrates both phases and returns a `vector<Diagnostic>`;
an optional `AnalysisReport *` receives every TU's parse outcome
(`TuOutcome`, the worse of its two phases: `Indexed`, `Partial` for a
parse that reported errors, `Skipped`, `Crashed` for a TU the checkpoint
poisoned, `Poisoned`/`TimedOut` for an isolated worker that died or
hung). Checkpoint records and worker shards carry the outcome
(journal and shard format v9; v10 adds `Diagnostic::scope` and the
outcome on an attempt record), so in-process, resumed, and isolated runs
report the same rows. Only a clean (`Indexed`) phase record replays; a
TU that failed is parsed again on resume, and only the
attempts-exhausted skip outlives a failure (reported with the journaled
outcome). Every path the indexer and analyzer record is absolute against
the TU's compile directory (`absoluteFileName`, `check/Indexer.h`).

**check as a CI gate** (`check/Report.h`, contract in
`docs/result-contract.md` "check exit codes" and
`docs/checks/README.md` "Finding identity"): the CLI turns the
diagnostics into findings (check name from the kind or the org check's
`name()`, a per-check severity, the location relative to
`--project-root`, a fingerprint of check + kind + root-relative file +
the entities involved — `Diagnostic::entities`, else
resolved/better declaration and missing header, plus the enclosing
function `Diagnostic::scope` for call-site checks, never a line number;
identical findings share it), drops those an inline
`// vycor: ignore[check]` covers, those in `--baseline` (a multiset:
per fingerprint only the surplus over the baseline count is reported,
preferring findings on changed lines; baseline file version 2), and
those outside the changed lines (`--patch-file`,
`--git-base`), renders `--format text|json|sarif`, and exits 0 clean,
1 findings at or above `--fail-on`, 2 usage, 3 a TU that did not parse
cleanly (unless `--allow-parse-failures`). The JSON report is
byte-identical across in-process, `--checkpoint`, and
`--isolate-workers` runs (`scripts/check-gate-check.py`, ctest
`check_gate`; SARIF validated against the vendored
`tests/data/sarif-schema-2.1.0.json`).

Both phases run per-TU on a worker pool (`AnalysisOptions::threadCount`,
same 0=hardware/1=serial semantics as `bakeIndexes`; per-file diagnostic
slots keep output order deterministic). With
`AnalysisOptions::checkpointPath` set (CLI: `--checkpoint`), per-TU
progress is journaled and replayed on resume: phase-1 records carry the
TU's full index contribution (no re-parse on resume), phase-2 records are
keyed on a hash of the whole contributing file set (any phase-1 change
invalidates them all — one TU's diagnostics depend on every other TU's
declarations), stamps are mtime+size (`FileStamp`), an options-fingerprint
mismatch discards the journal wholesale, and a TU whose parse fatally died
twice (attempt records, no completion) is skipped as poisoned instead of
re-killing every resume. See `check/Checkpoint.h`.

`--odr-diag` adds cross-TU ODR violation detection: phase 1 records a
`clang::ODRHash` per vague-linkage definition (inline functions, in-class
method bodies, class definitions — external linkage, non-template,
non-system-header), and an index-only pass compares sites project-wide.
`ODR_DivergentDefinition` = one site whose body hashes differently across
TUs (preprocessor-dependent definition); `ODR_DuplicateDefinition` = the
same entity defined at multiple sites with differing content.
Token-identical copies at different sites are deliberately not flagged
(benign vendoring), and method-level echoes of an already-flagged class
are suppressed. This is the ODR class ordinary builds cannot see: linkers
error on duplicate strong symbols but silently keep one arbitrary copy of
mismatched weak/COMDAT definitions. OdrEntries ride checkpoint payloads
and worker shards (since journal/shard format v2; v19 is current).

`--isolate-workers [--workers N]` runs the per-TU parses in subprocess
workers (megascope's model): the parent spawns `check --index-worker` /
`--analyze-worker` batches through the generic `dispatchIsolated`
dispatcher (`callgraph/WorkerPool.h` — batching, WORKER-TU poison markers,
crash/bisect, shared with the megascope bake), phase-1 workers write
per-TU index shards, and phase-2 workers read the merged index from a
handoff file (`--global-index`) and write diagnostics shards. Composes
with `--checkpoint`: shard results are journaled per TU as they land.
`AnalysisOptions::isolatedRunner` is the test seam (in-process fake
workers, no spawning). `--worker-timeout`/`--worker-memory-limit` apply
to these workers too (`runWorkerProcess`, below).

`could-be-internal` (`check/CouldBeInternal.h/.cpp`) is an opt-in
whole-program check over the megascope reference facts: external
functions and variables with one defining TU and no reference from
another TU (`Internal_Unreferenced`, a warning), or declared in a header
(`Internal_DeclaredInHeader`), or referenced from elsewhere only by test
sources (`Internal_TestOnly`, `--test-path-re`, matched relative to the
project root). Its verdict needs every C/C++ entry of the compilation
database parsed cleanly (`describeCoverageGap`); otherwise tier 1 is
`Internal_Unproven` and every message says why. main.cpp reads the
references section from `--index` (default
`<build-path>/.vycor/megascope.vycs`) when it holds exactly the selected
sources and none is stale, else bakes once with `bakeIndexes` and shares
the graph with `dead-code` / `static-init-hazards`. See
`docs/checks/could-be-internal.md` and `tests/test_could_be_internal.cpp`.

`hot-path-effects` is an opt-in graph-backed check
(`check/HotPathEffects.cpp`, run in main.cpp's graph block beside
`dead-code` and `static-init-hazards`): `analyzeHotPaths` over the call
graph with roots from `--hot-root`, `hotPaths` in the org config, or
`--hot-profile` + `--hot-min-count`; latent findings set
`Diagnostic::note` (Note severity). See `docs/checks/hot-path-effects.md`.

`unsynchronized-shared-global` (`check/UnsynchronizedSharedGlobal.h/.cpp`,
off, groups `noisy` and `compute-heavy`) reads the same graph and
reference facts: each `std::thread`/`std::jthread`/`std::async`/
`std::packaged_task` thread entry is a context (plus the main thread
from `main` and `--entry-point`), a BFS per context over (function, lock
set) states with parent pointers and per-function / per-context caps
joins the lock facts of each call on the path with those at the access,
and a variable (not const, atomic, thread_local, a sync primitive, or
used as a lock) written in one context and accessed in a concurrent one
with disjoint lock sets is a finding. `SharedGlobal_Race` (warning)
needs exhaustive searches and complete coverage; `_Unproven`,
`_Plausible` (plausible edge or packaged task) and `_InitThenPublish`
(one side on the main thread) are notes (journal/shard v17 renumbers
`Custom` for them). The lock facts come from `ReferenceVisitor`
(`callgraph/LockFacts.h` classifies mutex, guard and sync-primitive
types) and ride the reference records (format v17). See
`docs/checks/unsynchronized-shared-global.md` and
`tests/test_shared_global.cpp`. The lock-set reading and the per-call
join (`LockSets`, `CallLockFacts`: interned lock identities and sets,
canonical callees, the sets each call adds) live in `check/LockJoin.h`,
shared with lock-order-inversion so the two agree on what the same
lock is.

`lock-order-inversion` (`check/LockOrderInversion.h/.cpp`, off, group
`compute-heavy`) builds the held-while-acquiring graph from the
reference facts' `lock` / `lock_shared` records (each a waiting
acquisition with the locks held before it; try and timed locks are none,
each mutex of a `std::scoped_lock` / `std::lock` is acquired under the
set before it; recursion flags `kRefRecursive` / `kRefNonRecursive`;
snapshot format v18): an edge `A -> B` for each acquisition under `A`,
and for each call under `A` to every lock in the callee's acquisition
closure (Tarjan components of the call graph, one hash-consed bitset of
two bits per lock per component, all edges and proven edges only).
Cycles: Tarjan components of the lock graph, then a bounded DFS
enumeration from each cycle's lowest-ranked lock (8 locks, 64 cycles
per component, 4096 total). A cycle all of whose edges are witnessed
only under one more global lock is gated and dropped; a non-member
mutex acquired again while held is a self-deadlock unless recursive.
`LockOrder_Inversion` (warning) needs proven, exclusive witnesses and
complete coverage; `_Unproven`, `_Plausible` and `_Shared` are notes
(journal/shard v18 renumbers `Custom` for them). See
`docs/checks/lock-order-inversion.md` and `tests/test_lock_order.cpp`.

`callback-under-lock` is an opt-in TU-local AST check in
`check/CallbackUnderLock.cpp`: lexical standard mutex guard ownership plus
cycle-safe helper/virtual-override paths to application callbacks. It emits named
`Custom` diagnostics through the existing reporting/worker payload, and bypasses
checkpoint reuse because headers and compile flags can change the lock witness.
See `docs/checks/callback-under-lock.md` and `tests/test_callback_under_lock.cpp`.

`noexcept-candidate` (with `noexcept-candidate-move` and
`noexcept-candidate-virtual`) reports functions proven unable to throw that
are not declared `noexcept`. Phase 1 (`check/NoexceptFacts.cpp`,
`collectNoexceptFacts`) walks every function body of the TU itself — it
does not read the megascope call graph — and exports one
`NoexceptFactEntry` per project definition: flags plus the callees its
exits depend on. Bodies with no fact of their own (template
instantiations, implicit members, lambdas, system-header inlines) are
folded into their callers per TU. Phase 1.5 (`check/NoexceptCandidate.cpp`,
`analyzeNoexceptCandidates`) is a greatest fixed point over the merged
facts on flat arrays (sorted fact view, reverse CSR, one worklist): a
callee with no fact is unknown, never "cannot throw". When any TU did not
index cleanly, findings whose proof crosses into another TU are withheld
(`NoexceptCandidateOptions::coverageComplete`), a callee defined
`noexcept` there included: an obligation on it exists only where the
calling TU did not see that declaration. Findings carry
`Diagnostic::rank`, which the report sorts by and emits as JSON `rank` /
SARIF `properties.order`. Enabling it turns the `--checkpoint` journal off
for the run (stderr says so). Proof
rules, assumptions and what is withheld: `docs/checks/noexcept-candidate.md`;
tests in `tests/test_check_noexcept_candidate.cpp`.

`exception-boundary-escape` (off, `noisy`) reports thread entries
(`std::thread`/`std::jthread`, also emplaced into a standard container,
`pthread_create`, `thrd_create`), callbacks handed to C (a C-linkage
callee's function-pointer parameter, a C struct's function-pointer
field), `extern "C"` definitions, and frames compiled without exceptions
that an exception can reach, when they are not declared `noexcept`
(those are `exception-escape`'s). It runs on the noexcept-candidate
facts, which carry, besides the proof's data, leaf reasons with a site
(`NoexceptFactEntry::Reason`), a `Bare` callee bit (no `try` at all,
versus only typed handlers), the C-linkage and exception-mode flags,
and the hand-over sites (`ExceptionBoundaryEntry`, collected in the same
body walk). Phase 1.5 (`check/ExceptionBoundary.cpp`) builds the shared
graph (`check/NoexceptGraph.h`, also noexcept-candidate's) and runs one
reverse BFS per witness class (`proven`, `library`, `typed-handlers`,
`unknown`); proven findings are warnings, the rest notes, ranked. A
clean boundary is only counted, never claimed, and the count is a proof
only over a clean index. Reference: `docs/checks/exception-boundary-escape.md`;
tests in `tests/test_check_exception_boundary.cpp`.

`isa-leak` reports vague-linkage functions emitted by TUs whose compile
commands disagree on code generation (target features, FP semantics,
exceptions; `--isa-leak-facets`, default `isa,fp,eh`). Phase 1
(`check/IsaLeak.cpp`, `collectIsaLeakFacts`) reads one `CodegenProfile` per
compile command from the parsed invocation (effective `TargetOptions`
features, `LangOptions` FP/EH state) and walks the TU's emission graph the
way codegen does (`callgraph/EmissionGraph.h`): from strong definitions, the global variables
`DeclMustBeEmitted` keeps, homed vtables (every slot, inherited ones
included) and key-less vtables where a constructor or vptr-storing
destructor is emitted, through every referenced body, skipping unevaluated,
constant-evaluated and Clang-folded dead code and devirtualizing only where
CodeGen does (`getDevirtualizedMethod` plus its covariance and class checks), so an inline function no emitted code reaches is
not a sighting. Each emitted non-system `GVA_DiscardableODR`
function gets body bits (loop, FP arithmetic, FP moves (on x86-64 relevant to `avx`/`avx512fp16`/`sse`/`sse2`/`x87` alone), vector, bulk memory, bit/target
builtins, cleanup around may-throw calls, signed arithmetic), folded over
what it can inline. Phase 1.5 (`analyzeIsaLeaks`) projects each profile onto
the feature classes and facets the body is sensitive to and reports a
function with two or more projections on one triple: `IsaLeak_Divergent`
(warning) when its own body is sensitive, `IsaLeak_Inlined` (note) when only
what it can inline is (the copies then differ only in optimizing builds that
keep it out of line). Facts live in
`IsaLeakFacts` (own lock, interned profile, TU and function ids); they ride
worker shards (since v15) in that interned form (`IsaLeakPayload`: tables plus
flat id triples) but not the phase-2 handoff. Enabling it turns the
`--checkpoint` journal off. Reference: `docs/checks/isa-leak.md`; tests in
`tests/test_check_isa_leak.cpp`.

### `morph` — AST-Based Transformations

**Headers:** `include/vycor/morph/`
**Sources:** `src/morph/`

| File | Purpose |
|---|---|
| `MatcherEngine.h/.cpp` | Parses Clang dynamic matcher expression strings; registers callbacks with `MatchFinder`; runs `ClangTool` and accumulates `Replacement` objects |
| `TransformPipeline.h/.cpp` | Orchestrates multiple passes of `MatcherEngine`; merges replacements across passes; optionally writes to disk |

The main entry point is `vycor::TransformPipeline::execute(buildPath, files, dryRun)`.

### `callgraph` — Call Graph and Control Flow Analysis

**Headers:** `include/vycor/callgraph/`
**Sources:** `src/callgraph/`

| File | Purpose |
|---|---|
| `AtomicFile.h/.cpp` | `writeFileAtomically` (unique temp, fsync, rename, directory fsync — every index, journal header, and shard write), `IndexWriteLock` (`flock` on `<index>.lock` for `index`), stale temp cleanup |
| `CallGraph.h/.cpp` | Graph data structure: nodes (functions), edges (calls), class hierarchy, virtual overrides |
| `CallGraphBuilder.h/.cpp` | Two-phase AST visitor: Phase 1 indexes nodes/hierarchy, Phase 2 builds edges; after the code as written the edge visitor drains a worklist of the instantiation and compiler-written bodies the TU uses (see "Instantiation and compiler-written edges") |
| `CompilerWritten.h` | The compiler-written code both bake visitors enumerate: which special members the compiler defines, the definition to read, the destructor a type's destruction runs, a destructor's subobjects, explicit instantiation definitions |
| `CollapseFilter.h/.cpp` | Path-based edge collapse — skips internal edges in specified directories |
| `ControlFlowIndex.h/.cpp` | Per-call-site record of enclosing try/catch scopes, conditional guards, and live RAII locals; resident (deduplicated records, set tables, hash maps) for bakes, `batch`, and ephemeral queries, or mapped over a v12 snapshot for the one-shot query verbs (`docs/control-flow-access.md`) |
| `ControlFlowContextVisitor.cpp` | Phase 3 AST visitor: snapshots exception/guard context at each call site |
| `PathSearch.h/.cpp` | The shared bounded reverse path search (`findCallerPaths`): exact hops (USRs, call site, kind, confidence, execution context), canonical order, stop reasons, `complete`/`exhaustive`; used by `find_call_chain`, the oracle, and the lock tools. Contract: `docs/path-analysis.md` |
| `ControlFlowOracle.h/.cpp` | Query engine over the path search: exception verdicts (universal only when the search was exhaustive), per-path propagation outcome, nearest catches, call site context |
| `ReferenceIndex.h/.cpp` | Symbol reference facts (`docs/symbol-references.md`): one record per (referrer, target, role) with an occurrence count and one witness site, and per symbol its kind, linkage class, attributes, declarations, and defining TUs; content-deduplicated with per-TU contributor lists (`removeTUs`, `absorb`), canonical encoding as the snapshot's references section |
| `ReferenceVisitor.h/.cpp` | Phase 4 AST visitor of the single-parse bake: counts references by role (`call`, `address_of`, `read`, `write`, `vtable_use`, `typeinfo_use`) in a per-TU hash map keyed by entity slots, and lowers names, USRs, and linkage facts once per entity. After the code as written it drains a worklist of the instantiation bodies the TU uses (attributed to the pattern, adding only keys the pattern lacks, at the largest single-instantiation count) and of transparent functions (implicit special members, system-template instantiations, generic-lambda call operators), whose references are spliced into their users at lowering (Tarjan SCC summaries); implicit constructor/destructor calls, `operator new`/`delete`, and a vtable's implicit virtual destructor are recorded on the way; tracks the locks held (scoped guards, lock/unlock calls, call_once and static-local once guards) and records the lock sets per reference (`docs/symbol-references.md` "Lock facts"); records the TU's emission facts from `EmissionGraph` (vague-linkage and header-internal bodies with linkage name, pattern symbol, definition and size; format v20) |
| `EmissionGraph.h/.cpp` | `buildEmissionGraph`: one TU's emission graph, walked the way Clang's CodeGen decides what to emit (strong definitions, `DeclMustBeEmitted` globals, homed and key-less vtables, constant data's addresses; unevaluated, constant-evaluated and folded code skipped; devirtualized only where CodeGen does), with per-node body bits and a size in statements. Shared by isa-leak and the bake's emission facts (`ReferenceVisitor`, `docs/symbol-references.md` "Emission facts") |
| `LockFacts.h/.cpp` | Type classification the lock facts share: mutex types, guard types, synchronization primitives, `std::defer_lock`-style tags |
| `WorkerPool.h/.cpp` | Subprocess worker isolation shared by the megascope bake and check: `dispatchIsolated` (batching, WORKER-TU poison markers, crash/bisect), `runWorkerProcess` (spawn, `WorkerLimits` timeout that restarts at every marker and treats expiry as a crash with a `timeout` outcome, memory limit), and `bakeIsolated` |
| `CrashGuard.h/.cpp` | In-process crash guard: `llvm::CrashRecoveryContext` with handlers on a per-thread `sigaltstack`; parses write TU-local indexes absorbed only after a clean return, so a crash leaves no partial facts and no shared lock held |
| `Interrupt.h/.cpp` | SIGINT/SIGTERM: a watcher thread kills tracked workers, removes registered scratch paths and `RemoveFileOnSignal` files, and re-raises; workers die with their parent (`PR_SET_PDEATHSIG`) |
| `Effects.h/.cpp` | `EffectTable`: the functions that allocate, acquire a lock, do I/O, or throw, matched by qualified name with template arguments and inline namespaces dropped (`effectLookupName`); given the USR as well, a placement `operator new` (`isPlacementNewUsr`) is not an allocation. One table for every analysis that asks whether a call path has an effect; extended by the org config's `effects` key and `registry.addEffectFunctions` |
| `CoverageProfile.h/.cpp` | `llvm-cov export` JSON read into flat per-file tables once: `countAt(file, line, col)` (the count of the last segment at or before a location), `functionAt` (the innermost function body, entry counts summed over records sharing a body), `fileFor` (path-suffix match, ambiguity is no match) |
| `HotPath.h/.cpp` | `analyzeHotPaths`: roots (names, USRs, regexes, profile entry counts ≥ `minCount`), one label-setting widest-path search over flat arrays sized once by the interner (weight = smallest call-site or entry count; fewest calls among equals; canonical parent on ties: Proven first, then caller USR, site, kind; profile files bound two-sided), effect functions end a path, allow list skips; reached template instantiations whose body no TU read counted as `templateLeaves`; findings per effect call with a witness, `observed` / `unprofiled` / `latent`, canonical order (`docs/checks/hot-path-effects.md`) |
| `StaticInit.h/.cpp` | `StaticInitFacts`: the dynamic initializers and `__attribute__((constructor))` functions of each TU, with the functions each initializer calls directly. Collected by the bake's parse (`collectStaticInits`), merged across TUs by `canonicalize` (a root is its usr at its location). Held in memory only (`BakedIndexes::staticInits`), not in the snapshot |
| `RetainUntilExit.h/.cpp` | `retainUntilExit(p)`: the one way to leak on purpose (a one-shot query's index, a crashed parse's TU-local indexes). The pointer is kept reachable from a global, so LeakSanitizer reports only unintended leaks |
| `Utf8.h` | Index text: every string an index holds is valid UTF-8, converted from raw bytes by an exact escape (a byte outside a valid sequence becomes U+10FF00+byte; a literal U+10FF80..U+10FFFF is escaped byte by byte), so distinct raw strings stay distinct (`fromIndexText` inverts it). Producers (`CallGraph::add*`, `addCallSiteContext`, `ChannelIndex::addSite`) and `removeTUs` convert raw bytes; absorb, the loader and the diff copy text; lookups use `lookupText` (valid UTF-8 as is, else converted). The `StringInterner` stores bytes and never converts (check shares it, raw). The snapshot meta keeps TU paths raw (warm start stats them); its JSON sinks (`info`, `diff`, `index`'s summary, `--stats-json`) convert with `toIndexText`. Contract: `docs/result-contract.md` "Strings that are not UTF-8" |

**Single-parse build** (`megascope index` and the ephemeral query mode):
`bakeIndexes(compDb, files, ...)` runs all four visitor phases —
declaration/hierarchy index, edge building, CF context, symbol
references (`ReferenceVisitor`) — over ONE frontend parse per TU. No phase barrier is needed because edge building has no
cross-TU reads: the two joins that used to require completed Phase-1 data
(virtual-dispatch fan-out and the function-pointer-through-return join,
`EdgeKind::FunctionPointerReturn`) are performed at query time in
`CallGraph::calleesOf`/`callersOf`. Measured on llvm-project lib/Support
(149 TUs, 12 threads): cold bake 17.1s → 9.9s vs the old two-parse bake,
identical query results. `buildCallGraph` / `buildControlFlowIndex` remain
for callers that need only one of the two indexes (also single-parse).

**Virtual dispatch** is stored as ONE Plausible `VirtualDispatch` edge to the
static target per call site. `CallGraph::calleesOf`/`callersOf` expand it
through the transitive override map at query time, so overrides indexed
later (other TUs, warm refreshes) are visible to existing call sites.
Proven `VirtualDispatch` edges (concrete type known) are never expanded.

**Allocation edges**: a new-expression and a delete-expression each add
a `DirectCall` edge to the allocation or deallocation function overload
resolution selected (`operator new`, `operator new[]`, a placement
overload, a class's own), at the expression. `Proven`, except a plain
`delete p` through a virtual destructor, where the function is the
dynamic type's and the edge is `Plausible` (`delete[]`, `::delete` and a
destroying `operator delete` stay `Proven`). A type-dependent `new` in a
template's definition resolves nothing and adds no edge (each
instantiation's body has its own). The global
`operator new` / `operator delete` forms the compiler declares on its own
have one node at that implicit declaration (file `<unknown>`, line 0),
whether or not a TU includes `<new>` and even where a program defines a
replacement; the forms only `<new>` declares (`nothrow`, placement) sit
in that header.

**Instantiation and compiler-written edges**: after the code as written,
`CallGraphEdgeVisitor` drains a worklist (LIFO, each body once per TU)
of the bodies the TU uses that no written definition holds: function and
member instantiations (system headers included; `extern template` ones
have no body), generic-lambda specializations, explicit instantiation
definitions, and compiler-written special members (`CompilerWritten.h`,
shared with `ReferenceVisitor`). Their calls are edges under the
instantiation's own USR, with dependent calls resolved; the node is
marked `hasBody` (snapshot node flags value 4, OR-merged across TUs),
which is how `templateLeaves` tells an instantiation that calls nothing
from one no TU instantiated. The virtual members an instantiated class
template specialization defines are read too (only a vtable reaches
them), and its bases and overrides are recorded, so a dispatch reaches
`Impl<int>::run`. A lambda written in a function template keeps its
written identity: one node whose edges are every instantiation's calls;
a generic lambda's specializations share the closure's node too. Written
definitions also get what the compiler adds to them: non-written
member/base initializers, default member initializers, a defaulted body,
member/base destruction after a destructor's body; a member the compiler
constructs is sited at the member. Destructor edges: a local at
its declaration, a temporary at its expression, `delete`
(`VirtualDispatch` Plausible through a virtual destructor). Trivial
members add nothing; a closure's special members are transparent (its
captures' constructors and destructors at the use). Unevaluated operands
(`sizeof`, `noexcept`, `decltype`, requires-expressions) add nothing.
Nodes are located at the first declaration so every TU agrees. The
control-flow index reads only the code as written: these edges have no
context of their own.

**Throw edges**: a `throw` expression is a Proven `DirectCall` edge to
`__cxa_throw` (a bare `throw;` to `__cxa_rethrow`), whose node sits at
file `<unknown>`, line 0, like the compiler-declared `operator new`; the
control-flow index records the site's context as for any call.

**Edge collapse**: When `collapsePaths` is non-empty, edges where BOTH caller and callee
are in collapsed paths are skipped. Boundary edges (non-collapsed caller → collapsed callee)
are preserved. This reduces noise from utility/math headers while keeping entry points visible.

`makeClangTool` calls `compat/ToolingRuntime` once before frontend work. Its
instrumented initialization barrier materializes LLVM statistics state without
enabling statistics, so prebuilt LLVM lazy initialization is ordered across
parsing threads (including the isolated-worker test seam). In a
ThreadSanitizer build it also parses a few declarations that make Sema build
its function-local name sets (`SemaAttr.cpp`: the standard-library owners,
pointers, containers and nullable classes): a prebuilt libclang reads those
through a guard fast path the sanitizer cannot see, and reports their
construction on one parsing thread racing with a lookup on another.
Subsequent parses remain concurrent; no sanitizer reports are suppressed.

**Crash and hang containment**: `megascope index` bakes in subprocess workers
by default whenever `--threads` is not 1 and `--pch-dir` is unset
(`--isolate-workers=false` opts out). A worker that starts no new TU for
`--worker-timeout` seconds (default 600) is killed and handled as a
crash; its TU is recorded `timeout`. In-process parses (`--threads 1`,
ephemeral queries, tests) run under `CrashGuard.h` into TU-local
indexes absorbed only on a clean return. The fault-injection seam is
`CallGraph::setEdgeInsertHookForTesting`; the reproductions live in
`tests/test_crash_containment.cpp` and `scripts/interrupt-check.py`
(ctest `interrupt_cleanup`).

### `query` — Transport-Neutral Query Tools

**Headers:** `include/vycor/query/`
**Sources:** `src/query/`

Every megascope tool is a pure function `(json::Object args, ToolContext)
-> json::Value` over the in-memory indexes. The CLI verbs
(`cli/`) are a thin adapter over this table; tests call the handlers
directly.

| File | Purpose |
|---|---|
| `Tools.h` | `ToolContext` (indexes, entry points, cache, header summary, `facts` — the `IndexFacts` every payload cites — and `meta`, the loaded index's `SnapshotMeta`, which the dependency audit reads for per-TU outcomes and opened files), `ToolEntry` (name, description, JSON Schema, handler, `recordsKey` — the payload's list member, which drives the CLI's ndjson/tsv output and empty-result exit code — and `needs`, the index sections the handler reads), `QueryCache`, `getRegisteredTools()`, and the result contract (`docs/result-contract.md`): success = payload object; error = `{"error": msg, "status": kind}` built with `usageError` / `notFoundError` / `unavailableError`; ambiguity = `{"ambiguous": true, candidates...}` (`isAmbiguousResult`); `runTool` runs a handler and `completeResult` stamps `status` (`statusOf`) and `indexScope` onto the payload — what every adapter calls |
| `Identity.h/.cpp` | F8 identity resolution: `resolveIdentity` (name/usr/site/filter → USR; an identity that names no node and no edge endpoint is `not_found` with `didYouMean`), the disambiguation payload, `attachUsr` (`resolvedAs: "name"` for a node-less endpoint), the `name`/`function` alias, `rankFunctionMatches` (the `search_functions` ranking) and `suggestFunctions` |
| `Paging.h` | The shared paging contract of the list tools: `parsePage` (`limit` ≥ 1 with a per-tool default cap, `offset`), `attachPage` (`total`, `offset`, `limit`, `returned`, `truncated`, `nextOffset`), `addPagingProps` for the schemas (`docs/result-contract.md`, "Paging") |
| `Serialize.h/.cpp` | Enum spellings (`EdgeKind`, `Confidence`, `ExecutionContext`, `ChannelOperation`) and JSON serializers for edges, guards, channel sites — part of the output contract |
| `GraphTools.cpp` | lookup, search, callers, callees, call chain, class hierarchy, entry points, graph summary, callback/concurrency sites |
| `ExceptionTools.cpp` | exception safety, call-site context, RAII scopes, throw propagation, all-path contexts, nearest catches |
| `LockTools.cpp` | locks held, same-lock path search |
| `DeadCodeTools.cpp` | dead-code analysis (cached liveness) |
| `ChannelTools.cpp` | channel listing/query, channels-for-function, explain-ordering |
| `ReferenceTools.cpp` | `lookup_symbol` (linkage, attributes, declarations, defining TUs, referrers per role) and `get_references` (who references a symbol, or what it references, by role); both read the references section alone |
| `ImpactTools.cpp` | `impact_of_change`: seeds by name, usr, or unified diff, reverse walk over the `impact/` module; also the serializers the `diff` verb shares |
| `DependencyTools.cpp` | `audit_dependencies`, `explain_dependency`: load the `--target-map` file, build `LinkFacts` from the reference facts (the call edges for an index without them), run the `deps/` kernel, page and serialize (`docs/dependency-audit.md`) |
| `MetricsTools.cpp` | the `metrics` tool: metric selection, roll-up level, target map, paging, and the `--list-metrics` listing over the `metrics/` module |
| `EffectTools.cpp` | `query_path_effects`: roots, patterns, profile, `min_count`, effect subset and allow list over `HotPath.h`; the profile join and verdict in the payload (`clean` needs no `templateLeaves`) |
| `Registry.cpp` / `Registry.h` | Composes the per-family `register*Tools` lists into `getRegisteredTools()` (tools/list order); result-contract helpers |
| `Schema.h` | JSON Schema property builders shared by the registrations |
| `Limits.h/.cpp` | `readLimit`/`readLimitAs`: every user-supplied search limit is clamped to a documented maximum (`kMaxSearchDepth`, ...) before it is narrowed; `docs/path-analysis.md` "Limits" |

### `impact` — Semantic Diff and Change Impact

**Headers:** `include/vycor/impact/`
**Sources:** `src/impact/`

| File | Purpose |
|---|---|
| `SemanticDiff.h/.cpp` | `IdentityTable` (stable keys: usr, lambda keys by enclosing function and ordinal, byte offsets in closure-type USRs by ordinal), `ContextSignatures` (one id-level pass over the stored contexts, so a side's control-flow index is dropped before the other side loads), `semanticDiff` (functions and call relationships added/removed, per-relationship context signatures changed, moves, rename candidates, ambiguity), `checkComparability` (config, TU selection, failed TUs, analyzer), `diffRoutes` (path sets to a target on both sides), `changedFunctionsAfter` |
| `ImpactSearch.h/.cpp` | `findImpact`: bounded reverse BFS from the changed functions (`max_depth`, `max_fan_in` — the seeds themselves are always expanded — `max_work`), one shallowest path per affected function, canonical order |
| `PatchMapping.h/.cpp` | `parseUnifiedDiff` (after-side hunk ranges, deletions as the two lines they sit between) and `mapRangesToFunctions` (call-site or declaration hit exactly, else the nearest anchors' extents, else the file) |

Contract and worked examples: `docs/change-impact.md`.

### `deps` — Dependency Audit

**Headers:** `include/vycor/deps/`
**Sources:** `src/deps/`

| File | Purpose |
|---|---|
| `TargetMap.h/.cpp` | The build-system-neutral target map (`schema` 1: link targets, their sources and headers, declared `deps`, `depsForm` `direct` or `closure`), parsed and validated once at load; an external query-time input, never part of the snapshot |
| `DependencyAudit.h/.cpp` | Two layers. `LinkFacts` — flat arrays: one class (`Defined`, `Presumed`, `Inline`, `Unattributed`), one interned owner-target set and flags per symbol, hard and soft references in CSR form — produced by `linkFactsFromReferences(refs, graph, map, meta)` (overload candidates and dispatched virtual calls soft; vtable entries, bases and inherited virtuals added) or, for an index without reference facts, `linkFactsFromCallEdges(graph, map, meta)`. `auditDependencies(facts, map)` — the kernel: the declared-reach bit matrix, per-edge seeds, closure (soft references to the provider's own symbols included), needs, fan-in, what each direct edge alone pulls in, the link-target gain (a GNU ld archive-member simulation per final link target, before and after the closure is split out file by file: `linkTargetsHelped`/`linkTargetGain`/`linkTargetMembers`, with `fanInPct`/`splitGain` discounting a closure the rest still calls into), the findings. It reads `LinkFacts` only |

Attribution rules, the closure's treatment of inline bodies, the
payload members, and every known way an answer is wrong:
`docs/dependency-audit.md`. Each payload carries `completeness` (which
kinds of reference the evidence cannot see) and each finding
`"universal": false`; `unused-link-dependency` is withheld unless both
targets' sources are fully indexed. Adapters that write the target map
are scripts (`scripts/cmake-target-map.py`, CMake File API);
`scripts/dep-audit-nm-compare.py` measures the audit against the
undefined symbols of a built tree.
### `metrics` — Named Measurements

**Headers:** `include/vycor/metrics/`
**Sources:** `src/metrics/`

Some findings are measurements, not warnings. A metric is a named
reduction over the megascope index with no pass/fail, selected like a
check (`--metrics=<spec>`, `--list-metrics`) and documented under
`docs/metrics/`. The surface is the megascope tool `metrics`
(`query/MetricsTools.cpp`); `vycor-cpp metrics ...` is an alias for
`vycor-cpp megascope metrics ...` (`main.cpp`), so the output contract,
paging, `batch` and the goldens are the tools' own.

| File | Purpose |
|---|---|
| `Metric.h/.cpp` | `Metric` (name, summary, unit, native level, `unavailable`, `measure`), `MetricRun` (the samples, and `rollUp` — a group's value from its members, a sum unless the metric overrides it), `MetricSample`/`MetricEntity` (a sample's `pattern` keys the template roll-up, its `tuIds` index `MetricRun::tuNames`), `RollupLevel` (symbol, template, file, tu, directory, target), `runMetric` (group, roll up, canonical order: value descending, then entity file, line, name, usr, kind), record serialization |
| `MetricSet.h/.cpp` | The built-in metric table plus the registry's, `resolveMetricSpec` (the `--checks` spec syntax), `metricsRequestReadsIndex` (a listing or a request that cannot succeed answers before any index is loaded or baked) |
| `TargetOwners.h/.cpp` | The target map of the `target` roll-up: which link targets list a TU as a source (`targets[].name`, `targets[].sources`, `root`) |
| `RedundantInstantiation.cpp` | `redundant-instantiation`: per linkage name two or more TUs emit (the emission facts), copies × statements, with the template pattern for the `template` roll-up (`docs/metrics/redundant-instantiation.md`) |
| `StaticInitCost.cpp` | `static-init-cost`: per static-init root, the distinct functions reachable through the call graph and whether that reach meets the effect table; a group's value is the union of its members' reaches (`docs/metrics/static-init-cost.md`) |

Record shape, roll-ups and payload: `docs/metrics/README.md`. A metric
whose facts the index does not hold answers `unavailable` (exit 3),
never zero: `static-init-cost` reads `ToolContext::staticInits`, which
only an in-memory bake fills. Organization metrics register with
`VYCOR_REGISTER_METRIC` (`ext/Extensions.h`).

### `cli` — megascope Verbs (adapter)

**Headers:** `include/vycor/cli/`
**Sources:** `src/cli/`

| File | Purpose |
|---|---|
| `BakeConfig.h/.cpp` | `parseChannelTypesJson`, `loadOrgConfigIfSet`, `mergeExtensionConfig` — the bake configuration shared by `main.cpp`'s verbs and the ephemeral query mode |
| `MegascopeCli.h/.cpp` | The query and index-reducer verbs (`<tool>`, `call`, `tools`, `info`, `batch`, `dump`, `merge`): `parseToolArgs` derives `--flags` from each tool's JSON Schema (strings take a value, integers parse, booleans are bare, arrays repeat; hyphens and underscores interchangeable; `--args '<json>'` seeds), `emitToolResult` implements the output contract (compact JSON / `--pretty` / `--format ndjson` with a leading `{"_summary":...}` line / `--format tsv` with sorted columns) and `exitCodeFor` the exit codes, derived from the payload's typed `status` (0 results, 1 empty or not found, 2 usage, 3 index missing or unavailable facts, 4 ambiguous; `docs/result-contract.md`); `resolveIndexPath` is the `--index` → `$VYCOR_INDEX` → `<build-path>/.vycor/megascope.vycs` → `./.vycor/megascope.vycs` chain; `dump` streams every call-site context and channel site through `ControlFlowIndex::forEachContext` (ndjson default, or one json document via `llvm::json::OStream`); `merge` (`runMergeVerb` → `SnapshotIO::merge`) combines per-TU or per-target `.vycs` shards into one saved index (`--output`, `--input`, `--input-list`, `--allow-mismatch`, `--no-wait`, diamond-dependency TU deduplication); ephemeral mode (`--source`/`--source-list`/`--source-re` with `--build-path` or `-- <compile-flags...>`) runs `selectSources` + `bakeIndexes` in memory and answers from that, no index read or written |
| `DiffVerb.cpp` | the `diff` verb: loads `--before` and `--after` saved indexes, refuses non-comparable pairs unless `--allow-mismatch`, `--to` adds the route diff, `--impact` the walk from the changed functions; `seedImpactPatch` turns `--patch-file` / `--git-base` `--git-head` into `impact_of_change`'s `patch` argument |
| `SourceSelection.h/.cpp` | `selectSources`, `canonicalPath`, `absolutePath`, and `loadCompilationDatabase(buildPath, fixedArgs, error)` (supports direct compiler flags after `--` via `FixedCompilationDatabase`, a compilation database JSON file path via `JSONCompilationDatabase::loadFromFile`, or a build directory via `CompilationDatabase::loadFromDirectory`) |

The query verbs load the index with `LoadMode::ReadOnly`
(`callgraph/Snapshot.h`): the edge dedup map and per-TU provenance that
only `removeTU`/`absorb` read are skipped (halves the load on the 938-TU
testbed), the graph ignores (and warns once about) any later mutation
in every build type, and the loaded
indexes are deliberately leaked at exit. They also decode only the
sections the tool declares (`ToolEntry::needs`, set in
`query/Registry.cpp`): the snapshot is sectioned (format v8; v9 adds the
per-TU dependency tables to the meta, v10 the bake provenance and the
per-TU input fingerprints and parse outcomes, v11 a `rethrows` flag per
catch handler, v12 a control-flow section laid out for reading in
place, v13 a checksum per section and for the header, v14 index text
(`callgraph/Utf8.h`) for every graph, control-flow and channel string,
v15 the references section and its two header counts, v16 each
reference record's largest contributing TU, v17 lock facts and a flag
byte on each reference record, v18 the lock acquisition records,
v19 the calls of template instantiations and compiler-written code in
the graph, v20 the emission facts in the references section:
header with
`IndexSummary` counts and a `{kind, offset, length, checksum}` table
for meta / graph / control flow / channels / references), so a
graph-only tool never
decodes (or checksums) the
call-site contexts, `info` reads the meta section alone, and
`graph_summary` reports the header counts (`ToolContext::summary`).
A read-only load that needs the control-flow section does not decode
its records either: the section keeps them sorted by call site with
by-caller / by-callee orders and a string-sorted id table after them,
and `ControlFlowIndex` answers every query from the mapped file
(`isMapped()`; `docs/control-flow-access.md` has the layout, the
measurements behind it, and the parity tests). `index`, `merge`, and
worker shards load `Mutable` and everything — the records decoded into
the resident form, the orders skipped. The bake's `--entry-point` list
is recorded in the meta and is the default for queries that pass none.

`main.cpp` peels the verb off argv (and peels direct compiler flags after
`--` into `fixedCompileArgs`) before `llvm::cl` runs: query verbs (and
`merge`) never touch `llvm::cl`; `index` shares the bake option block with
the verb-less `--bake-worker` form the isolated bake spawns (any other
verb-less invocation is a usage error, and `serve` prints its removal
notice). `megascope index` and `megascope merge` print a one-line JSON
summary on stdout.

**Buck2 & Bazel integration**:
- **Buck2** (`buck2/vycor.bzl`, `buck2/vycor.bxl`, `buck2/README.md`,
  `examples/buck2_megascope/`): Starlark rules (`vycor_tu_index`,
  `vycor_target_index`, `vycor_global_index`, `vycor_query`,
  `vycor_check`), wrapper macros (`vycor_cxx_library`,
  `vycor_cxx_binary`), and BXL scripts (`index`, `check`) that produce
  `.vycs`/`.mgsc`/`.shard.json` artifacts per translation unit, per
  library/binary target, and for the global index using `VycorIndexTSet`
  (`transitive_set`) and `megascope merge`.
- **Bazel** (`MODULE.bazel`, `WORKSPACE`, `bazel/vycor.bzl`,
  `bazel/aspects.bzl`, `bazel/settings.bzl`, `bazel/BUILD.bazel`,
  `bazel/README.md`, `examples/bazel_megascope/`): symmetric Starlark
  rules (`vycor_tu_index`, `vycor_target_index`, `vycor_global_index`,
  `vycor_query`, `vycor_check`), drop-in wrapper macros
  (`vycor_cc_library`, `vycor_cc_binary` + `vycor_cxx_*` aliases), and
  zero-config Bazel Aspects (`vycor_index_aspect`, `vycor_check_aspect`)
  propagating `VycorIndexInfo` `depset`s and `CcInfo` contexts with zero
  analysis-time `depset.to_list()` flattening.

TU selection (`SourceSelection.h/.cpp`): `--source` and `--source-list`
entries are unioned; `--source-re` and `--skip-paths` narrow the base
set. With no `--source`/`--source-list`/`--source-re` the base set is the
TU set recorded in the existing index (so a bare `index` refreshes what
was indexed instead of widening it), or every C/C++ entry of the
compilation database when there is no index yet (`--source-re .`
re-selects the whole database). Paths are canonicalized (absolute,
dots removed) before dedupe and filtering.

Warm start (`docs/index-provenance.md`): a TU is dirty when its own
stamp changed, its effective-input fingerprint differs, any file its
parse opened changed, or its last parse did not end `Indexed`. The bake
records each TU's opened files with the frontend's own stat
(`BakedIndexes::deps`, `SnapshotMeta::deps`/`tuDeps`, v9), its outcome
(`BakedIndexes::outcomes`, `SnapshotMeta::outcomes`, v10), and main.cpp
records the fingerprint (`callgraph/InputFingerprint.h`: the bake
environment — analyzer/toolchain identity, extra args, sysroot, PCH
dir, GCC install — plus every compile command of the file, verbatim and
in order; `SnapshotMeta::fingerprints`/`provenance`, v10);
`SnapshotIO::dirtyTUs` compares all of it and reports why
(`DirtyReport`). Files touched during the bake are recorded with an
unknown stamp (`markUnstableStamps`) and re-checked once. The dirty
set goes through the same parallel bake as a cold build (`bakeIndexes`,
or `bakeIsolated` under `--isolate-workers`) and is merged with the
shard `absorb`; past half the selection *changed* (retries excluded)
the cold bake runs instead, and an environment mismatch rebuilds
outright. Failed TUs are retried alongside any refresh that rewrites
the index, or on `--retry-failed`; a refresh that would otherwise touch
nothing leaves them as recorded. `--force` rebuilds regardless.
`index` first loads the meta section only (selection + dirty
check); an `index` with nothing to refresh reports the header counts
and never decodes the graph, otherwise the full mutable load follows
and the drop + dirty set is removed in one `removeTUs` call per index
(one scrub per affected adjacency vector for the whole set).
`scripts/warm-refresh-check.py` (ctest `warm_refresh`) checks that every
kind of refresh equals a clean rebuild, in-process and isolated.

Write integrity (`docs/index-provenance.md`, format v13): every index,
check journal header, and worker shard is published through
`writeFileAtomically` (`callgraph/AtomicFile.h`: unique temp file in the
target directory, fsync, rename, directory fsync; temp removed and
stream error cleared on failure). `index` holds an advisory
`flock` on `<index>.lock` (`IndexWriteLock`) from the meta load to the
save (`--no-wait` fails instead of waiting; readers never lock). A load
verifies the header checksum and each decoded section's xxh3 checksum
and names the damaged section in `SnapshotLoadStats::error`; `info`
verifies all sections (`SnapshotIO::verify`). A bake that parsed none of
its TUs is never saved (`SnapshotIO::unpublishableBake`; `index` exits
1). The check journal is truncated to its last valid record before
appending. `scripts/write-integrity-check.py` (ctest
`write_integrity`) and `tests/test_write_integrity.cpp` reproduce each
failure mode.

**30 tools** (CLI verbs; `batch` and `call` take the same names): `search_functions`, `lookup_function`, `get_callees`,
`get_callers`, `find_call_chain`, `query_exception_safety`,
`query_call_site_context`, `query_raii_scopes_at_callsite`,
`query_throw_propagation`, `query_all_path_contexts`,
`query_nearest_catches`,
`query_locks_held`, `query_same_lock`, `analyze_dead_code`,
`get_class_hierarchy`, `list_entry_points`, `graph_summary`,
`list_callback_sites`, `list_concurrency_entry_points`, `list_channels`,
`query_channel`, `query_channels_for_function`, `explain_ordering`,
`impact_of_change`, `audit_dependencies`, `explain_dependency`, `metrics`,
`lookup_symbol`, `get_references`,
`query_path_effects`.

Identical edges registered by multiple TUs (header-inlined code) are
**deduplicated at insert** with per-TU refcounting, so `removeTU` only drops
an edge when its last contributing TU is removed. Edge records store
interned string IDs internally; public queries materialize `CallGraphEdge`
with resolved strings. Path-walking tools (`find_call_chain`,
`query_locks_held`, `query_same_lock`, and the exception path tools)
share one traversal (`callgraph/PathSearch.h`), accept `max_paths` /
`max_depth` (edges) / `max_fan_in`, and report `stopReasons`,
`complete`, `exhaustive`, and `skippedHubs` (`docs/path-analysis.md`).

Every tool payload, on every transport, carries `status` (`ok`,
`ambiguous`, `usage_error`, `not_found`, `unavailable`) and
`indexScope` (the bake reference, `freshness`, and the requested /
indexed / partial / failed TU counts of the index answered from). Exit
codes derive from `status`, never from message text;
a universal exception verdict needs an exhaustive search and complete
coverage (`docs/result-contract.md`). A function identity the index
does not hold (no node, no edge endpoint) is `not_found` with
`didYouMean` suggestions, never an empty `ok`; the list tools without
their own bound page through `limit` / `offset` with a default cap
and report `total` / `truncated` / `nextOffset`.

### `ext` — Organization Extension Points

**Headers:** `include/vycor/ext/`
**Sources:** `src/ext/` (core), top-level `ext/` (fork-owned slot-in)

| File | Purpose |
|---|---|
| `Extensions.h/.cpp` | `ExtensionRegistry` (process-wide): custom `Check`s, custom `Metric`s, lock/channel type registration, effect functions, guard classifiers (feature flags). Macros `VYCOR_REGISTER_CHECK` / `VYCOR_REGISTER_METRIC` / `VYCOR_EXTENSION_SETUP` |
| `OrgConfig.h/.cpp` | `--org-config` JSON: declarative lock/channel types, feature-flag patterns, collapse paths, disabled checks, effect functions |

The scheme exists so organization forks never edit upstream files (clean
merges): declarative config goes in an org JSON passed as `--org-config`;
compiled hooks go in top-level `ext/*.cpp`, which CMake globs **directly
into the `vycor-cpp` and `vycor_tests` executables** (NOT `vycor_lib` —
static-init registrars in an archive would be dropped by the linker).
`ext/tests/*.cpp` is globbed into `vycor_tests`. `ext/examples/` is not
built. Full guide: `docs/EXTENDING.md`.

Key semantics:
- Custom check checks run per TU **after** the built-in `AnalyzerVisitor`
  (in `AnalyzerConsumer::HandleTranslationUnit`); one fresh instance per
  TU; emit `Diagnostic::Custom` with `checkName` (left empty, the
  analyzer fills in the emitting check's `name()`). The name is also the
  finding's fingerprint input and SARIF rule id (`check/Report.h`);
  filling `Diagnostic::entities` gives the fingerprint a stable identity
  (otherwise the message, minus line references, is used).
- Registry lock/channel types are merged into the CLI-built
  `LockTypeConfig`/`ChannelTypeConfig` in `main.cpp`
  (`mergeExtensionConfig`, CLI-first order, deduped) — so they participate
  in snapshot config-match/invalidation automatically.
- Guard classification (feature flags) happens at **query/serialization
  time** (`classifyGuard` in `megascope dump`, `query_call_site_context`,
  channel-site listings) — never at index time, so changing patterns does
  not invalidate snapshots. Annotation shape:
  `{"annotation": {"kind": "feature-flag", "name": "<flag>"}}` plus the
  existing `inTrueBranch` for on/off.

---

## CLI Entry Point

`src/main.cpp` uses LLVM's `CommandLine` library with three `cl::SubCommand`
objects (`prism` is gone; `vycor-cpp prism ...` prints the megascope
equivalents and exits 2):

```
vycor-cpp check     --build-path <dir> (--source <file>... | --source-list <file|->) [--list-checks] [--checks <spec>] [--checks-config <file>] [--threads <n>] [--checkpoint <file>] [--isolate-workers [--workers <n>] [--worker-timeout <s>] [--worker-memory-limit <MiB>]] [--org-config <file>]
                     [--format text|json|sarif] [--output <file>] [--fail-on note|warning|error|none] [--allow-parse-failures] [--baseline <file>] [--write-baseline <file>] [--patch-file <file|-> | --git-base <rev> [--git-head <rev>]] [--project-root <dir>] [-v]
vycor-cpp morph     --rules-json <file> --build-path <dir> --source <files...> [--dry-run]
vycor-cpp megascope index   --build-path <dir> [--source <file>...] [--source-list <file|->] [--source-re <regex>] [--skip-paths <pattern>...] [--index <file>] [--force | --retry-failed] [--no-wait] [--collapse-paths <pattern>...] [--org-config <file>] [--threads <n>] [--isolate-workers[=false]] [--worker-timeout <s>] [--worker-memory-limit <MiB>]
vycor-cpp megascope <tool>  [--index <file> | --build-path <dir>] [tool flags from its schema...] [--format json|ndjson|tsv] [--pretty]
vycor-cpp megascope <tool>  --build-path <dir> --source <file>... | --source-list <file|-> | --source-re <regex> [--skip-paths ...] [--collapse-paths ...] [--threads <n>] [--org-config <file>] [tool flags...]   # ephemeral: bake in memory, no index
vycor-cpp megascope batch   [--index <file>]      # NDJSON {"tool":..,"args":{..}} on stdin
vycor-cpp megascope dump    [--index <file> | --build-path <dir>] [--format ndjson|json] [--pretty]   # stream every call-site context and channel site
vycor-cpp megascope diff    --before <index> --after <index> [--to <name>] [--impact] [--no-context] [--allow-mismatch]   # semantic diff of two saved indexes
vycor-cpp megascope tools | info [--files] | help
vycor-cpp metrics   [--list-metrics] [--metrics <spec>] [--rollup symbol|template|file|tu|directory|target] [--target-map <file>] [--limit <n>] [--offset <n>] --build-path <dir> --source <file>... | --source-list <file|-> | --source-re <regex>   # = megascope metrics
```

`--org-config` (check/megascope) loads the organization config JSON
(see `include/vycor/ext/OrgConfig.h` and `docs/EXTENDING.md`); its
lock/channel types and collapse paths merge with the equivalent CLI flags.

**Named checks (check):** every analysis is a named check with a page
under `docs/checks/` (see `docs/checks/README.md` for the list, groups,
and defaults). Selection sources in order (later wins): a
`.vycor-check.json` found walking up from the working directory (or
`--checks-config <file>`), the `--checks=<spec>` flag (clang-tidy style:
`all,-noisy`, `-name` disables, groups expand), then the legacy toggles
(`--odr-diag`, `--coverage-diag`, `--dead-code`) as appended enables. The
resolver lives in `check/CheckSet.h/.cpp`; unknown names are a hard
error; main.cpp maps the resolved set onto `AnalysisOptions` booleans and
forwards it to isolated workers as `--checks=-all,<resolved...>`.
Organization checks (per-TU `Check` and merged-index `IndexCheck`,
`ext/Extensions.h`) join the same namespace via `name()`; org groups via
`registry.addCheckGroup`.

Each subcommand has its own scoped options (declared with `llvm::cl::sub(...)`).
To add a new subcommand, follow the pattern in `main.cpp`:
1. Declare a `static llvm::cl::SubCommand MyCmd("name", "description")`.
2. Declare option variables with `llvm::cl::sub(MyCmd)`.
3. Add an `if (MyCmd) { ... }` branch in `main()`.

### Indexed vs ephemeral

- **Indexed**: `megascope index` bakes a unified call graph + control-flow index into a file once; every tool verb (`megascope get-callers --name f`, `megascope batch`, `megascope dump`) loads it and answers. **Always index for security audits or multi-file analysis.**
- **Ephemeral**: the same verbs given `--source`/`--source-list`/`--source-re` (with `--build-path`) bake those TUs in memory per invocation and answer — quick single-file investigations, the role the removed `prism` subcommand played. The bake sees only the selected TUs, so cross-file callers are invisible.

### Edge Collapse (--collapse-paths)

`megascope` (`index` and the ephemeral query mode) accepts
`--collapse-paths` to reduce noise from header-inlined utility code. Patterns are path component substrings:

```bash
--collapse-paths Client/Math --collapse-paths Client/Core
```

This matches paths containing `/Client/Math/` or `/Client/Core/` as directory
components. Internal edges (both endpoints in collapsed paths) are skipped;
boundary edges (non-collapsed caller → collapsed callee) are preserved.

---

## Build System

`CMakeLists.txt` (top-level):
- Requires CMake 3.20+, C++17.
- Reads the project's own SemVer from the root `VERSION` file into
  `project(vycor-cpp VERSION ...)`, and `configure_file`s
  `include/vycor/Version.h.in` into `${CMAKE_BINARY_DIR}/generated/vycor/Version.h`
  (`VYCOR_VERSION_STRING` etc.) — `main.cpp` uses it to make `vycor-cpp
  --version` report both the vycor-cpp version and the embedded LLVM
  version. See `COMPATIBILITY.md` "Tagging and releases" for the version/tag
  scheme this feeds.
- Iterates `VYCOR_SUPPORTED_LLVM_VERSIONS` (currently `22 21 20 18`,
  newest first) via `find_package()`. The CI matrix in
  `.github/workflows/ci.yml` mirrors this list.
- Falls back to `extern/llvm-project` submodule if no system package matches.
- Disables RTTI (`-fno-rtti`) to match LLVM's default.
- A lightweight compatibility header (`include/vycor/compat/ClangVersion.h`)
  provides `VYCOR_LLVM_AT_LEAST(major)` for version-conditional code.
- See `COMPATIBILITY.md` for the support matrix, known API differences
  between supported LLVM majors, and the procedure for adding/removing a
  supported version.
- `install(TARGETS vycor-cpp RUNTIME DESTINATION ...)` in `src/CMakeLists.txt`
  supports `cmake --install` staging for release tarballs (no CPack — a
  single binary doesn't need it). Used by `.github/workflows/release.yml`.

`src/CMakeLists.txt`:
- Builds `vycor_lib` from `check/*.cpp`, `morph/*.cpp`, `callgraph/*.cpp`, `query/*.cpp`, `cli/*.cpp`, `impact/*.cpp`, `deps/*.cpp`, `metrics/*.cpp`, and `ext/*.cpp`.
- Builds `vycor-cpp` executable from `main.cpp` plus a `CONFIGURE_DEPENDS`
  glob of top-level `ext/*.cpp` (organization slot-in — attached to the
  executable, not the archive, so static registrars survive linking).
- Links against: `clangTooling`, `clangDynamicASTMatchers`, `clangASTMatchers`,
  `clangAST`, `clangBasic`, `clangFrontend`, `clangSema`, `clangSerialization`,
  `clangRewrite`, `clangToolingCore`, `LLVMSupport`.

`tests/CMakeLists.txt`:
- Resolves Catch2 v3.10+ in three tiers (no network at configure time):
  1. Pre-existing `Catch2::Catch2WithMain` imported target.
  2. `find_package(Catch2 3.10 CONFIG)` — system / vcpkg / Conan / Spack.
  3. `extern/Catch2` submodule (pinned to v3.10.0 upstream by default).
- Defines `PROJECT_SOURCE_DIR` for example file paths.
- Registers the Catch2 tests with their tags as ctest labels (`ctest -L
  worker_pool`). The `[crash-guard]` tests, which crash a parse on purpose,
  run with `tests/sanitizers/lsan-crash-recovery.supp` (the frames LLVM's
  `CrashRecoveryContext` abandons); nothing else is suppressed.

Sanitizer and fuzz options (top-level `CMakeLists.txt`):
- `VYCOR_SANITIZE` — `address,undefined` (ASan + UBSan, `-fno-sanitize-recover=undefined`
  so a UB report fails the test), `thread` (TSan; cannot combine with
  `address`), or any other `-fsanitize=` list. Applied to every target
  configured after it (project, tests, Catch2); the prebuilt LLVM
  libraries stay uninstrumented. Every executable of an ASan build links
  `src/compat/SanitizerDefaults.cpp` (`allow_user_poisoning=0`: LLVM's
  header-inline `BumpPtrAllocator` poisoning, compiled into both our
  instrumented objects and the uninstrumented LLVM libraries, otherwise
  reports use-after-poison inside clang; the file says why that is the
  narrowest switch).
- `VYCOR_FUZZ` — clang only: coverage instrumentation everywhere, the
  libFuzzer targets under `fuzz/` (`fuzz_snapshot`, `fuzz_checkpoint`,
  `fuzz_shard`, `fuzz_batch`) and `fuzz_seeds`, which writes their seed
  corpora from the test fixtures. Implies `VYCOR_SANITIZE=address,undefined`
  unless set. Target list, input formats, run commands, and the findings
  so far: `fuzz/README.md`.
- Both need the compiler's sanitizer runtimes (`libclang-rt-<N>-dev` for
  clang on Debian/Ubuntu).
- `VYCOR_FUZZ` picks `address,undefined` in a normal variable, not the
  cache: reconfiguring the same build directory with `-DVYCOR_FUZZ=OFF`
  builds unsanitized again.

---

## Build and Test Commands

```bash
# Configure (Release) — picks the newest supported LLVM automatically
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Configure against a specific LLVM version (e.g. 20)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/usr/lib/llvm-20

# Configure (Debug — faster iteration)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build the CLI tool
cmake --build build --target vycor-cpp

# Build tests
cmake --build build --target vycor_tests

# Run tests
cd build && ctest --output-on-failure

# Run a specific test tag
cd build && ctest -R "GlobalIndex" --output-on-failure

# End-to-end CLI output contract (also a ctest: `ctest -R cli_golden`):
# the built binary over examples/deep_chains vs examples/deep_chains/cli-golden/.
scripts/cli-golden.py --binary build/src/vycor-cpp
scripts/cli-golden.py --binary build/src/vycor-cpp --update   # accept an intended change
```

`scripts/cli-golden.py` compares each query's exit code and its stdout
as a sorted multiset of normalized lines (fixture and scratch paths
become placeholders, standard-library USRs are masked, records naming a
system-header location are dropped), so the goldens hold across hosts
and standard-library versions. Any change to a tool's payload, a verb's
flags, the ndjson/tsv shapes, or the exit codes shows up there; refresh
the goldens in the same PR. `scripts/bench.py --cli [--sections]`
measures the one-shot query latency the CLI model pays per call
(process start + index load + query), with the per-section load split;
`scripts/cf-access-bench.py` is the matched-workload comparison of two
binaries or index formats behind `docs/control-flow-access.md`.

The order of every list a tool emits is contractual
(`docs/deterministic-output.md`): the same sources baked in any TU
order, on any thread count, cold or warm, answer byte for byte the
same; `tests/test_determinism.cpp` compares raw payloads and
`scripts/warm-refresh-check.py` compares raw tool output over a warm
index against a clean rebuild. The validation corpus
(`corpus/cases/`, `scripts/corpus-run.py`, ctest `corpus` and
`corpus_selfcheck`; `docs/validation.md`) checks answers against
hand-written witnesses, negatives, and orders, and writes a report that
keeps quality and cost apart. Its `header_change`,
`compile_flag_invalidation`, and `change_impact` cases are before/after
patch pairs.
Benchmarks (`scripts/bench.py`) are run by hand, never by ctest.

```bash
# ASan + UBSan (or -DVYCOR_SANITIZE=thread), then the usual ctest
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DVYCOR_SANITIZE=address,undefined
cmake --build build-asan && (cd build-asan && ctest --output-on-failure)

# Fuzzing (fuzz/README.md)
cmake -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DVYCOR_FUZZ=ON -DVYCOR_BUILD_TESTS=OFF
cmake --build build-fuzz --target fuzz
build-fuzz/fuzz/fuzz_seeds fuzz-seeds
build-fuzz/fuzz/fuzz_snapshot fuzz-corpus/snapshot fuzz-seeds/snapshot -max_total_time=600
```

CI (`.github/workflows/ci.yml`) runs, besides the Debug build-and-test on
every supported LLVM: on the newest LLVM only, `release` (the shipped
Release configuration, full ctest including `cli_golden` and `corpus`),
`asan-ubsan` (full ctest), and `tsan` (the thread-pool tests by label, a
4-thread in-process bake of `examples/deep_chains` with queries, and one
corpus case). The sanitizer jobs set `log_path` in `*SAN_OPTIONS` and
end with a step that fails if any report file exists, so a report from a
subprocess worker (which the parent absorbs as a crashed TU) fails the
job too; filtered ctest runs pass `--no-tests=error`.
`.github/workflows/fuzz.yml` fuzzes, 60 s per target on a pull request
that touches code the target executes (the path lists in the workflow)
and 20 min each nightly, minimizes the nightly corpora (`-merge=1`)
before caching them, and uploads failing inputs. A sanitizer or fuzz finding is fixed with a unit
test that reproduces it; a suppression is only for a third-party (LLVM)
issue, scoped as narrowly as the tool allows, with a comment saying why.

Run tests from the project root, or ensure `PROJECT_SOURCE_DIR` is set
correctly (the CMake build sets it automatically via a compile definition).

---

## Adding a New Analysis to check

1. **If new data is needed globally**, extend `GlobalIndex` (`GlobalIndex.h`
   and `GlobalIndex.cpp`) with a new entry struct and lookup method.

2. **Collect data** by extending `IndexerVisitor` in `Indexer.cpp`:
   - Add a `VisitXxxDecl(clang::XxxDecl *decl)` method.
   - Populate the new `GlobalIndex` fields.

3. **Detect fragility** by extending `AnalyzerVisitor` in `Analyzer.cpp`:
   - Add a `VisitXxxExpr` or `VisitXxxDecl` method.
   - Query `GlobalIndex` and push `Diagnostic` entries.

4. **Add tests** in `tests/test_adl_ctad.cpp` following the existing pattern
   of constructing in-memory code strings and running `runToolOnCodeWithArgs`.

---

## Adding a New Transform to morph

1. Create a `TransformRule` struct with:
   - `matcherExpression`: a Clang dynamic matcher string
     (e.g. `"functionDecl(hasName(\"foo\"))"`)
   - `bindId`: the bind ID for the root node (e.g. `"fn"`)
   - `callback`: a `ReplacementCallback` lambda that receives the
     `MatchFinder::MatchResult` and returns `vector<Replacement>`

2. Add the rule to a pipeline pass:
   ```cpp
   TransformPipeline pipeline;
   pipeline.addPass({ rule1, rule2 });
   pipeline.execute(buildPath, sourceFiles, dryRun);
   ```

3. Test using the example files in `examples/` as TDD targets.

The JSON rules format for the `morph` CLI subcommand is not yet implemented
(see TODOs below). For now, rules are added programmatically.

---

## Key Design Patterns

### Two-Phase Analysis

`runAnalysis()` runs two separate `ClangTool` passes over the same file set:

```
Pass 1: IndexerActionFactory → IndexerConsumer → IndexerVisitor
  → fills GlobalIndex with all declarations across all TUs

Pass 2: AnalyzerActionFactory → AnalyzerConsumer → AnalyzerVisitor
  → reads GlobalIndex, walks each TU, emits Diagnostics
```

The two-pass design is necessary because a single TU cannot know what
declarations exist in other TUs that are not included.

### RecursiveASTVisitor

All AST traversal uses Clang's `RecursiveASTVisitor<Derived>` CRTP base.
Override `VisitXxxDecl` or `VisitXxxExpr` methods; return `true` to continue
traversal, `false` to stop.

### FrontendActionFactory

`ClangTool::run()` takes a `FrontendActionFactory*`. The pattern used here:

```
XxxActionFactory → creates XxxAction per TU
XxxAction → creates XxxConsumer
XxxConsumer::HandleTranslationUnit → drives XxxVisitor
```

### MatchFinder Callbacks

`MatcherEngine` uses Clang's `MatchFinder` with a nested `CallbackAdapter`
class that bridges between `MatchFinder::MatchCallback` and the user-supplied
`ReplacementCallback` function objects.

---

## Development Workflow

Changes land on `main` via pull requests. Feature branches use a topical
prefix (`fix/`, `feat/`, `docs/`, etc.) and a short description, e.g.
`fix/llvm-18-threadpool-compat`.

Commit convention: descriptive imperative messages, no ticket numbers required.

Long-lived `release/llvm-18`, `release/llvm-20`, `release/llvm-21`, `release/llvm-22` branches
track each supported LLVM major; bug fixes are cherry-picked from `main` to
each applicable release branch via the `backport/llvm-NN` PR label. See
`COMPATIBILITY.md` for the full cherry-pick policy.

---

## Cutting a Release

A pushed tag matching `vX.Y.Z` (on `main`) or `vX.Y.Z-llvmNN` (on a
`release/llvm-NN` branch) triggers `.github/workflows/release.yml`, which
builds Linux + macOS binaries for the relevant LLVM major(s), attaches them
as tarballs to a GitHub Release, and updates the `bearbones/homebrew-vycor-cpp`
tap. Bump the root `VERSION` file, commit, then tag and push — see
`COMPATIBILITY.md` "Tagging and releases" for the exact command sequence and
the full tag grammar. Test with `workflow_dispatch`'s `dry_run` input before
relying on a real tag push.

---

## Ephemeral Work Artifacts Policy (No Plans or Momentary Notes in VCS)

Temporary development artifacts must stay temporary or go into **GitHub Issues**, never into version control:
- **Never commit** momentary design notes (`docs/design-*.md`), task/work-package plans (`docs/plans/**`, `docs/tasks/**`, `docs/handoffs/**`), architecture/CLI reviews (`docs/*-review.md`), root-cause/diagnosis writeups, or roadmap/TODO scratch files (`ROADMAP.md`, `docs/issues/**`).
- **Keep `docs/` strictly for durable, user- or contributor-facing reference documentation** that describes the current system as built (`docs/checks/**`, `docs/EXTENDING.md`, `docs/megascope-usage.md`, and the contract specifications).
- **Track planned work, follow-ups, design proposals, and open questions in GitHub Issues** (`gh issue create` / `gh issue edit`).
- **Use untracked scratch directories** (`scratch/` or `/tmp/`) for temporary scripts, benchmark logs, and working notes during a session.

---

## Namespace

All types and functions live in `namespace vycor`. There are no nested
namespaces per feature — the feature folders are a filesystem organization
only, not a namespace boundary.

---

## External Dependencies

- **LLVM/Clang 18, 20, 21, or 22** — system package preferred (`find_package`),
  git submodule at `extern/llvm-project` as fallback. Selectable via
  `CMAKE_PREFIX_PATH=/usr/lib/llvm-NN`.
- **Catch2 v3.10+** (tests only) — resolved in three tiers: pre-existing
  imported target -> `find_package` -> `extern/Catch2` submodule.
  Network is not required at configure time. See README.md "Hermetic
  and internal-package-manager builds" for organizations using
  internal mirrors or alternate package managers.
- No other runtime dependencies.
- **macOS install path**: the self-hosted Homebrew tap
  `bearbones/homebrew-vycor-cpp` (`brew tap bearbones/vycor-cpp && brew
  install vycor-cpp`), not `homebrew/core` — see `COMPATIBILITY.md`
  "Tagging and releases". Each formula `depends_on "llvm@NN"`: Homebrew's
  LLVM formulas link Clang/LLVM as shared libraries, so that dependency is
  required at runtime, not just at build time.

`abi-layout-divergence` compares physical record layouts by canonical USR and
target ABI. Its worker/journal payload is version 17. Enabling it bypasses legacy
checkpoint reuse because headers and compile flags affect layout. See
`docs/checks/abi-layout-divergence.md`.
