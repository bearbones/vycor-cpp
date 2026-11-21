# Dependency audit

This page is the contract for `audit_dependencies` and
`explain_dependency`: the two megascope tools that compare what a build
*declares* its link targets depend on with what the indexed code
*references*. The envelope is `docs/result-contract.md`; the order rules
are `docs/deterministic-output.md`.

## What it answers

A build graph says "target A links target B". It does not say why, or
how much of B the reason is. For every pair of link targets the audit
reports:

- **Which symbols of the provider do the consumer's objects need?** The
  count, and through `explain_dependency` each symbol with the call site
  that needs it.
- **What do those symbols drag along?** Their closure inside the
  provider — the provider code they transitively need — and the other
  targets that closure references.
- **What would the consumer stop linking if that closure were split
  out of the provider?** The targets it links only because of this
  edge, and how many symbols they hold.

Three findings fall out: a dependency that is declared but not seen in
use, one that is in use but not declared, and a *thin* dependency — a
few symbols with a small closure, the lead for splitting a large
library.

**Every answer here is evidence from call edges only**
(`completeness.edgeSource: "call-edges"`). A reference to a variable, a
vtable, or a typeinfo object makes a real link dependency that no call
edge records. No finding is a universal verdict; in particular an
`unused-link-dependency` finding means "no call was seen", never "safe
to remove". [Completeness](#completeness) lists what is invisible and
how the answer can mislead; [Checking against the
objects](#checking-against-the-objects) is how to measure it on a built
tree.

## Running it

```bash
# 1. Describe the build's targets (CMake; other build systems: "Adapters").
scripts/cmake-target-map.py --build-dir build --configure --output targets.json

# 2. Index the sources those targets compile.
vycor-cpp megascope index --build-path build

# 3. The edge table: thin dependencies, largest gain first.
vycor-cpp megascope audit-dependencies --build-path build \
    --target-map targets.json --finding thin-dependency --sort gain --format tsv

# 4. One edge: the symbols, the witnesses, the closure.
vycor-cpp megascope explain-dependency --build-path build \
    --target-map targets.json --consumer app --provider engine
```

The target map is an input of the query, not of the bake: the index
holds no target names, so one index answers for any map (another
configuration's targets, a proposed re-partition written by hand).
Within one `batch` process the analysis of a map is computed once and
reused while the map file's size and modification time are unchanged.

## Target map

A JSON document that names the build's link targets. vycor never reads
a build system; an adapter writes this file (see [Adapters](#adapters)).

```json
{
  "schema": 1,
  "generator": "cmake-file-api 4.2.0 (codemodel 2.9)",
  "depsForm": "direct",
  "root": "/work/project",
  "roots": ["/work/project", "/work/project/build"],
  "targets": [
    {"name": "engine", "kind": "static_library",
     "sources": ["src/engine/render.cpp", "src/engine/audio.cpp"],
     "headers": ["include/engine/render.h"],
     "deps": ["platform"]},
    {"name": "platform", "kind": "static_library",
     "sources": ["src/platform/file.cpp"], "deps": []},
    {"name": "app", "kind": "executable",
     "sources": ["src/app/main.cpp"], "deps": ["engine"]}
  ]
}
```

| Member | Required | Meaning |
|---|---|---|
| `schema` | yes | `1`. Any other value is rejected |
| `generator` | no | free text naming what wrote the file; echoed as `targetMap.generator` |
| `depsForm` | no | `"direct"` (default): each `deps` list is what the target's own build file names. `"closure"`: a list may already include what the target's dependencies pull in, so the audit cannot tell a named dependency from an inherited one (see [Declared form](#declared-form)) |
| `root` | no | base for relative paths. Relative itself, it resolves against the directory of the map file; absent, that directory is the root |
| `roots` | no | the directories the build owns (source tree, build tree for generated headers). Relative entries resolve against `root`; default `[root]`. A header outside every root — a system or third-party header — is never attributed to a target because of who includes it |
| `targets` | yes | array of targets; names must be unique |
| `targets[].name` | yes | the target's name, non-empty |
| `targets[].kind` | yes | `executable`, `static_library`, `shared_library`, `module_library`, `object_library`, `interface_library`, `external`. Echoed as `providerKind`; a target without sources owns no symbols whatever its kind |
| `targets[].sources` | no | the translation units compiled into the target's objects. One source may be listed by several targets |
| `targets[].headers` | no | headers the target claims: a function *declared* in one and whose body the index never saw is presumed to be the target's. Listing is needed only where [attribution](#attribution) cannot work it out |
| `targets[].deps` | no | names of the targets it declares a dependency on. Every name must be a target of the map; duplicates and the target's own name are dropped |
| `targets[].interfaceDeps` | no | names of the targets it tells its consumers to link without linking them itself (a header-only library forwarding the library its headers call; CMake's `target_link_libraries(t INTERFACE ...)`). They extend what a consumer reaches through the target and so decide `declared: "transitive"` against `"none"`; they are not the target's own dependencies, so they make no row and no unused finding. A name also in `deps` counts as a dependency only |

Paths are made absolute against `root` and normalized lexically (`.` and
`..` removed; symlinks are **not** resolved, so the map must spell a
file the way the compilation database does). A document that breaks any
rule above is a `usage_error` naming the first problem. Members the
table does not list are ignored, so an adapter can carry its own data
(the CMake adapter adds `artifact`, `objects` and `otherSources` for
the comparison script).

Targets are sorted by name on load; every list of targets in a payload
is in that order.

## Attribution

The index knows functions and the call edges between them; it does not
record which function has a body or where. The audit works that out
from where a function's calls are written, in six rules applied in
order. A **target's files** are its mapped sources plus every header
under `roots` that only translation units of one target set opened
(a *private header*; the per-TU opened-file tables of the index decide
this). A file compiled into several targets — a source two targets
list, a header private to both — belongs to that **set** of targets,
and so does the code in it.

| # | The function | Class | Owner |
|---|---|---|---|
| 1 | makes a call written in a target's file | `defined` | the targets of every such file |
| 2 | makes no such call, but is declared in a target's file | `defined` | the targets of that file |
| 3 | makes calls, all written in shared headers | `inline` | none: it is emitted into whichever object uses it |
| 4 | makes no call and is template code (its USR names an instantiation or a member of a class template) | `inline` | none |
| 5 | makes no call and is declared in a shared header that has an owner | `presumed` | the header's owner |
| 6 | anything else | unattributed | none: ignored |

"A call" in rules 1 to 3 is any call the index recorded in the body,
whatever its kind: a call that creates no reference (below) still shows
where the body is written. One case comes before rule 2: a function
whose calls are written in an indexed source that no target lists, and
in no target's file, is unattributed — its body is in a file the map
leaves out, so it is neither a provider's symbol nor an inline body
that carries references (`completeness.references.unmappedBody` counts
the references dropped with it).

The owner of a shared header is the first that applies:

1. the target that lists it under `headers`;
2. the target sets that own the most `defined` functions declared in
   that header, when the header is under `roots`. Several sets tied for
   the most are all presumed: the header's leaves could be any of
   theirs, so each counts as an uncertain use;
3. the one target set with a source of the same file stem that opens
   it (`tokenizer.hpp` and `tokenizer.cpp`).

Rule 5 takes a `presumed` symbol's owner from it. An `inline` function
keeps the owner of its declaring header as its *home*: not a link
owner — nothing links against an inline body — but enough to say the
consumer uses that target's code ([indirect uses](#indirect-uses)).

What the rules mean for the cases that matter:

- **Defining TU → target.** A function with calls in `render.cpp` is
  owned by every target that lists `render.cpp`. A function without a
  single call in it (a leaf: a getter, a function that only touches
  data) has nothing that pins its body, so it is `defined` only when
  its declaration sits in a target's file, and `presumed` otherwise.
- **Header-defined inline and template code** is `inline`: owned by no
  target, charged to whoever calls it, and walked through by the
  closure (next section). An inline leaf — no calls in its body —
  declared in a header with an owner cannot be told apart from a
  declaration whose body lives in a source file, and is `presumed`.
- **A TU compiled into several targets** makes its functions owned by
  all of them. A reference to such a function is a use of each owner; a
  consumer that is itself one of the owners references its own copy and
  uses no one.
- **Nodes outside every target** — system and third-party headers
  outside `roots`, functions in indexed sources no target lists — are
  unattributed. They are neither symbols of a provider nor carriers of
  a reference. `completeness.sources.unmappedTus` counts indexed
  translation units the map does not place.

`presumed` symbols are reported apart everywhere
(`uncertainSymbols`, `providerPresumedSymbols`, `owner: "presumed"` on a
seed): the owner is a guess, and the symbol may be an inline function
that links nothing.

A call through a vtable slot whose target is not proven
(`VirtualDispatch`, `Plausible`) and a call through a function pointer
joined at query time (`FunctionPointerReturn`) create no reference:
a dispatched call names a vtable slot, not a function symbol. They are
counted in `completeness.references` (`virtualDispatchSkipped`,
`deferredSkipped`) and kept as [indirect uses](#indirect-uses). A
virtual call the index proved (`Proven`) is a direct reference.

The index also records a *qualified* call to a virtual function
(`Base::run()` from an override) as an unproven `VirtualDispatch`,
although it is bound statically and the object does name the symbol.
The audit cannot tell the two apart, so such a call is an indirect use
and not a symbol of the row.

### Indirect uses

A row's `indirectUses` counts the provider's code the consumer touches
without a reference the linker is known to need:

- a `defined` or `presumed` symbol of the provider reached only by an
  unproven virtual call or a function-pointer return, from the
  consumer's objects or from an inline body they use;
- an `inline` function the consumer uses whose home is the provider.

They are disjoint from the row's symbols, enter no closure and no thin
finding, and do one thing: a row with an indirect use is never reported
as unused.

## Inline bodies and the closure

A reference is **held by a target's objects** when its call site is
written in one of the target's files, or in a shared header but inside
a `defined` function (a macro or an `.inc` body expanded there). A
reference written in an `inline` function's body is held by nobody
until something uses that function.

- **Seeds.** The consumer's seeds onto a provider are the provider's
  symbols reached from references the consumer's objects hold, walking
  *through* `inline` functions: `main.cpp` calls the header's
  `clamp()`, `clamp()` calls the provider's `fail()`, so `fail` is a
  seed with `via: "inline"` and `clamp` named under `through`.
  `viaInline` counts the seeds no source of the consumer names
  directly.
- **Closure.** From the seeds, every `defined` symbol of the provider
  they transitively reference, again walking through `inline` bodies.
  `closureSymbols` and `closureFiles` size it; `explain_dependency`
  lists it.
- **Needs.** A reference that leaves the provider from inside the
  closure names another target the closure needs (`closureNeeds`).
  When the referenced symbol has several owners, the ones the provider
  already depends on are charged; with none of those, all of them.

The walk treats an inline body as present in full wherever it is used.
The compiler may have inlined it and dropped the calls a constant
condition removes, or never emitted a body nobody called; the audit
does not know. That over-counts, never under-counts, the references an
inline body carries.

## The edge table

`audit_dependencies` answers with one row per pair of targets that is
either declared (in the consumer's `deps`) or observed (at least one
symbol or uncertain symbol referenced).

| Argument | Meaning |
|---|---|
| `target_map` | required: path of the target map |
| `consumer`, `provider` | only the rows of that consumer / onto that provider (exact target names; an unknown name is `not_found`) |
| `finding` | only rows with this finding: `unused-link-dependency`, `undeclared-link-dependency`, `thin-dependency`, or `any` |
| `sort` | `name` (default): by consumer, then provider. `gain`: `droppableSymbols` descending, then `closureSymbols` ascending, then by name |
| `thin_max_symbols`, `thin_max_closure_pct` | the thin thresholds (defaults 5 and 10) |
| `limit`, `offset` | paging over `edges` (default limit 200) |

| Row member | Meaning |
|---|---|
| `consumer`, `provider`, `providerKind` | the pair |
| `declared` | `direct`, `transitive`, `closure`, or `none` — [Declared form](#declared-form) |
| `symbols` | `defined` symbols of the provider the consumer's objects reference |
| `uncertainSymbols` | `presumed` symbols of the provider they reference |
| `viaInline` | of `symbols`, those reached only through an inline body |
| `indirectUses` | the provider's symbols the consumer touches without a reference the linker is known to need — [Indirect uses](#indirect-uses) |
| `closureSymbols`, `closureFiles` | the closure of `symbols` inside the provider, and the files holding it |
| `providerSymbols`, `providerFiles`, `providerPresumedSymbols` | the provider's size: `defined` symbols, the files holding them, `presumed` symbols |
| `separabilityPct` | the share of the provider's `defined` symbols *outside* the closure, 0–100: how much of the provider the consumer would not need |
| `closureNeeds` | other targets the closure references |
| `onlyViaEdge` | targets (the provider included) the consumer reaches through this dependency and through no other dependency it declares or uses |
| `droppableTargets` | of those, the ones the closure would not pull back in: what the consumer stops linking if the closure is split out into a target of its own |
| `droppableSymbols` | the `defined` symbols in `droppableTargets`, less the closure itself when the provider is among them — the gain |
| `providerDroppable` | the provider is one of `droppableTargets` |
| `consumerCovered`, `providerCovered` | every mapped source of the target was indexed cleanly |
| `consumerUnplacedUses` | a property of the consumer, repeated on its rows: the functions its objects reference that are declared under `roots` and that no rule could place in a target. Any of them may belong to this row's provider. Information only: no finding depends on it |
| `witness` | the first seed in canonical order: `site`, `from`, `to`, `kind` (absent without one) |
| `findings` | the row's findings, each `{"id", "evidence": "call-edges", "universal": false}` |

`onlyViaEdge`, `droppableTargets` and `droppableSymbols` are computed
for `declared: "direct"` rows only (elsewhere 0 and empty): they
describe removing a line from the consumer's build file. They count
symbols the index attributed, not bytes and not link time; a row with a
large `droppableSymbols` is where to look, not a measurement of the
saving.

The payload also carries `targetMap` (`path`, `generator`, `depsForm`,
`targets`, `roots`), `completeness`, `thinThresholds`, `findingCounts`
(over every row the `consumer` / `provider` filters keep, before the
`finding` filter and paging), `unusedWithheld`, and `sort`.

### Declared form

| `declared` | Meaning |
|---|---|
| `direct` | the consumer's build file names the provider |
| `transitive` | the provider is reachable only through another declared dependency |
| `closure` | the map is in closure form and the provider is listed, but another listed dependency also reaches it — it may be named or inherited |
| `none` | the consumer's declared dependencies do not reach the provider |

A `depsForm: "closure"` map lists everything a target's dependencies
pull in. The audit then treats as `direct` exactly the listed
dependencies no other listed dependency reaches (they cannot be
inherited), and the rest as `closure`. A build system's closure may
also contain order-only dependencies that are not link dependencies at
all (CMake's `add_dependencies`); they look declared here.

### Findings

| Finding | Condition |
|---|---|
| `unused-link-dependency` | `declared: "direct"`; no symbol, no uncertain symbol and no indirect use; consumer and provider both covered; the index's coverage is stated and it was baked without `--collapse-paths`; the index attributes at least one symbol (`defined` or `presumed`) to the provider |
| `undeclared-link-dependency` | at least one `defined` symbol referenced, and `declared` is `transitive` or `none` |
| `thin-dependency` | `declared` is `direct` or `closure`; `1 ≤ symbols + uncertainSymbols ≤ thin_max_symbols`; `closureSymbols + uncertainSymbols` is at most `thin_max_closure_pct` percent of `providerSymbols + providerPresumedSymbols` |

`unused-link-dependency` **fails closed**: a target with a source that
is missing from the index or did not parse cleanly, an index without
stated coverage, an index baked with collapsed paths, a provider the
index holds no symbol of, or an indirect use yields no finding.
`unusedWithheld` counts the declared-direct rows with no symbol and no
uncertain symbol that got no finding for one of those reasons, so "no
unused findings" is never mistaken for "everything is used". When the
finding is reported it is still not a verdict: the objects may need the
provider for a variable, a vtable, a static initializer, or a function
the audit could not attribute (below; `consumerUnplacedUses` says how
many of the consumer's callees that is).

`thin-dependency` describes the shape of an edge, not its value. Rank
thin rows by `droppableSymbols` (`--sort gain`), then read the closure
with `explain_dependency`: a thin edge onto a small leaf library is
true and worthless, and a thin edge whose closure needs most of what
the provider needs saves nothing.

## `explain_dependency`

One row in full. Arguments: `target_map`, `consumer`, `provider`
(required; exact names, different targets), `closure_limit` (closure
symbols to list, default 50, `0` for none), the thin thresholds, and
`limit` / `offset` over `seeds`. A pair with no row (not declared, not
observed) answers `ok` with empty `seeds` and exit code 1.

| Member | Meaning |
|---|---|
| `edge` | the row, as in the edge table |
| `seeds` | every provider symbol the consumer references — certain before uncertain, named by a source before via-inline, then by site. Each: `symbol` (`name`, `usr`, `file`, `line`), `owner` (`defined` / `presumed`), `via` (`direct` / `inline`), `kind` (the edge kind), `site` (where the reference to the symbol is written), `from` (the function that makes it), and for `via: "inline"` `through`: the call in the consumer's own files (`site`, `from`, `to`) the walk started from |
| `closure` | the closure's symbols by qualified name, then USR: `name`, `usr`, `file`, `line` (the declaration), `definedIn` (the file its calls are written in, when known) |
| `closureTotal`, `closureTruncated` | the full count, and whether `closure_limit` cut the list |

## Completeness

Every payload of both tools carries `completeness`:

| Member | Meaning |
|---|---|
| `edgeSource` | `"call-edges"` — what the references were derived from |
| `unusedIsUniversal` | `false`: an unused finding is not a proof |
| `invisible` | the kinds of link dependency this evidence cannot see, as stable tags (below) |
| `depsForm` | the map's form |
| `coverageStated` | whether the index states its coverage (`indexScope`) and carries the per-TU tables attribution needs |
| `edgesCollapsed` | the index was baked with `--collapse-paths`; the `collapsed-call-edges` tag is then present and no unused finding is raised |
| `targets` | `total`, `withSources`, `covered` |
| `sources` | `mapped` (distinct sources in the map), `indexed` (parsed cleanly), `failed`, `notIndexed`, `unmappedTus` |
| `symbols` | `defined`, `presumed`, `inline`, `unattributed` |
| `references` | `anchored` (held by a target's objects), `inlineBody` (travel with a caller), `virtualDispatchSkipped`, `deferredSkipped`, `unmappedBody` (made by a function whose body is in an indexed source no target lists), `unresolvedSite` (the call site's file matched no known file) |

`invisible`:

| Tag | What is missed |
|---|---|
| `variable-references` | reading, writing, or taking the address of another target's global or static data member |
| `vtable-and-typeinfo-references` | constructing or deriving from a class whose vtable another target emits; `typeid`, `dynamic_cast`, catching its type |
| `static-initializer-calls` | a call made while initializing a namespace-scope variable: it has no enclosing function, so no edge |
| `implicit-destructor-calls` | destroying a local, a temporary, or a member whose destructor another target defines: no call is written, so no edge (the index records a destructor only for a local of a polymorphic class) |
| `calls-in-template-bodies` | a call written in a function template or in a member of a class template. The index does not walk instantiations: a dependent call is recorded nowhere, and the instantiation a caller reaches has no outgoing edges, so the walk through inline code stops there |
| `definitions-without-calls` | a function whose body makes no call is placed by its declaration alone (rules 2, 5, 6) |
| `name-based-lookup` | `dlsym`, plugin registries, linker sections, `--whole-archive` |
| `other-build-configurations` | code under an `#if` the indexed configuration did not take; other platforms' sources |
| `collapsed-call-edges` | only for an index baked with `--collapse-paths`: calls between two files under a collapsed path were never recorded, so closures inside those paths are undercounted. Bake the index for an audit without collapse paths |

Known ways an answer is wrong, beyond what the tags say:

- **A leaf defined in a source, declared in a shared header** is
  `presumed`, or unattributed when the header has no owner by listing,
  vote, or stem. A consumer that calls *only* unattributed functions of
  a provider is reported as not using it. List the provider's public
  headers under `headers` to close most of this.
- **An inline leaf in an owned header** looks `presumed`: the consumer
  appears to need a symbol that links nothing.
- **A forward declaration in a source file** makes rule 2 place the
  function in that source's target.
- **A function whose calls are all written in macro bodies or `.inc`
  files outside `roots`** has no call in a target's file and looks
  `inline` or `presumed`.
- **An explicit template instantiation** is template code by its USR,
  so `inline`, though one target's object emits it. A consumer that
  calls it shows an indirect use of the header's owner, not a symbol.
- **A function with a body in several targets** (the same function
  defined in sources of different targets) holds the references of
  every copy, so a closure through it over-counts.
- **A source another translation unit includes** (a unity build, an
  `#include "impl.cpp"`) counts as indexed when the including unit
  parsed cleanly, though its own compile command was never used.
- **A call site the index cannot match to one file**
  (`references.unresolvedSite`) loses its reference, and no finding is
  withheld for it.
- **The map and the index are not checked against each other.** A map
  written from another checkout or configuration than the index was
  baked from is compared as it stands.
- **A function template named `operator<`** spells its instantiations
  `operator<<#...`, the same as `operator<<` itself; the name is read as
  the stream inserter, so the instantiation looks `presumed` rather
  than `inline`.
- **A partial index** can make a shared header look private: only the
  indexed translation units vote. `consumerCovered` / `providerCovered`
  say when that can be the case.
- **A generated header** is private to nobody unless the build
  directory is among `roots` (the CMake adapter adds it).
- **The optimizer.** A call the compiler removed or inlined is still an
  edge here; the objects do not need the symbol.

## Checking against the objects

`scripts/dep-audit-nm-compare.py` measures how far the answer is from
what a built tree links. For every target it reads the undefined and
defined symbols of the target's objects with `llvm-nm`, takes
`undefined(consumer) ∩ defined(provider)` as the truth for the pair,
and compares that with the audit's seeds.

```bash
scripts/dep-audit-nm-compare.py --binary build/src/vycor-cpp \
    --index build/.vycor/megascope.vycs --target-map targets.json \
    --out report.json
```

It needs the object files, or the static archive, of each target: the
map's `objects` / `artifact` members, which the CMake adapter fills
from the build directory. Targets without them are counted as not
built and skipped. `--strip-underscore` (default `auto`: on macOS)
drops the underscore Mach-O puts before every symbol.

The report splits every difference by cause:

- **Object symbols by class**: functions, variables, vtables, typeinfo.
  Everything but functions is invisible to call edges by construction.
- **Functions the objects need and the audit missed**, by why: the
  call exists in the consumer's own sources but the callee was not
  placed in the provider (`calleeNotPlaced`), the only calls are in
  header bodies (`headerBodyOnly`), the index holds the function with
  no call from the consumer (`noCallEdge`), or does not hold it
  (`notInIndex`).
- **Functions the audit reports and the objects do not need**, by how
  the audit reached them (`defined`, `viaInline`, `presumed`) and what
  the objects say (`noUndefinedReference` — optimized away or never
  emitted; `consumerEmitsItsOwn` — an inline copy; `noSuchSymbol` — the
  provider defines no such symbol).
- **Findings against the objects**: unused findings the objects
  contradict (split into "functions needed" and "only data needed"),
  thin findings by how many symbols the objects really need, undeclared
  findings the objects confirm.

Functions are matched by qualified name without parameters or template
arguments, so a set of overloads counts once on both sides.

## Adapters

vycor is build-system-neutral: an adapter is a script that emits the
target map.

**CMake** — `scripts/cmake-target-map.py --build-dir <dir> [--output
targets.json] [--configure] [--config <name>]` reads a File API
codemodel-v2 reply. CMake writes the reply only when a query file
exists at configure time; without a reply the script writes the query
and stops (exit 3) so the next `cmake <dir>` produces one, or runs
cmake itself with `--configure`.

| CMake | `depsForm` | Dependencies from |
|---|---|---|
| 4.2 and newer (codemodel 2.9+) | `direct` | each target's `linkLibraries` and `objectDependencies`: what `target_link_libraries` and `$<TARGET_OBJECTS:...>` name. `interfaceLinkLibraries` the target does not link itself become `interfaceDeps`. `INTERFACE` libraries are listed |
| 3.14 – 4.1 | `closure` | each target's `dependencies`: the transitive build-order closure, `add_dependencies` included. No `INTERFACE` libraries |

Sources are the entries CMake compiles as C, C++, Objective-C or
Objective-C++ (other languages go to `otherSources`); `headers` are the
header files a target lists among its sources; `roots` are the source
and build directories; imported targets are `external` (imports of one
name in several directories are one target); `UTILITY` targets are
dropped. With a `compile_commands.json` in the build
directory each target also gets `objects`.

**Bazel and Buck2** — not implemented. The rules under `bazel/` and
`buck2/` already walk the target graph to merge per-target indexes; the
same walk has what the map needs (`srcs`, `hdrs`, and direct `deps` per
`cc_library` / `cxx_library`), in `direct` form, with `hdrs` giving
`headers` exactly instead of by inference. A `genquery` / `cquery
--output=jsonproto` or a BXL script that prints the JSON above is a
complete adapter.

## Implementation

`include/vycor/deps/`, `src/deps/`:

- `TargetMap.h` — the parsed map: flat CSR tables, validated once at
  load.
- `DependencyAudit.h` — two layers. `LinkFacts` is the evidence: symbols
  with an owning target set and a class, and the references between
  them, in flat arrays. `linkFactsFromCallEdges` fills it from the call
  graph (the attribution rules above). `auditDependencies` is the
  kernel: it reads `LinkFacts` and the map only, and computes seeds,
  closures, needs and gains with bit matrices and stamp arrays sized
  once per audit. A producer over reference facts — variables, vtables,
  typeinfo — replaces the first layer and leaves the kernel and the
  payloads as they are; `completeness.edgeSource` and `invisible` come
  from the producer.

`src/query/DependencyTools.cpp` holds the two tools.
