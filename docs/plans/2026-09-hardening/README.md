# Hardening, performance, and usability: dispatch guide

Prepared 2026-09-23 from a read-only review of `main` at `8f7d68e`, after
packages A–F of [2026-09-next](../2026-09-next/README.md) landed (G, the
optional morph contract, is still open and is not repeated here). These are
implementation plans, not claims that the proposed capabilities exist.

No code was built or measured for this review. File:line citations were read
at `8f7d68e`; the high-severity items were checked directly against the
source. Figures quoted from `docs/control-flow-access.md` and
`docs/index-provenance.md` are historical; anything else numeric is marked as
an estimate. Each package's first step re-establishes its evidence.

## Objective

Stop the tool from giving silently wrong answers (a torn or emptied index, an
`ok` status for a misspelled name), stop it from hanging, make `anneal`
usable as a CI gate, and remove the largest remaining storage and query
costs.

## Packages and dependencies

| ID | Package | Kind | Start | Completion depends on |
|---|---|---|---|---|
| H | [Index write integrity](H-index-write-integrity.md) | resilience | Now | — |
| I | [Crash and hang containment](I-crash-hang-containment.md) | resilience | Now | — |
| J | [MCP server robustness](J-mcp-server-robustness.md) | resilience/usability | Now | I (reindex_tu crash path) |
| K | [Answer honesty and bounded results](K-answer-honesty-bounds.md) | usability | Now | — |
| L | [anneal as a CI gate](L-anneal-ci-gate.md) | usability | Now | — |
| M | [Control-flow context dedup](M-control-flow-dedup.md) | performance, measurement-gated | Measurement now | H (format version) |
| N | [Query-time algorithms and bake timing](N-query-algorithms.md) | performance | Now | — |
| O | [Onboarding, packaging, docs drift](O-onboarding-packaging.md) | usability | Now | — |
| P | [CI hardening: sanitizers and fuzzing](P-ci-hardening.md) | resilience | Now | Final pass after H, I, J |

Suggested order when slots are limited: **H, K, I** first (small changes,
each fixes a silently wrong result or a hang), then **J, L, P**, then **M, N,
O**. M's first step (count distinct control-flow keys on the testbed) is one
command and should run early regardless, because it decides whether M is
worth doing.

## Deferred (not dispatched)

Recorded so they are not rediscovered. Revisit after M lands, since M
shrinks the data each of these touches.

- **Map the graph section like the v12 control-flow section.**
  `readInternerTable` copies every string, then rebuilds the hash index (about
  285 ms of the 0.35 s graph load, per docs/control-flow-access.md), and nodes
  keep their own `std::string` copies of `usr`/`qualifiedName`. The graph
  section is now the floor for every one-shot query.
- **Stream-merge warm refresh.** A one-TU refresh measured 10.7 s, with the
  parse about 2.2 s; the full load (2.3 s) and save (1.1 s) are fixed
  overhead (`src/main.cpp:1264`, `Snapshot.cpp:301-669`).
- **Single-parse anneal.** Phase 2 re-parses every TU (`Analyzer.cpp:1396-1415`,
  `1528-1537`) although the default AST checks only read string facts that
  phase 1 could record. Estimated up to 2× anneal wall time; large effort.
- **Compact resident index memory.** `vector<size_t>` positions (u32 would
  do) and int-sized enums in `StoredEdge`.

## Shared execution rules

The rules in [2026-09-next/README.md](../2026-09-next/README.md#shared-execution-rules)
apply unchanged: isolated worktree and topical branch from current
`origin/main`, an early interface note for anything another package consumes,
CLI and MCP stay adapters over `src/query/`, self-review, relevant tests plus
CLI goldens, a focused PR, and the full LLVM matrix before merge.

Additionally:

- A package that changes a tool payload, exit code, or flag refreshes the CLI
  goldens (`scripts/cli-golden.py --update`) and the corpus witnesses in the
  same PR, and says so in the PR body.
- Snapshot format versions are allocated at merge time. H and M both change
  the format; whichever merges second takes the next number and rebases.
- Resilience fixes land with a test that reproduces the failure first
  (a torn write, a hung worker, a garbled frame). A fix without a
  reproducing test is incomplete.

## Shared-file ownership

| Surface | Lead owner | Others |
|---|---|---|
| `Snapshot.cpp` save path, header, checksum | H | M (context section layout) |
| `Checkpoint.cpp` journal append/recovery | H | L reads findings only |
| `WorkerPool.cpp` spawn, timeout, empty-bake fallback | I | H (save refuses an empty bake) |
| Crash guard in `CallGraphBuilder.cpp` / `ControlFlowContextVisitor.cpp` | I | — |
| `McpProtocol.cpp`, `McpServer.cpp` | J | I owns `reindexTU`'s crash handling |
| `Identity.cpp`, result paging in `src/query/` | K | J adds MCP annotations only |
| anneal output/exit code in `main.cpp`, finding identity | L | — |
| `ControlFlowIndex.*` storage | M | — |
| `PathSearch.cpp`, `DeadCodeAnalyzer.cpp`, `ImpactSearch.cpp` | N | — |
| README, AGENTS.md, release workflow, `doctor` | O | every package updates docs it changes |
| `.github/workflows/ci.yml`, fuzz targets | P | — |

## Required handoff

As in 2026-09-next: branch and PR, completed scope, public contract changes,
verification evidence (including the reproducing test for resilience items),
limitations, shared-file changes, and remaining dependencies.

## Dispatch prompts

- **H:** Implement `docs/plans/2026-09-hardening/H-index-write-integrity.md`. Reproduce the concurrent-writer tear and the empty-bake overwrite first, then make every index and journal write atomic, locked, checksummed, and error-checked.
- **I:** Implement `docs/plans/2026-09-hardening/I-crash-hang-containment.md`. Replace the `siglongjmp` crash guard, add worker timeouts that feed the existing poison/bisect path, clamp user-supplied search limits, and clean up on SIGINT/SIGTERM.
- **J:** Implement `docs/plans/2026-09-hardening/J-mcp-server-robustness.md`. Fix framing resync and size caps, make `reindex_tu` atomic, bound the query cache, answer `initialize` before the bake, and add the protocol annotations.
- **K:** Implement `docs/plans/2026-09-hardening/K-answer-honesty-bounds.md`. Return `not_found` with suggestions for unknown names, add a shared paging contract to the unbounded list tools, and update goldens and corpus.
- **L:** Implement `docs/plans/2026-09-hardening/L-anneal-ci-gate.md`. Give anneal exit codes, per-TU outcomes, JSON and SARIF output, stable finding fingerprints, baselines, and inline suppressions.
- **M:** Execute `docs/plans/2026-09-hardening/M-control-flow-dedup.md`. Measure the duplication ratio first and publish a go/no-go; implement deduplicated contexts only if the gate passes.
- **N:** Implement `docs/plans/2026-09-hardening/N-query-algorithms.md`. Narrow the path-search corridor, move dead-code analysis to ids, fix the small query-time waste, and add parse-vs-visitor bake timing.
- **O:** Implement `docs/plans/2026-09-hardening/O-onboarding-packaging.md`. Add bake progress and `vycor-cpp doctor`, make release binaries find clang's builtin headers on a clean machine, unify source selection, and fix the listed doc drift.
- **P:** Implement `docs/plans/2026-09-hardening/P-ci-hardening.md`. Add sanitizer and Release CI jobs and fuzz targets for the four binary/stream parsers; triage what they find.
