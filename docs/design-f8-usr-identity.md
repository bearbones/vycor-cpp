# Design: USR-Based Node Identity (review F8)

Date: 2026-07-07
Status: approved direction (operator decision: sequenced AFTER F12);
fixtures-first — do not start the core swap until the precision test
suite exists and is red.
Scope: every layer — visitors, CallGraph, ControlFlowIndex, MCP tools,
snapshot (v6), tests.

## Motivation (what is wrong today)

`getQualifiedNameAsString()` is the universal node identity. Three
precision failures follow:

1. **Overload collapse.** `process(const char*)` and
   `process(std::string)` are one node; callers merge. A reachability
   or taint question answered against the merged node can be WRONG, not
   merely imprecise — the audit use case's worst failure mode.
2. **Template specialization collapse.** `parse<Json>` and
   `parse<Yaml>` merge the same way.
3. **Macro-expansion site collision.** Multiple call sites can share a
   `file:line:col` spelling; `ControlFlowIndex::bySite_` keeps one
   context per spelling (last wins).

## Identity design

- **Canonical id: the Clang USR** (`clang::index::generateUSRForDecl`,
  header `clang/Index/USRGeneration.h`) — unique, cross-TU-stable,
  encodes signature and template arguments. Example:
  `c:@F@process#*1C#` vs `c:@F@process#&1$@N@std@S@basic_string...`.
- **Display name stays first-class.** Every node carries both
  `usr` (identity) and `qualifiedName` (display). Humans and LLMs query
  by display name; the engine resolves.
- **Fallbacks** (nodes that have no usable decl/USR):
  - Lambdas: keep the existing synthetic
    `lambda#file:line:col#enclosing` scheme, used AS the USR (prefixed
    `vycor-lambda:` to keep namespaces disjoint). It is already
    identity-stable across phases by construction.
  - Synthesized ctor names (`Type::Type` from make_unique detection)
    and `<indirect>`: `vycor-synth:<current name>` prefix.
  - `generateUSRForDecl` returning failure: same `vycor-synth:` path.
- **Call-site identity** (fixes #3): context key becomes
  `(callSite spelling, callerUsr)` — a compound key in
  `ControlFlowIndex::bySite_` (id pair), disambiguating macro-shared
  spellings by enclosing function. `contextAtSite` keeps its
  spelling-only signature (returns the unique match or the first with a
  `collision` flag) plus a new precise overload.

## Layer-by-layer plan

### 1. Visitors (`CallGraphBuilder.cpp`, `ControlFlowContextVisitor.cpp`)
- Memoize `(usr, displayName)` per `Decl*` per TU (extend the existing
  per-TU name memo; USR generation costs ~µs and allocates, so memoize
  exactly like display names).
- Every `graph_.addNode/addEdge/...` call site passes both usr and
  display name. `functionReturns`, overrides, effectiveImpls,
  varCallSources_ all key on USR.

### 2. CallGraph
- `nodes_` keyed by usrId; `CallGraphNode` gains `usr`; adds
  `byName_ : displayNameId -> small vector<usrId>` (the disambiguation
  index, maintained in addNode).
- Edges/relations store usrIds (no struct change — SId semantics shift).
- Public queries: existing by-name APIs (`calleesOf(name)` etc.) resolve
  through `byName_`; **0 candidates → empty, 1 → proceed, N → the
  API returns all N merged PLUS a flag**; new `*ByUsr` precise variants
  for tools that have a USR in hand. `EdgeRef` unchanged (ids are ids).

### 3. ControlFlowIndex
- caller/callee fields become usrIds; compound site key as above.

### 4. MCP tools (the UX layer — design center, not an afterthought)
- **Resolution contract**: every tool that takes `name` also accepts
  optional `usr`. With `name` only:
  - 1 candidate → answer normally, include `"usr"` in the response.
  - N candidates → `isError: false` disambiguation response:
    `{"ambiguous": true, "candidates": [{usr, qualifiedName, file,
    line, signature-ish display}...]}` — the LLM picks and re-queries
    with `usr`. (Never silently merge; never hard-error.)
- `search_functions` results carry `usr` per match (the primary
  discovery path already returns candidates — it becomes the natural
  disambiguator).
- `graph_summary`/`analyze_dead_code`/path tools work in usr space
  internally; display names only at JSON emit.

### 5. Snapshot v6
- Node record: + usr string (usrs live in the same interner as
  everything else; the id-preserving v5 mechanics carry over unchanged —
  bump version, add field).

### 6. Tests — FIXTURES FIRST (gate for starting any of the above)
Write these before the core swap; they must FAIL against today's build:
- `examples/precision/overloads.*`: two `process` overloads with
  disjoint caller sets; assert `callersOf` (by usr) keeps them disjoint
  and the by-name API reports ambiguity.
- Template pair `parse<A>/parse<B>` with disjoint callers; same shape.
- Macro fixture: one macro expanded in two functions producing
  same-spelling call sites; assert two distinct contexts survive.
- Name-resolution UX: exact-unique, ambiguous-list, usr-bypass, unknown.
- Snapshot v6 round-trip with all of the above.

## Phasing (each lands green)

1. **PR A**: precision fixtures (marked expected-fail / tagged
   `[!shouldfail]` where the framework allows) + this design doc row in
   the review table.
2. **PR B**: identity core — visitors + CallGraph + ControlFlowIndex +
   snapshot v6 + by-name resolution in the graph API. Fixtures flip
   green; whole suite green.
3. **PR C**: MCP disambiguation contract + tool schema/description
   updates + `docs/mcp-usage.md`.

## Risks / open questions

- **Node-count growth**: every template instantiation becomes a node.
  Expect nodes ↑ (measure on the 938-TU testbed; if explosive, collapse
  instantiations of the same primary template behind a canonical USR
  and keep specializations distinct only when explicit).
  **RESOLVED (2026-07-07): measured NOT explosive; policy is
  keep-distinct.** 57,115 → 94,690 nodes (1.66×) on the 938-TU testbed,
  attribution exact via `--dump-nodes` + the cached pre-F8 v5 snapshot;
  67% of multi-instantiation primaries have differing caller sets, so
  the collapse fallback would destroy real precision. The residual cost
  is presentation-level (uncapped disambiguation lists on ~21 generic
  utility names — `llvm::cast` returns 858 candidates ≈ 188 KB); the
  follow-up is a candidate cap + grouped summary in the MCP layer, not
  identity collapse. Full numbers and policy:
  `docs/callgraph-mcp-review.md` §"Template node growth".
- **USR stability across Clang majors** (18/20/21 CI matrix): USRs are
  stable in practice within a major; snapshots are already
  version-gated and machine-local, so cross-version drift only costs a
  cold rebuild. Assert nothing about USR *content* in tests — only
  distinctness/stability within a run.
- **LLM ergonomics**: the disambiguation contract must be cheap for an
  agent loop (one extra round-trip max). Validate against the mcp-smoke
  harness with an ambiguous fixture.

## Acceptance

1. Precision fixtures green (and provably red pre-change, in PR A).
2. Full suite green, both CI matrix and local.
3. 938-TU bake: node count reported before/after; bake wall within
   ±10%; queries within noise (usr strings are longer — interner grows;
   measure RSS delta).
4. mcp-smoke run demonstrating the disambiguation flow end-to-end.
