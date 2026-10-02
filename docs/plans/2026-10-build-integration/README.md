# Call-graph build cost, and building the index with the compile

Prepared 2026-10-02 at `main` `c8c5488`. This is a study, not a record of
shipped behaviour. It answers two questions:

- What does building the megascope index cost, and where does that cost go?
- What would it take to produce each TU's index as an artifact next to its
  object file, in a Bazel or Buck2 remote-execution build, possibly as a
  Clang frontend plugin inside the compile itself?

Evidence comes from three sources:

1. A read of the bake pipeline at `c8c5488`.
2. New measurements on vycor-cpp's own sources (below).
3. A read of upstream Clang, Apple's `swiftlang/llvm-project` fork, the
   Buck2 prelude, rules_cc, and the REAPI proto.

Each claim is marked **measured**, **verified in code**, or **estimate**.
Figures from the 938-TU testbed come from earlier docs and are cited as
such; this study did not rerun them.

## Conclusions

1. **The bake is a Clang parse.**
   - Indexing a TU costs 1.05x a `clang++ -fsyntax-only` of it (range
     0.84–1.14 over 12 TUs, measured).
   - Clang's parse and Sema account for 90–91% of the instructions, of which
     template instantiation alone is 23%. Our three visitors account for
     6–8% (callgrind, measured).
   - Speeding up the visitors can win at most single-digit percent. Whole-bake
     cost moves only by parsing less often (caching, pairing with the
     compile) or parsing cheaper (PCH, modules).
2. **Pairing with the compile is cheap in CPU and expensive in coupling.**
   - Indexing inside the compile action (one parse) would add about 2–3% to a
     `clang -O2 -g` compile (estimate from the measured visitor share).
   - Indexing as a separate re-parse action adds about 36% CPU over that
     compile (measured: 68 s of indexing against 189 s of `-O2 -g` compiles
     on the same 12 TUs).
   - The in-compile route ties vycor to the exact compiler build. It cannot
     declare its output through Bazel's public C++ API, and every vycor
     release invalidates every cached object file.
3. **Recommended order:**
   1. Make the per-TU shard hermetic and add `shard` and `merge` verbs.
   2. Ship a compile-adjacent action: a Bazel aspect and a Buck2 subtarget
      that re-parse with the target's exact flags, cached and remotely
      executed independently of the object files.
   3. Offer the in-compile plugin only where its blockers do not apply (Buck2,
      or an organization-built clang), after the action has proven the shard
      format.
   The shard format and the merge are the same in both routes. Only the
   producer changes.
4. **The per-TU output is mostly not the TU's own code**:
   - 79% of the measured index file is the graph section.
   - Of the 39,752 nodes tallied by origin, 36.4k are declarations from
     LLVM/Clang and libstdc++ headers. The indexer adds a node for every
     non-template first declaration it sees, system headers included
     (verified in code, `CallGraphBuilder.cpp:264`).
   - Call-site contexts in headers are stored once per including TU: 6.33x on
     this tree; 7.06M contexts against 352,639 edges on the testbed.
   These multiply in every shard and every merge, so shrinking them is a
   prerequisite for shipping shards through a remote cache, not an
   optimization after it.
5. **The study found four wrong-answer bugs in the bake** (below), two of
   them confirmed by reproduction. They should be fixed before any
   build-system work builds on the shard.

## Measurements

### Environment and commands

- **Machine:** 4 vCPU Xeon 2.8 GHz (KVM), 15 GB RAM.
- **Toolchain:** Ubuntu 24.04, LLVM/Clang 18.1.3, g++ 13.3.
- **Build:** vycor-cpp at `a76a81d` (identical bake code to `c8c5488`),
  built `RelWithDebInfo`.
- **Workload:** the 45 TUs of `vycor-cpp/src`, with
  `--source-re '/vycor-cpp/src/'` over that build's
  `compile_commands.json`. These are heavy TUs: 11 of them include the Clang
  AST and Tooling headers and account for 65% of the bake.
- **Ratio table:** each TU's megascope time (best of 3, single-threaded,
  in-process) against `clang++ <same flags> -fsyntax-only`,
  `clang++ -O0 -g -c`, the original `clang++ -O2 -g -c`, and the original
  `g++ -O2 -g -c`.
- **Profile:** `valgrind --tool=callgrind` on a single-threaded in-process
  bake of 3 light TUs and 1 heavy TU.

### Bake runs (45 TUs)

| Run | Wall | Peak RSS |
|---|---|---|
| Cold, 1 thread, in-process | 157.6 s | 704 MB |
| Cold, 4 threads, in-process | 44.1 s | 2,175 MB |
| Cold, 4 workers, isolated (the default) | 45.4 s | parent 193 MB, worker ≤ 627 MB |
| Warm, nothing changed | 0.03 s | 53 MB |
| One TU added back, isolated | 2.90 s | 175 MB |

One-TU refresh breakdown: mutable load 0.22 s, parse 2.28 s, absorb 0.02 s,
save 0.17 s.

Testbed figures for comparison (938 TUs; `docs/index-provenance.md`,
`docs/control-flow-access.md`, `docs/megascope-cli-review.md`):

- Cold bake 150–190 s at 5.6 GB in-process.
- v12 index 558 MB.
- One touched TU: 10.4–10.7 s, of which 2.3 s is the load and 1.1 s the
  save regardless of the change.
- A header with 74 includers: 44–49 s.

### Index cost against the compile (12 sample TUs, seconds, single-threaded)

| | megascope | syntax-only | clang -O0 -g | clang -O2 -g | g++ -O2 -g |
|---|---|---|---|---|---|
| Sum | 68.0 | 64.9 | 101.0 | 189.3 | 225.7 |
| megascope ÷ column | — | 1.05 | 0.67 | 0.36 | 0.30 |

The whole-tree 4-thread bake (44 s) is 0.29x the g++ build of the same tree
(151 s).

### Where the instructions go (callgrind, inclusive)

| Bucket | light TUs | heavy TU |
|---|---|---|
| Clang parse + Sema | 90.2% | 91.1% |
| … of which template instantiation | 23.0% | 23.6% |
| Our visitors (`HandleTranslationUnit`) | 8.0% | 6.4% |
| … USR generation | 3.3% | 2.5% |
| Merge (`absorb` + per-TU teardown) | 0.8% | 1.6% |
| Snapshot save | 0.2% | 0.3% |

### What the index holds

Measured on the 45-TU index, 16.5 MB:

- **Graph section:** 12.98 MB, 79% of the file (interner 6.0 MB, nodes
  6.2 MB, edges 0.6 MB).
- **Nodes:** 42,403 nodes against 19,250 edges. Of the 39,752 nodes
  tallied by origin, 29.6k come from LLVM/Clang headers and 6.8k from
  libstdc++/libc.
- **Call-site contexts:** 26,148 records but only 18,529 distinct
  (caller, callee, site) triples.
  - Contexts in the TU's own file: 17,783 records, 17,207 distinct.
  - Contexts in headers: 8,365 records but only 1,322 distinct, a 6.33x
    duplication. The worst header site appears 34 times.
- **Header dedup:** removing the duplicate header contexts would cut this
  index's contexts by 29%. That is below package M's 40% gate. The testbed
  is far more header-heavy (7.06M contexts against 352k edges), so M's gate
  still has to be measured there.

## Pipeline complexity (verified in code unless marked)

Notation:
- A = AST nodes in a TU, headers included.
- C = call sites in user code.
- R = records in the whole index.
- S = interned strings.

| Stage | Cost | Notes |
|---|---|---|
| Parse | dominant | above |
| Three full `TraverseDecl` passes | 3·O(A) | `ControlFlowContextVisitor.cpp:1051-1055` (indexer, edges, control flow). Together 6–8% of instructions |
| Control-flow context per call site | O(depth × context bytes) | deep-copies enclosing try scopes (with handler summaries), guards and every live RAII local. Storage is deduplicated through set tables, CPU is not |
| USRs | each visitor has its own `UsrCache` | the same declaration's USR is generated up to 3× per TU |
| Absorb into the master | O(TU records), serialized | one mutex per index, held for the whole merge |
| Isolated path | each record encoded and decoded twice | the worker writes a full v13 save, including the read-only lookup orders the parent never reads. The parent then decodes and re-interns everything |
| Save | O(S log S + R) | string sort, four counting-sort orders, xxh3 over every byte |
| Warm refresh | O(R) + dirty parse | full mutable load and full save for any change. 3.4 s fixed on the testbed |
| Header code | contexts × includers | edges and channel sites are deduplicated across TUs, contexts are not (`ControlFlowIndex.cpp:236-264`) |

The visitors are not where the time goes, but two things in them scale
badly:

- **Context snapshotting** copies the whole enclosing context at every call
  site.
- **The assertion-guard list never shrinks** (bug 1 below): every call site
  after an assertion in a TU carries every earlier assertion, which is
  O(C × assertions) per TU.

## Bugs found by this study

| # | Bug | Status | Effect |
|---|---|---|---|
| 1 | `assertionGuards_` is appended to and never cleared (`ControlFlowContextVisitor.cpp:437`, read at `:638`) | **confirmed by reproduction** | After `CHECK(x > 0)` in `first()`, a call in `second()` reports the guard `x > 0`. Every later call site in the TU inherits every earlier assertion: wrong guards and quadratic snapshot cost |
| 2 | `addDerivedClass`, `addMethodOverride`, `addEffectiveImpl` and `addFunctionReturn` ignore their TU argument, and `removeTUs` never touches those maps (`CallGraph.cpp:448-600`, `:757`) | **confirmed in code** | After a warm refresh that removes an override (or drops a TU), virtual-dispatch fan-out and function-return joins still use the stale relation until a cold rebuild. `scripts/warm-refresh-check.py` does not cover the case |
| 3 | 2,885 contexts (696 distinct) carry an empty call-site path, such as `:527:1` in libstdc++'s `__glibcxx_rwlock_rdlock` | **confirmed in output** | System-header code leaks past the system-header filter through macro or builtin spellings |
| 4 | The control-flow visitor has no lambda boundary: calls inside a lambda body take the enclosing function as caller and inherit its try, guard and RAII context. The edge visitor attributes the same calls to the lambda node | **inferred from code, not reproduced** | A context and its edge may disagree on the caller, and a lambda run later (a callback, a thread) is reported as protected by its creator's `try` |

Two inconsistencies, not wrong answers:

- `graph_summary.edgeCount` counts `calleesOf` over nodes (19,193) while
  `info` counts stored edges (19,250).
- Index files differ byte-for-byte between thread counts. The output
  contract (`docs/deterministic-output.md`) allows this, but remote caching
  of shards and merged indexes needs byte determinism.

## The per-TU shard

Since the single-parse bake, every edge is produced from one TU's AST alone:
- **Virtual-dispatch fan-out** is expanded at query time (`CallGraph::calleesOf`/`callersOf`).
- **The function-pointer-through-return join** is also done at query time.
- **The edge visitor reads nothing global** (verified,
  `CallGraphBuilder.cpp:620-625`, `:772-783`).

So a shard is already almost a pure function of (TU, compile command, vycor
configuration):

| Fact | Local to the TU? | What the shard needs |
|---|---|---|
| Nodes, edges, control-flow contexts, channel sites | yes | relative paths (below) |
| Hierarchy, overrides, effective impls, function returns | yes; the merged index is their union | a contributor TU per relation, so a merge or refresh can drop them (fixes bug 2) |
| Collapse filter, lock and channel types | depend on configuration | the configuration becomes part of the action key, not shard content |
| Interner ids, storage order | run-dependent | a canonical order at write time (sorted strings, sorted records) |
| TU and dependency mtimes, `bakeStartNs`, unstable stamps | host-dependent | drop from the shard. Under a build system, staleness is the build system's job |
| Compile-command fingerprint | contains `directory` and absolute paths | relativize, or drop when the action key already covers it |
| Outcome text (`signal 11`) | host-dependent | keep the status, normalize the detail |

Hermeticity hazards (verified in code):

- Absolute, un-canonicalized paths appear in:
  - call-site strings;
  - lambda USRs (`vycor-lambda:…#<file:line:col>`);
  - `tuPath` on every context;
  - node `file`;
  - try, guard and RAII locations.
- The same header reached through two `-I` spellings yields two keys.
- Clang's own USRs are safe: internal-linkage declarations embed only the
  file's basename (`USRGeneration.cpp` `printLoc`).

The shard should store paths relative to the execution root or workspace,
with a remap table like the one Apple's index store uses (`PathRemapper`).
It should store no stamps, and ids in canonical order. The merged index can
then be rebuilt byte for byte from the same shards.

## Ways to produce shards

### A. Separate pass (today)

`megascope index` re-parses every selected TU on the developer or CI host,
in-process or in isolated workers. It needs no build-system integration and
does its own warm refresh. It cannot use remote execution or the remote
cache, and its flags can drift from the real build's when the compilation
database is stale. Keep it as the fallback for projects that use neither
Bazel nor Buck2, and as the reference the other producers are checked
against.

### B. Compile-adjacent action

One action per TU, next to the compile action and using the same flags. It
runs `vycor-cpp megascope shard` (new) with `-fsyntax-only` semantics and
declares one output, the shard.

- **Bazel:** an aspect over `cc_library`/`cc_binary` that reads the
  target's compilation context and toolchain flags, the same way
  `bazel_clang_tidy` and the Kythe extraction aspect do. The shards land in
  an output group.
- **Buck2:** a subtarget on the prelude's cxx rules. The prelude already
  defines `[index-store]`, built by a separate `-fsyntax-only
  -index-store-path` action and merged per target and then over the
  transitive closure through a tset (`prelude/cxx/index_store.bzl`,
  `prelude/apple/apple_library.bzl`). That is the exact topology vycor
  needs.

Properties:

- **Cost:** a second parse, measured at about 36% of a `clang -O2 -g`
  compile's CPU and 30% of g++'s. Under remote execution this is parallel
  capacity, not added latency.
- **Cache keys:** the shard's key covers the sources, headers and flags plus
  the vycor binary and configuration. A vycor release invalidates shards
  only, never object files.
- **Compilers:** works for any compiler. A GCC-built project's flags have to
  be translated for Clang, which vycor already does for the GCC install
  directory.
- **Crashes:** a crashing TU fails or degrades only its own action, with no
  poison or bisect machinery.
- **Staleness:** handled by the build system's action cache. vycor's
  dependency stamps and fingerprints become unnecessary in this mode.

### C. In-compile plugin

The plugin is a `PluginASTAction` registered as `AddBeforeMainAction`. It
runs on the complete post-Sema AST before codegen consumes it, with
end-of-TU template instantiation already done.
- `AddAfterMainAction` would run after the backend has written the object
  and forces the AST to be kept alive (`ClearASTBeforeBackend = false`), so
  it is the wrong choice here (verified in upstream
  `FrontendAction.cpp`/`CodeGenAction.cpp`).
- The plugin is loaded with `-fplugin=libvycor.so`, gets arguments through
  `-fplugin-arg-vycor-…`, and derives its output name from `-o` the way
  `-fstack-usage` derives `<obj>.su`.

Cost: about 2–3% over a `clang -O2 -g` compile (estimate: the measured
6–8% visitor share of a parse-only bake, scaled by the measured 0.36
bake/compile ratio).

Blockers:

1. **Declared outputs.** Remote execution discards any file that is not a
   declared output (REAPI `output_paths`), and so does sandboxing. So a
   plugin writing `foo.vycor` next to `foo.o` loses it on remote runs and on
   cache hits.
   - **Bazel:** `cc_common.compile` exposes no way to declare an extra
     output. The `additional_outputs` parameter in rules_cc's compile code
     is private and used only for C++20 modules
     ([bazel#22036](https://github.com/bazelbuild/bazel/issues/22036),
     open). A toolchain feature can add the `-fplugin` flag but cannot
     declare an output.
   - **Bazel workarounds**, both unproven:
     - patch rules_cc;
     - a compiler wrapper that runs clang with the plugin and then folds the
       shard into the declared `.o` as an excluded section
       (`objcopy --add-section`), so a cached object carries its shard.
   - **Buck2** already attaches extra outputs (`-ftime-trace` JSON, `.dwo`,
     remarks) to the compile action as hidden outputs
     (`prelude/cxx/compile.bzl`), so a `.vycor` output there is a small
     prelude change.
2. **ABI coupling.** A plugin loads only into the exact Clang build it was
   compiled against: same version, configuration, ABI-breaking-checks
   setting and C++ standard library.
   - Apple's Xcode clang does not support plugins.
   - A statically linked Windows clang has no plugin support.
   - GCC has no Clang plugins.
   - Distro clang (linked against `libclang-cpp.so`) and LLVM release
     tarballs (as used by toolchains_llvm) need different plugin builds.
   - The alternative is a "clang + vycor" compiler binary with the consumer
     linked in statically. That removes the loading problem, but every vycor
     change becomes a compiler change.
3. **Cache blast radius.** The plugin and its flags are inputs to every
   compile action. Enabling vycor, or upgrading it, misses the cache on every
   object file, and builds with and without vycor never share objects.
4. **Failure coupling.** A vycor crash fails the compile unless the plugin
   catches everything and writes an empty shard with an error status. The
   in-process crash guard cannot do that for a real fault.

Precedent:

- Apple's index-while-building (`-index-store-path`) is the closest. It
  wraps the codegen action, writes per-TU unit files and per-file records,
  and deduplicates header records by content hash at write time (a record
  that already exists is not rewritten).
- It never landed in upstream LLVM ([D39050](https://reviews.llvm.org/D39050)).
  It lives only in Apple's fork.
- Even Buck2 runs it as a separate `-fsyntax-only` action rather than inside
  the compile.

### Comparison

| | A. separate pass | B. compile-adjacent action | C. in-compile plugin |
|---|---|---|---|
| Extra CPU per TU | 1 parse (on the host) | 1 parse (remote, parallel) | ~2–3% of the compile (estimate) |
| Remote execution / cache | no | yes | yes, if the output can be declared |
| Bazel | n/a | aspect, no rule changes | rules_cc patch or object-section wrapper |
| Buck2 | n/a | subtarget (precedent: `[index-store]`) | prelude hidden output (precedent: `-ftime-trace`) |
| Compilers | Clang flags | any (GCC via flag translation) | the exact Clang build only |
| vycor upgrade invalidates | everything (re-bake) | shards only | every object file |
| Staleness detection | vycor stamps and fingerprints | the build system | the build system |

## Merging shards

The final product is the existing v13 index, so the query verbs and
`docs/result-contract.md` do not change. The merge is `absorb`: linear in
records, with per-TU refcounts that already handle duplicates.

- **Topology:** merge per target, then over the transitive closure (Buck2's
  index-store topology). The per-target merges cache, and a changed TU
  re-runs its target's merge and the final one.
- **One flat merge** over 10k shards works but re-runs entirely on any
  change.
- **Header facts:** without content addressing, every level re-merges the
  same header facts. There are two known ways to do it:
  - Apple's store: the record name is a hash of the content, and existing
    records are not rewritten.
  - scip-clang: a header whose expansion is identical everywhere is emitted
    by exactly one TU.

  vycor's version: a TU's shard references header facts by
  `(header path, content hash)` and the merge keeps one copy. Package M's
  dedup key is the same idea applied in memory.
- **Coverage and provenance:** the merged index records the shard set and
  each TU's outcome as today. Freshness becomes "baked by build X" rather
  than "stamps checked".
- **Warm refresh** in this mode is the build system re-running changed
  shard actions plus the merge. vycor's own refresh path stays for mode A.

## Proposed packages

Same execution rules as `docs/plans/2026-09-hardening/README.md`. Snapshot
format versions are allocated at merge time.

| ID | Package | Depends on |
|---|---|---|
| Q | **Bake correctness:** fix bugs 1–4, each with a reproducing test. Add a warm-refresh scenario that removes an override, and make `graph_summary.edgeCount` agree with `info` or document the difference | — |
| R | **Shard size:** add a node only when it is referenced by an edge, defined in user code, or part of a user class's hierarchy. Measure the graph section before and after. Fold in package M's measurement on the testbed, since the shard design depends on its answer | Q |
| S | **Hermetic shard + verbs:** a shard format (relative paths with a remap table, no stamps, canonical order, relations tagged by contributing TU); `megascope shard --compile-command …` (one TU in, one shard out); `megascope merge` (k-ary, shards or merged indexes in, v13 out); content-addressed header facts. Shards from the same inputs must be identical byte for byte (test); merging shards must answer exactly what a direct bake answers (corpus, CLI goldens) | Q, R |
| T | **Build integration (route B):** a Bazel aspect and a Buck2 subtarget with example workspaces under `examples/`, per-target and transitive merges, and a remote-cache hit test. Measure cold and incremental cost against mode A on one real project | S |
| U | **Plugin prototype (route C)**, measurement-gated: an `AddBeforeMainAction` plugin built against LLVM 18 that writes the same shard; the Buck2 prelude change; the measured overhead over a plain compile. Go/no-go on whether the 2–3% saving is worth the coupling | S; T for the comparison |

Q is independent of the rest and fixes wrong answers today. It should go
first.

## Open questions

- Which build system matters first, Bazel or Buck2? Route C is realistic
  only on Buck2.
- Is GCC support required? It rules out C as the only producer.
- How large is the testbed's header duplication, measured (M's gate)? It
  decides whether content-addressed header facts are essential or a later
  optimization.
- Should mode A keep its own warm refresh once B exists, or become a
  "re-run the shard actions locally" driver over the same verbs?
