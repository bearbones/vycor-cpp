# Semantic diff and change impact

Status: implemented on `main`. Owner: package E of
`docs/plans/2026-09-next/`. This page is the contract for `megascope
diff` (two indexes compared) and `impact_of_change` (one index, a
changed set, the callers it reaches). The envelope is
`docs/result-contract.md`; the path facts are `docs/path-analysis.md`;
the order rules are `docs/deterministic-output.md`.

## What it answers

- **What changed between two bakes, as program relationships?**
  Functions that appeared or disappeared, calls that appeared,
  disappeared, or changed attribute, and call-site protection or lock
  facts that changed — never a line number.
- **Is there a new route to a target?** The paths from the entry points
  to one function, before and after, compared as sequences of edges.
- **Who is affected by a change?** The callers, transitively and within
  a budget, of a set of changed functions — from names, from a unified
  diff, or from the semantic diff itself — with a witness path each.

Impact is a *candidate* set: a function is affected when a call chain
from it reaches a changed function within the declared bounds. It is
not proof that the function's behaviour or a test changes, and it is
not proof that anything outside the set is unaffected.

## Identity across indexes

Two indexes never share interner ids or insertion order; nothing here
uses them. A function's cross-index identity is its **comparison key**:

| Node | Key | Note |
|---|---|---|
| a declaration with a USR | the USR string | signature and template arguments are in the USR, so an overload or an instantiation is its own function; a file-local (`static`) function's USR names its file, so moving one between files is a removal and an addition |
| a lambda (`vycor-lambda:lambda#file:line:col#enclosing`) | `vycor-lambda:<file>#<enclosing>#<ordinal>` | the ordinal is the lambda's rank by (line, col) among the lambdas of the same file and enclosing function **in that index**, so a line shift keeps the identity. When the two indexes hold a different number of lambdas for one (file, enclosing) pair, every lambda of that pair is **ambiguous** (below) |
| a synthesized identity (`vycor-synth:...`) | the string | as is |

A function is **moved** when its key is on both sides and only its
file or line differs; a move is not a change (it is counted in
`summary.movedFunctions`, listed under `moves` with `--include-moves`).
A function is **renamed or moved across files** only as a *candidate*:
a removed and an added function that share a display name are paired
under `identity.renameCandidates`, and stay counted as one removal and
one addition. The diff never asserts continuity it cannot see.

**Ambiguous** identities (`identity.ambiguous`) are excluded from every
change list: their edges and contexts are neither added nor removed,
and the record says how many were withheld. The one source today is a
lambda group whose size changed.

## Relationships

A **relationship** is `(caller key, callee key, edge kind)`; its value
is the multiset of its call sites' attributes `(confidence, execution
context, indirection depth)`. Call-site spellings are witnesses, never
identity, so a line shift changes nothing. The edges compared are the
ones the tools answer with (`CallGraph::calleesOf` of every node):
virtual-dispatch expansions and function-pointer-through-return joins
are relationships like any other.

| `change` | When |
|---|---|
| `function_added` / `function_removed` | the key is on one side only |
| `call_added` / `call_removed` | the relationship is on one side only |
| `call_changed` | on both sides with a different site count or attribute multiset (a second call site, a confidence that moved from `Plausible` to `Proven`, a call that became a thread spawn) |
| `context_changed` | on both sides with a different **context signature** multiset (below); reported whether or not the relationship is also `call_changed` |

A **context signature** is what the exception and lock tools read at a
site, without its locations: whether a try/catch encloses the call and
the handler types innermost first (`...` for catch-all, `!` suffix when
the handler rethrows), the caller's `noexcept` spec, whether the site is
inside a catch block, the live lock types, and the enclosing guards'
condition texts. Contexts are compared only when both indexes were
loaded with their control-flow section (`diff` does so unless
`--no-context`); a site with no indexed context on one side and a
context on the other is a change, with `absent` in place of the
signature.

Every record names the functions by key and display name and carries
the after-side call sites (or the before-side ones for a removal) as
witnesses: `sites: [{callSite, confidence, executionContext?}]`,
bounded by `--max-sites` per record.

## Comparability

Before facts are compared, the two bakes are:

```json
"comparability": {
  "comparable": true,
  "absenceReliable": false,
  "reasons": ["after: 1 requested TU failed (/src/broken.cpp)"],
  "analyzerSame": true, "toolchainSame": true, "configSame": true,
  "contextsCompared": true,
  "before": {"bake": "...", "requested": 4, "indexed": 4, "partial": 0,
             "failed": 0, "complete": true},
  "after":  {"bake": "...", "requested": 5, "indexed": 4, "partial": 0,
             "failed": 1, "complete": false},
  "tusOnlyBefore": [], "tusOnlyAfter": ["/src/broken.cpp"],
  "failedBefore": [], "failedAfter": ["/src/broken.cpp"]
}
```

- `configSame`: collapse paths, lock type config, and channel types
  match. When they do not, the edge sets differ for reasons that are not
  the sources', so the comparison is **refused**: `status:
  unavailable`, exit 3, unless `--allow-mismatch`, in which case it is
  labelled and runs.
- `analyzerSame` / `toolchainSame` (`IndexProvenance`): a different
  analyzer or Clang can model the same sources differently; labelled,
  never refused.
- `absenceReliable`: both indexes cover every requested TU and request
  the same TU set. When false, a function or call present on one side
  only may be a coverage gap, and each `function_added` /
  `function_removed` record whose file is a TU absent or failed on the
  other side says so in `explanation` (`tu_absent_before`,
  `tu_absent_after`, `tu_failed_before`, `tu_failed_after`). Files are
  matched to TU paths by path suffix, since node files are spelled as
  the compile command spelled them.

The payload's `indexScope` cites the **after** index; the before
index's scope is under `comparability.before`.

## Routes (`--to`)

With `--to NAME` (or `--to-usr`, `--from` as for `find_call_chain`)
`diff` runs the bounded path search on both indexes with the same
limits and compares the paths as sequences of `(caller key, callee key,
kind)` hops:

```json
"routes": {"target": "sink", "before": {"pathCount": 1, "complete": true,
           "exhaustive": true, "targetKnown": true}, "after": {...},
           "added": [[hop, hop]], "removed": [], "unchanged": 1,
           "complete": true}
```

`routes.complete` is false when either search was cut (`docs/
path-analysis.md`); an added route is then a route the before search
did not enumerate, which may or may not have existed. A name that is
ambiguous on either side returns the `ambiguous` payload for that side.

## Impact

`impact_of_change` runs over one index (the after index under `diff
--impact`). The changed set comes from, in order of precision:

| Input | Maps to | `via` |
|---|---|---|
| `changed` (names or USRs) | the function | `argument` |
| `patch` (unified diff text; `--patch-file F` and `--git-base A --git-head B [--repo DIR]` on the CLI produce it) | see below | `call_site`, `definition`, `extent`, `file` |
| `diff --impact` | every function on either side of a `call_*` or `context_changed` record's caller, plus added and removed functions | `diff` |

A patch hunk's after-side line range (its before-side range for a pure
deletion, taken at the hunk's after-side position) is mapped by:

1. **`call_site`**: an indexed call site (any edge's `file:line:col`)
   lies in the range — its caller changed. Exact.
2. **`definition`**: a function's recorded location lies in the range.
   Exact for the function whose header changed.
3. **`extent`**: the range lies between a function's recorded location
   and the next recorded function location in the same file. The index
   records where a function starts, not where it ends, so this is an
   **estimate** (a change to file-scope code after a function's end is
   attributed to that function).
4. **`file`**: the file has indexed functions but the range precedes
   every one of them (file-scope declarations, includes): every
   function of the file is a candidate, labelled `file`.

A file with no indexed function at all is `unmapped` (a header with
only declarations, a file the index does not hold), and so is a hunk
the index cannot place. Patch paths are matched to indexed spellings by
suffix (`patch_root` prepends a directory first). `mapping` counts each
precision so the reader knows how much of the set is exact.

The traversal is a reverse breadth-first walk from every changed
function at once over caller edges (stored and query-time expansions),
callers taken in canonical edge order, each function recorded at its
first (shallowest) reach with the path that reached it:

```json
{"changed": [{"usr": "...", "name": "parse", "file": "...", "line": 12,
              "via": "argument"}],
 "affected": [{"usr": "...", "name": "run", "file": "...", "line": 40,
               "depth": 1, "isEntryPoint": false, "changed": "parse",
               "path": [hop]}],
 "affectedCount": 7, "truncated": false,
 "entryPointsAffected": [{"name": "main", "usr": "...", "depth": 3}],
 "complete": true, "exhaustive": false, "stopReasons": ["depth_limit"],
 "skippedHubs": [], "mapping": {...}, "unmapped": [...]}
```

`affected` is ordered by (depth, key) and cut at `max_results`
(`truncated: true`, `affectedCount` is the full count). `complete`,
`exhaustive`, `stopReasons`, `skippedHubs` mean what they mean for the
path tools: `depth_limit` says callers exist beyond `max_depth`;
`work_budget` and `hub_pruned` (`max_fan_in`) clear `complete`. A
changed function that is not a known identity is listed under
`unknown` and searched for nothing.

## Order

| Payload | List | Order |
|---|---|---|
| `diff` | `changes` | change kind (`function_removed`, `function_added`, `call_removed`, `call_added`, `call_changed`, `context_changed`), then caller key, callee key, kind; sites within a record in canonical edge order |
| `diff` | `identity.ambiguous`, `identity.renameCandidates`, `moves` | key (before key for a candidate pair) |
| `diff` | `routes.added`, `routes.removed` | engine canonical order |
| `impact_of_change` | `changed` | key |
| `impact_of_change` | `affected` | (depth, key); `max_results` cuts after ordering |
| `impact_of_change` | `entryPointsAffected` | (depth, key) |
| `impact_of_change` | `unmapped` | (file, first line) |

## CLI

```bash
# What changed, as relationships, with the after-side witnesses
vycor-cpp megascope diff --before base.vycs --after head.vycs
vycor-cpp megascope diff --before base.vycs --after head.vycs --format ndjson | \
  jq -c 'select(.change) | {change, caller, callee, kind}'

# A route to a target that the change introduced
vycor-cpp megascope diff --before base.vycs --after head.vycs --to Sink::write \
  --format json | jq '.routes.added'

# Call sites whose protection changed
vycor-cpp megascope diff --before base.vycs --after head.vycs --format ndjson | \
  jq -c 'select(.change=="context_changed") | {caller, callee, before: .before.signatures, after: .after.signatures}'

# Callers affected by the change, from the diff itself
vycor-cpp megascope diff --before base.vycs --after head.vycs --impact --max-depth 5

# Callers affected by a named function, or by a patch
vycor-cpp megascope impact-of-change --changed parse --max-depth 3
git diff -U0 main HEAD | vycor-cpp megascope impact-of-change --patch-file - --index head.vycs
vycor-cpp megascope impact-of-change --git-base main --git-head HEAD --repo . --index head.vycs
```

`diff` reads both indexes read-only and writes nothing; the Git adapter
runs `git diff` in the named repository and never touches the checkout
or an index. Exit codes follow the contract: 0 when `changes` (or
`affected`) is non-empty, 1 when empty, 2 usage, 3 incomparable or
unreadable, 4 ambiguous target. `diff` is a CLI verb because it needs
two indexes; `impact_of_change` is a tool on every transport.

## Checks

- `tests/test_change_impact.cpp` (`[impact]`): identity keys (lambda
  ordinals, ambiguous groups); a line shift and a whitespace edit
  yield no change; each change kind on hand-built pairs; renamed and
  moved functions; the comparability labels for a partial and a
  differently selected index; the impact walk's order, budgets, hub
  pruning, and witness paths; patch parsing and every mapping
  precision; deep_chains baked with one and four threads, in reversed
  TU order, and warm-refreshed diff to zero changes.
- `scripts/warm-refresh-check.py`: after every refresh scenario the
  warm index and the clean rebuild diff to zero changes.
- `corpus/cases/change_impact` (`docs/validation.md`): a patch pair
  whose expectations name the added route, the removed call, the call
  site whose protection changed, and the callers impact must list; a
  whitespace-only overlay that must diff to nothing.

## Costs

Measured on the llvm-project testbed (`scripts/bench.py` host, 938
TUs); see the "Measurements" section of the handoff in this page's
history and `corpus/reports/baseline.json` for the corpus figures.

## Follow-ups

- **Anneal finding deltas** are not part of this package: `anneal`
  has no stable finding identity or export contract to diff against;
  one has to exist before "new findings since base" can be honest.
- The Git adapter maps after-side ranges only; a rename detected by
  `git diff -M` is reported as a removed and an added file.
