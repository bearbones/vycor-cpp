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
  count, and through `explain_dependency` each symbol with the reference
  site that needs it.
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

**Every answer is evidence, not a verdict.** The audit reads the
symbol reference facts of the index (`completeness.edgeSource:
"references"`, `docs/symbol-references.md`): calls, address-ofs, reads
and writes of variables, vtable and typeinfo uses, including those made
by template instantiations and by code the compiler writes. What those
facts cannot see — lookups by name at run time, code another build
configuration compiles — is listed in `completeness.invisible`. An index baked
before the reference facts existed (format v14 and older) is audited
from its call edges instead (`edgeSource: "call-edges"`, [Call-edge
fallback](#call-edge-fallback)), which misses every reference that is
not a call. No finding is universal; in particular an
`unused-link-dependency` finding means "no reference was seen", never
"safe to remove". [Completeness](#completeness) lists what is invisible
and how the answer can mislead; [Checking against the
objects](#checking-against-the-objects) is how to measure it on a built
tree.

## Running it

```bash
# 1. Describe the build's targets (CMake; other build systems: "Adapters").
scripts/cmake-target-map.py --build-dir build --configure --output targets.json

# 2. Index the sources those targets compile.
vycor-cpp megascope index --build-path build

# 3. The edge table: thin dependencies, those that help a link target first.
vycor-cpp megascope audit-dependencies --build-path build \
    --target-map targets.json --finding thin-dependency --format tsv

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

The reference facts record, per symbol (function, variable, class), its
linkage, its attributes, its declarations and the TUs that define it,
and per reference the referring symbol, the target, the role, and one
witness site (`docs/symbol-references.md`). The audit turns them into a
**class** and an **owning target set** per symbol, and decides which
target's objects **hold** each reference.

A **target's files** are its mapped sources plus every header under
`roots` that only translation units of one target set opened (a
*private header*; the per-TU opened-file tables of the index decide
this). A file compiled into several targets — a source two targets
list, a header private to both — belongs to that **set** of targets,
and so does the code in it.

### Symbol classes

| The symbol | Class | Owner |
|---|---|---|
| a function or variable with a strong definition — external linkage, or internal linkage or an explicit instantiation defined in a TU's main file — in a target's source, or in a header only one TU includes (an `.inc` body) | `defined` | the targets of every TU that defines it |
| a class whose vtable a target's TU emits (it defines the key function) | `defined` | the targets of those TUs |
| an inline function, a template, a lambda, a symbol internal to a header, a strong definition written in a header several TUs include, a class without a key function | `inline` | none: it is emitted into whichever object uses it |
| an external symbol no indexed TU defines, declared in a header whose owner is not fully indexed | `presumed` | the header's owner |
| anything else: system-header code, file-scope pseudo-symbols, an external symbol no indexed TU defines whose owner is fully indexed (`symbols.undefinedInCoveredOwner`), code defined only in an indexed source no target lists | unattributed | none: ignored |

The owner of a shared header is the first that applies:

1. the target that lists it under `headers`;
2. the target sets that own the most `defined` symbols declared in
   that header, when the header is under `roots`. Several sets tied for
   the most are all presumed: the header's declarations could be any of
   theirs, so each counts as an uncertain use;
3. the one target set with a source of the same file stem that opens
   it (`tokenizer.hpp` and `tokenizer.cpp`).

A private header is owned by its target set. An `inline` symbol keeps
the owner of its declaring header as its *home*: not a link owner —
nothing links against an inline body — but enough to say the consumer
uses that target's code ([indirect uses](#indirect-uses)).

What the rules mean for the cases that matter:

- **Defining TU → target.** A function defined in `render.cpp` is owned
  by every target that lists `render.cpp`, whether or not its body makes
  a call.
- **Header-defined inline and template code** is `inline`: owned by no
  target, charged to whoever references it, and walked through by the
  closure ([Inline bodies and the closure](#inline-bodies-and-the-closure)).
- **A TU compiled into several targets** makes its symbols owned by all
  of them. A reference to such a symbol is a use of each owner; a
  consumer that is itself one of the owners references its own copy and
  uses no one.
- **One identity for distinct code.** A USR two targets' sources both
  define (every tool's `main`, an anonymous namespace's members in two
  files of one name) is owned by both. Each of its references is charged
  to the target set of the file the reference is written in, or of the
  one TU that recorded it (`references.siteAnchored`), so one tool's
  `main` does not lend its references to another. A closure walking
  through such a symbol follows only the references of the provider's
  own copy.
- **Presumed** symbols are reported apart everywhere
  (`uncertainSymbols`, `providerPresumedSymbols`, `owner: "presumed"` on
  a seed): the definition may be in a source the index lacks, or may
  not exist.
- `completeness.sources.unmappedTus` counts indexed translation units
  the map does not place; `references.unmappedBody` counts the
  references their code made, which nobody holds.

### References

Every role is a link reference — `call`, `address_of`, `read`,
`write`, `vtable_use`, `typeinfo_use` — with two exceptions, which are
*soft*: they are kept as [indirect uses](#indirect-uses) and walked by
the closure, but name no symbol of a row.

- **A dispatched virtual call.** A call of a virtual function the
  compiler dispatches through the vtable names a slot, not a symbol.
  The call graph tells a dispatched call from one bound statically (an
  object of known type, a proven target); the bound call is a link
  reference (`references.virtualDispatchSkipped` counts the others).
- **Overload candidates.** A template whose overload set stays open
  until instantiation records a reference to every candidate, at one
  site. Two or more calls (or address-ofs) from one referrer, at one
  witness site, to different symbols of one unqualified name are such
  candidates (`references.overloadCandidates`): any one instantiation
  uses one of them, or none. An instantiation that selects one leaves
  the same record (same referrer, target and site), so nothing marks
  the selected candidate. Differently named targets at one site — a
  macro expanding to several calls — are ordinary references.

The audit adds the references the compiler writes for a class:

| Kind | From → to | Held by |
|---|---|---|
| `vtable_entry` | a class → each virtual function it declares | wherever the class's vtable is emitted |
| `base_class` | a dynamic class → each dynamic base class (from the call graph's hierarchy, where some TU saw both definitions) | the derived class's vtable homes that saw the base |
| `inherited_virtual` | a dynamic class → each virtual function of a base it does not override (destructors aside) | as `base_class` |
| `enclosed_lambda` | a function → a lambda it calls | the function |

A base the hierarchy names but no TU saw beside the derived class (a
name shared by every specialization of a template, or by classes of two
anonymous namespaces) is not taken (`references.baseClassesUnseen`).

### Who holds a reference

| The referring symbol | Held by |
|---|---|
| `defined` | its owners (one of them for a shared identity, above) |
| `inline` | nobody, until something uses it: the reference travels with the body |
| `inline`, with a weak or `used` definition in a header | every target whose TUs open that header: such a body is emitted whether used or not (`symbols.emittedEverywhere`) |
| a lambda | the targets of the file it is written in |
| file scope (a default member initializer, a namespace-scope initializer the visitor gives no function) | the targets of the file it is written in, or of the one TU that recorded it; else nobody (`references.fileScopeDropped`) |
| unattributed | nobody |

### Indirect uses

A row's `indirectUses` counts the provider's code the consumer touches
without a reference the linker is known to need:

- a `defined` or `presumed` symbol of the provider reached only by a
  soft reference (a dispatched virtual call, an overload candidate),
  from the consumer's objects or from an inline body they use;
- an `inline` symbol the consumer uses whose home is the provider.

They are disjoint from the row's symbols, enter no thin finding, and do
one thing: a row with an indirect use is never reported as unused.

## Inline bodies and the closure

- **Seeds.** The consumer's seeds onto a provider are the provider's
  symbols reached from references the consumer's objects hold, walking
  *through* `inline` bodies: `main.cpp` calls the header's `clamp()`,
  `clamp()` calls the provider's `fail()`, so `fail` is a seed with
  `via: "inline"` and `clamp` named under `through`. `viaInline` counts
  the seeds no source of the consumer names directly.
- **Closure.** From the seeds, every `defined` symbol of the provider
  they transitively reference, again walking through `inline` bodies.
  The walk also takes the `defined` provider symbols a soft reference
  names (an overload candidate, a virtual function called through its
  slot): one of them may be what an instantiation uses, so the closure
  over- rather than undercounts what would move with the seeds.
  `closureSymbols` and `closureFiles` size it; `explain_dependency`
  lists it.
- **Needs.** A hard reference that leaves the provider from inside the
  closure names another target the closure needs (`closureNeeds`). When
  the referenced symbol has several owners, the ones the provider
  already depends on are charged; with none of those, all of them. A
  soft reference adds no need: an inline body's soft references are the
  union of every instantiation's candidates.
- **Fan-in.** `closureFanIn` counts the `defined` provider symbols
  outside the closure that reference into it (directly, softly, or
  through inline bodies), and `closureFanInFiles` the files holding
  them. Zero: the closure is a leaf of the provider and can be split out
  whole. Non-zero: the rest of the provider would depend on the split
  part, so the provider keeps linking it and what it needs — the
  consumer's edge moves, the provider's link does not shrink.

The walk treats an inline body as present in full wherever it is used.
The compiler may have inlined it and dropped the references a constant
condition removes, or never emitted a body nobody called; the audit
does not know. That over-counts the references an inline body carries.
A template's references are those of its pattern, which holds what
every instantiation the index read references
(`docs/symbol-references.md`, "Code the compiler writes"). The walk
narrows them two ways, both for a reference that travels with an inline
body, and takes it for a consumer (in a closure, for the provider) only
when both allow:

- **Who recorded it.** A reference that only TUs of one target set
  recorded is *scoped* to that set (`references.tuScoped`). The index
  keeps, per reference, the number of TUs that recorded it and the first
  and last of them by path; the contributors are all of one set when
  every TU the index meta lists between those two is, which is how the
  sources of one target sort. A reference recorded by TUs of several
  sets, or with a TU of another set between its first and last, is not
  scoped; without index meta only a reference one TU recorded is.
- **Who can see its target.** A TU references a symbol only through a
  declaration it sees, and records a declaration it sees (for a class,
  its definition when it has one). The targets that can reference a
  symbol are those with a TU whose opened-file table (index meta) names
  a file holding one of its recorded declarations (`symbols.seenBy`
  counts the symbols that narrows); a target whose TUs never open that
  file takes no reference to it from a shared body. A TU no target lists
  counts for the targets of the mapped sources it includes (a unity
  build). A TU with reference facts but an empty table (one recorded
  without it) counts as seeing every symbol. Without index meta nothing is narrowed.

So a consumer that uses `make<A>()` does not walk through what another
target's `make<B>()` builds when `B` is declared where the consumer
never looks, or when only the other target's TUs instantiate it. What
neither tells apart stays a union: a consumer walks through what its own
TUs' other instantiations build, and through what any target that sees
the same declarations builds. Fan-in climbs through an inline body
under the same rule; registration marks (below) do not narrow, so they
can over-mark.

### Registration seeds

A symbol whose body (or an inline body it reaches) takes the address of
a function of a template is a *registration*: a factory instantiated for
one type and handed to a registry (`RegisterMCAsmParser<T>`,
`TargetRegistry::RegisterX`). What the factory builds is reached through
the registry at run time, not through the closure, and the factory's
body is its pattern's, which holds what every registered type's
instantiation builds. Such seeds are flagged
(`registers: true` on a seed, `registrationSeeds` on the row), and a
thin finding whose every certain seed is one carries
`"registration": true`: the small closure is an artifact, not a lead.

## The edge table

`audit_dependencies` answers with one row per pair of targets that is
either declared (in the consumer's `deps`) or observed (at least one
symbol or uncertain symbol referenced).

| Argument | Meaning |
|---|---|
| `target_map` | required: path of the target map |
| `consumer`, `provider` | only the rows of that consumer / onto that provider (exact target names; an unknown name is `not_found`) |
| `finding` | only rows with this finding: `unused-link-dependency`, `undeclared-link-dependency`, `thin-dependency`, or `any` |
| `sort` | `link_gain` (default): `linkTargetGain` descending, then `linkTargetsHelped` descending, then `closureSymbols` ascending, then by name. `split_gain`: `splitGain` descending, then as `link_gain`. `gain`: `droppableSymbols` descending, then `closureSymbols` ascending, then by name. `name`: by consumer, then provider |
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
| `separabilityPct` | the share of the provider's `defined` symbols *outside* the closure, 0–100: how much of the provider the consumer would not need. A thin row's closure is at most `thin_max_closure_pct` of the provider, so on thin rows this is at least 90 by construction and does not rank them |
| `closureNeeds` | other targets the closure references |
| `onlyViaEdge` | targets (the provider included) the consumer reaches through this dependency and through no other dependency it declares or uses |
| `droppableTargets` | of those, the ones the closure would not pull back in: what the consumer stops linking if the closure is split out into a target of its own |
| `droppableSymbols` | the `defined` symbols in `droppableTargets`, less the closure itself when the provider is among them — the gain |
| `providerDroppable` | the provider is one of `droppableTargets` |
| `linkTargetsHelped`, `linkTargetGain`, `linkTargetMembers` | the final link targets (executables, shared and module libraries) that would load less if the closure were split out of the provider, the most emitted symbols one of them would stop loading, and that target's members of which nothing is loaded any more — [Link-target gain](#link-target-gain) |
| `fanInPct`, `splitGain` | the share of the rest of the provider that references the closure, and `linkTargetGain` discounted by it — [Link-target gain](#link-target-gain) |
| `closureFanIn`, `closureFanInFiles` | `defined` provider symbols outside the closure that reference into it, and their files — [the closure](#inline-bodies-and-the-closure) |
| `registrationSeeds` | certain seeds that register a factory — [Registration seeds](#registration-seeds) |
| `providesMain` | the consumer is an executable that defines no `main` and the provider, a library, defines one (a test runner's main library, for instance): needed although no consumer object names it |
| `providerStaticInits` | `defined` variables of the provider whose initializer calls a function: code that runs when a program links the provider's object |
| `consumerCovered`, `providerCovered` | every mapped source of the target was indexed cleanly |
| `consumerUnplacedUses` | a property of the consumer, repeated on its rows: the symbols its objects reference that are declared under `roots` and that no rule could place in a target. Any of them may belong to this row's provider. Information only: no finding depends on it |
| `witness` | the first seed in canonical order: `site`, `from`, `to`, `kind` (absent without one) |
| `findings` | the row's findings, each `{"id", "evidence", "universal": false}` (`evidence` is `completeness.edgeSource`), with the extra members under [Findings](#findings) |

`onlyViaEdge`, `droppableTargets` and `droppableSymbols` are computed
for `declared: "direct"` rows only (elsewhere 0 and empty): they
describe removing a line from the consumer's build file. They count
symbols the index attributed, not bytes and not link time; a row with a
large `droppableSymbols` is where to look, not a measurement of the
saving.

### Link-target gain

`droppableSymbols` is what the *consumer* stops linking. When the
consumer is a static or object library, nothing is linked until a final
link target (an executable, a shared or a module library) links it,
and the linker does not take a static library whole: it loads an
archive member only when the member defines a symbol something already
loaded references. The link-target gain simulates that, per final link
target, at the granularity of objects.

**Members.** Every mapped source of a target with objects is one
member. A `defined` symbol lives in the member of the translation unit
that holds its code (the defining source; for a class, the TU that
emits its vtable; an identity defined in several TUs, such as each
program's `main`, has one member per owner). What a member references
is what its definitions' code references, through the inline bodies it
uses, plus the references of its file-scope code (namespace-scope
initializers). Only hard references count.

**Weight.** A member weighs its *emitted symbols*: its `defined`
symbols plus the inline bodies its code uses, since each object
compiles its own copy of those. On the LLVM testbed this tracks the
members' allocated bytes better than the count of defined symbols does
(rank correlation 0.85 against 0.81 over 709 archive members); bytes
themselves need a built tree, which the index does not have.

**One link.** Which targets a final link target links is read from the
map, as for `onlyViaEdge`: its rows that are declared or used, the
`interfaceDeps` of what it reaches, and the declared `deps` of every
other target on the way. The target's own members are loaded, and so is
every object of an object library on the line; a shared or module
library on the line is loaded whole once any of its symbols is used. A
referenced symbol is then resolved, in order of preference, from the
target itself, an object or shared library on the line, a static
library on the line, or any static library that defines it (a reference
the build resolves some other way still loads the member), until
nothing new is loaded. `linkInput` (in `explain_dependency`) is the
weight this loads as the map stands, and `linkInputMembers` the
members.

**The split.** For a `declared: "direct"` row, the row's closure moves
out of the provider into a new target of the provider's kind, file by
file: each provider member that held part of the closure becomes two
objects, its part of the closure (a member of the new target) and the
rest of its code (still the provider's), each with the inline bodies it
uses; when a link loads both, they weigh what the member did (an inline
body both use is one copy). Every final link target that links
the consumer is simulated again with the row's edge replaced by the
closure's needs: a reference to a closure symbol loads the part that
holds it, and a provider member's rest is loaded only if something
still references what is left in it, so members pulled in only through
the closure — in the provider and transitively in any other target —
drop out. When the provider is an object library every part is linked,
and the rest only if the provider is still on the line another way. A
row with an empty closure gains only what leaves the line (an object or
shared library no longer reached).

| Member | Meaning |
|---|---|
| `linkTargetGain` | the most emitted symbols one final link target stops loading: `linkInput` before less the weight after, the new target's parts included. Never negative: a target that would load more is not helped |
| `linkTargetMembers` | that target's members of which no code is loaded after the split: neither the member, nor its rest, nor its part |
| `linkTargetsHelped` | how many final link targets gain anything |
| `fanInPct` | `closureFanIn` as a percentage of the provider's `defined` symbols outside the closure, 0–100: how much of what stays behind still calls into what moved |
| `splitGain` | `linkTargetGain × (rest − fanIn) / rest`, rounded down, where `rest` is the provider's `defined` symbols outside the closure and `fanIn` is `closureFanIn`: the gain, discounted by the share of what stays behind that still calls into what moved (`linkTargetGain` itself when nothing stays behind) |

**Fan-in.** A closure the rest of the provider references heavily is a
cut through the middle of the provider: after the split the rest depends
on the new target, every program that links the rest links it too, and
the refactor is as large as the fan-in. The simulation already charges
for it where it changes what a link loads (a rest that calls into the
closure keeps the parts it calls loaded), but not for the cost of making
the cut. `splitGain` is the separate rank key that weighs that
cost (`--sort split_gain`); `linkTargetGain` stays the physical
quantity.

`link_gain` (the default sort) ranks a row that helps a program above
one whose library gain no program sees. On an LLVM testbed (13 tools,
34 static and 3 object libraries) the simulated loads match the archive
members GNU ld loads (its `-Wl,-Map`) to 96% both ways, and against the
input bytes each link saves when the true closure is split out of the
objects section by section, `linkTargetGain` ranks the 206 direct rows
with rank correlation 0.76, against 0.17 for the target-level walk it
replaced.

What it does not see:

- **The closure's error.** A closure that is too small — the usual
  cause is code reached by name at run time, through a registry — makes
  every gain too large. One that is too large (header inline code the
  compiler never emitted, followed to a symbol) can move into the new
  object a symbol the rest of the provider still needs and so free the
  members that only it kept loaded: the gain is then that of the larger
  split the closure describes, not of the smallest one.
- **Undeclared paths.** A dependency a third target uses without
  declaring is not followed, so the gain can be overstated when the
  build only works because some other path pulls the provider in.
- **Granularity.** One member per mapped source: a source compiled into
  a target through another target's objects, or objects a build adds
  outside the map (CMake folding an object library into an archive),
  are not modelled. Link order is ignored: a symbol two archives on the
  line define resolves to the one whose name sorts first, not to the
  first on the command line.

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
| `unused-link-dependency` | `declared: "direct"`; no symbol, no uncertain symbol and no indirect use; not `providesMain`; consumer and provider both covered; the index's coverage is stated and (for the call-edge fallback) it was baked without `--collapse-paths`; the index attributes at least one symbol (`defined` or `presumed`) to the provider |
| `undeclared-link-dependency` | at least one `defined` symbol referenced, and `declared` is `transitive` or `none` |
| `thin-dependency` | `declared` is `direct` or `closure`; `1 ≤ symbols + uncertainSymbols ≤ thin_max_symbols`; `closureSymbols + uncertainSymbols` is at most `thin_max_closure_pct` percent of `providerSymbols + providerPresumedSymbols` |

`unused-link-dependency` **fails closed**: a target with a source that
is missing from the index or did not parse cleanly, an index without
stated coverage, a provider the index holds no symbol of, an
indirect use, or a provider that supplies an executable's `main`
(`providesMain`: the C runtime's startup code calls it, so no consumer
object names it) yields no finding. `unusedWithheld` counts the
declared-direct rows with no symbol and no uncertain symbol that got no
finding for one of those reasons, so "no unused findings" is never
mistaken for "everything is used". When the finding is reported it is
still not a verdict: the program may rely on the provider's static
initializers (the finding then carries `providerStaticInits`, their
count), or on a symbol the
audit could not attribute (`consumerUnplacedUses` says how many of the
consumer's references that is). `consumerUnplacedUses` and
`references.unresolvedSite` are reported, never gating.

An `undeclared-link-dependency` whose every certain seed is reached
through an inline body declared in a target the consumer *does* declare
carries `throughInline`: the inline `symbol`, its `header`, the header's
`owners`, and the consumer's `site` that reaches it. The consumer's own
code names nothing of the provider; the dependency is that header's,
and the fix belongs to its owner (declare the provider as an interface
dependency) more than to the consumer.

`thin-dependency` describes the shape of an edge, not its value. Rank
thin rows by `linkTargetGain` (the default sort), `splitGain`
(`--sort split_gain`) or `droppableSymbols` (`--sort gain`), then read the closure and its fan-in with
`explain_dependency`: a thin edge onto a small leaf library is true and
worthless, a thin edge whose closure needs most of what the provider
needs saves nothing, a closure with fan-in leaves the provider's own
link as it is, and a thin finding with `"registration": true` is a
registry hook whose real use the closure cannot follow.

## `explain_dependency`

One row in full. Arguments: `target_map`, `consumer`, `provider`
(required; exact names, different targets), `closure_limit` (closure
symbols to list, default 50, `0` for none), the thin thresholds, and
`limit` / `offset` over `seeds`. A pair with no row (not declared, not
observed) answers `ok` with empty `seeds` and exit code 1.

| Member | Meaning |
|---|---|
| `edge` | the row, as in the edge table |
| `linkTargetsHelped` | each final link target the split would help: `target`, `gain` (emitted symbols it stops loading), `members` (members of which it loads nothing any more), `linkInput` and `linkInputMembers` (what it loads as the map stands) — by gain descending, then name |
| `seeds` | every provider symbol the consumer references — certain before uncertain, named by a source before via-inline, then by site. Each: `symbol` (`name`, `usr`, `kind` — `function`, `variable`, `class` — and the declaration's `file`, `line`), `owner` (`defined` / `presumed`), `via` (`direct` / `inline`), `kind` (the reference's role or added kind), `site` (where the reference to the symbol is written), `from` (the symbol that makes it), `registers: true` for a registration, and for `via: "inline"` `through`: the reference in the consumer's own files (`site`, `from`, `to`) the walk started from |
| `closure` | the closure's symbols by qualified name, then USR: `name`, `usr`, `kind`, `file`, `line` (the declaration), `definedIn` (the file that defines it, when known) |
| `closureTotal`, `closureTruncated` | the full count, and whether `closure_limit` cut the list |
| `fanIn`, `fanInTruncated` | the symbols `closureFanIn` counts, in the closure's order and shape, cut at `closure_limit` |

## Completeness

Every payload of both tools carries `completeness`:

| Member | Meaning |
|---|---|
| `edgeSource` | `"references"`, or `"call-edges"` for an index without reference facts — what the references were derived from |
| `unusedIsUniversal` | `false`: an unused finding is not a proof |
| `invisible` | the kinds of link dependency this evidence cannot see, as stable tags (below) |
| `depsForm` | the map's form |
| `coverageStated` | whether the index states its coverage (`indexScope`) and carries the per-TU tables attribution needs |
| `edgesCollapsed` | call-edge fallback only: the index was baked with `--collapse-paths`; the `collapsed-call-edges` tag is then present and no unused finding is raised. Reference facts are recorded whatever `--collapse-paths` says |
| `targets` | `total`, `withSources`, `covered` |
| `sources` | `mapped` (distinct sources in the map), `indexed` (parsed cleanly), `failed`, `notIndexed`, `unmappedTus` |
| `symbols` | `defined`, `presumed`, `inline`, `unattributed`; with reference facts also `undefinedInCoveredOwner` (declared in a header whose owners are all fully indexed, defined by no indexed TU), `emittedEverywhere` (weak or `used` header definitions held by every includer) and `seenBy` (attributed symbols whose declarations only some targets' TUs open; an inline body's reference to one is taken only for those) |
| `references` | `anchored` (held by a target's objects), `inlineBody` (travel with an inline body), `virtualDispatchSkipped` (dispatched virtual calls, soft), `deferredSkipped` (call-edge fallback: function-pointer returns), `unmappedBody` (made by code of an indexed source no target lists), `unresolvedSite` (the site's file matched no known file); with reference facts also `overloadCandidates` (soft), `siteAnchored`, `tuScoped` (inline-body references only TUs of one target set recorded, taken only for that set), `vtableEntries`, `baseClasses`, `inheritedVirtuals`, `baseClassesUnseen`, `fileScopeDropped` |

`invisible` with reference facts:

| Tag | What is missed |
|---|---|
| `qualified-virtual-calls` | without the call graph, a call of a virtual function bound statically (`Base::run()` from an override) cannot be told from a dispatched one and is soft |
| `same-basename-internal-symbols` | two internal symbols of one name in files of one basename share an identity (`docs/symbol-references.md`, "Limits") and their facts merge |
| `name-based-lookup` | `dlsym`, plugin registries, linker sections, `--whole-archive` |
| `other-build-configurations` | code under an `#if` the indexed configuration did not take; other platforms' sources |

Known ways an answer is wrong, beyond what the tags say:

- **The closure is too small where a registry does the work.** A seed
  that hands a factory to a registry has a closure of itself and the
  factory: what the registry later builds through it is reached at run
  time. Every gain column of such a row is too large;
  `registrationSeeds` and reading the closure are the checks.
- **The closure is too large through a template.** A template's
  references are the union of every instantiation the index read,
  narrowed only to the target set of the TUs that recorded them ("Inline
  bodies and the closure"), so a closure through `make<T>()` includes
  what the other `T`s' constructors need when TUs of the same target, or
  of several, instantiate them.
- **Compiler-written code is read, with the approximations
  `docs/symbol-references.md` lists under "Limits"** (an exception path
  judged from the form of the code, instantiations a TU names but does
  not define).
- **Seeds can be compile-time only.** A call the optimizer folded or an
  inline body the compiler never emitted is still a reference here; the
  objects do not need the symbol.
- **Overload candidates are soft.** A call that a template resolves to
  one of several same-named candidates names no seed; when the
  candidates are the consumer's only use of a provider the row shows an
  indirect use, not a symbol.
- **An inline body carries every reference it makes** in every target
  that uses it, whatever the optimizer removes.
- **A function template named `operator<`** spells its instantiations
  `operator<<#...`, the same as `operator<<` itself; the name is read as
  the stream inserter.
- **A source another translation unit includes** (a unity build, an
  `#include "impl.cpp"`) counts as indexed when the including unit
  parsed cleanly, though its own compile command was never used.
- **A call site the index cannot match to one file**
  (`references.unresolvedSite`) is still charged to its referrer's
  owner, and no finding is withheld for it.
- **The map and the index are not checked against each other.** A map
  written from another checkout or configuration than the index was
  baked from is compared as it stands.
- **A partial index** can make a shared header look private: only the
  indexed translation units vote. `consumerCovered` / `providerCovered`
  say when that can be the case.
- **A generated header** is private to nobody unless the build
  directory is among `roots` (the CMake adapter adds it).

### Call-edge fallback

An index without reference facts is audited from its call edges. A
function is placed by where its calls are written, in six rules applied
in order:

| # | The function | Class | Owner |
|---|---|---|---|
| 1 | makes a call written in a target's file | `defined` | the targets of every such file |
| 2 | makes no such call, but is declared in a target's file | `defined` | the targets of that file |
| 3 | makes calls, all written in shared headers | `inline` | none |
| 4 | makes no call and is template code (its USR names an instantiation or a member of a class template) | `inline` | none |
| 5 | makes no call and is declared in a shared header that has an owner | `presumed` | the header's owner |
| 6 | anything else | unattributed | none |

A reference is held by the target whose file its call site is written
in, or by a `defined` function's owners when the site is a shared header
(a macro or an `.inc` body expanded there). An unproven
`VirtualDispatch` call and a `FunctionPointerReturn` call are soft. Its
`invisible` tags add what call edges cannot see: `variable-references`,
`vtable-and-typeinfo-references`, `static-initializer-calls`,
`implicit-destructor-calls`, `calls-in-template-bodies`,
`definitions-without-calls` (a leaf is placed by its declaration alone,
so a leaf defined in a source and declared in a shared header is
`presumed` or unattributed), and `collapsed-call-edges` for an index
baked with `--collapse-paths`. Re-index to audit from references.

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
  `all names` compares every class: a vtable or typeinfo symbol is
  matched to the audit's seed on its class (`class Name`), a function or
  variable by qualified name. Under the call-edge fallback everything
  but functions is invisible by construction.
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
arguments, so a set of overloads counts once on both sides. The
comparison measures seeds and findings; it says nothing about closure
size, which only reading `explain_dependency` against the provider's
code checks.

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
  with an owning target set, a class and flags, and the hard and soft
  references between them, in flat CSR arrays over interned ids.
  `linkFactsFromReferences` fills it from the reference facts (the rules
  above), `linkFactsFromCallEdges` from the call graph (the fallback).
  `auditDependencies` is the kernel: it reads `LinkFacts` and the map
  only, and computes seeds, closures, needs, fan-in and gains with bit
  matrices and stamp arrays sized once per audit. The link-target gain
  runs on a member table built once per audit (`LinkModel`: per member
  its symbols, what its code references and its weight, in CSR form) and
  a simulator (`LinkSim`) whose queue and stamps are sized once; a row's
  split adds its parts and rests to the simulator's node arrays, cleared
  and refilled per row without reallocating once they have grown. On
  the LLVM testbed above (892 sources, 50 targets) the whole audit runs
  in under a second in 340 MB, most of it the loaded references.
  `completeness.edgeSource` and `invisible` come from the producer.

`src/query/DependencyTools.cpp` holds the two tools; they read the
references section and the graph (`ToolEntry::needs`).
