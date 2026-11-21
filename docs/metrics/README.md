# Metrics

Some findings are measurements, not warnings. A **metric** is a named
measurement over the index, selected the way checks are and reported as
records with one shape. Nothing passes or fails: a metric says how much,
where, and why; what to do about it is the reader's call.

```bash
vycor-cpp metrics --list-metrics
vycor-cpp metrics --build-path build --source-re 'src/.*\.cpp$'
vycor-cpp metrics --build-path build --source-re 'src/.*\.cpp$' \
    --metrics=static-init-cost --rollup tu --format ndjson
```

`vycor-cpp metrics` is the megascope tool `metrics` under a top-level
name: `vycor-cpp megascope metrics`, a `batch` request with
`"tool": "metrics"`, and `megascope call metrics` run the same handler
and answer with the same payload. Every flag below is that tool's schema
(`vycor-cpp metrics --help`), and the index flags are megascope's
(`docs/megascope-usage.md`).

## Built-in metrics

| Metric | Unit | Native level | Summary |
|---|---|---|---|
| [redundant-instantiation](redundant-instantiation.md) | statements | symbol | Function bodies compiled by many TUs: each inline function, template instantiation and header-defined internal function two or more TUs emit, as copies × statements |
| [static-init-cost](static-init-cost.md) | functions | symbol | Work done before `main`: what each dynamic initializer and constructor function reaches, and whether that reach allocates, locks or does I/O |

`vycor-cpp metrics --list-metrics` prints the live table, organization
metrics included, as records `{metric, summary, unit, level}`. It reads
no index.

## Selecting metrics

`--metrics=<spec>` takes the syntax of `--checks`: comma-separated
entries applied left to right, `name` enables, `-name` disables, `all`
is every metric. A spec that opens with a removal starts from everything
(`--metrics=-static-init-cost` is "all but"). With no `--metrics`, every
metric runs. An unknown name is a usage error that lists the known ones.

A metric whose facts the index does not hold makes the whole run
`unavailable` (exit 3) and says which facts are missing. An absent
measurement is never reported as zero, and never silently dropped from a
report that asked for it.

## The record

```json
{
  "metric": "static-init-cost",
  "entity": {
    "kind": "variable",
    "name": "reg::coreSlot",
    "usr": "c:registry.cpp@N@reg@coreSlot",
    "file": "/src/core/registry.cpp",
    "line": 50
  },
  "value": 6,
  "unit": "functions",
  "components": {"directCalls": 1, "allocation": 1, "lock": 1, "io": 0, "tus": 1},
  "witness": {"lock": ["reg::registerPlugin", "pthread_mutex_lock"]}
}
```

| Member | Meaning |
|---|---|
| `metric` | The metric's name. |
| `entity` | What was measured. All five members are always present; one a kind does not use is `""` or `0`. `kind` is the metric's own at the symbol level (`variable`, `constructor-function`, …) and the roll-up level's name above it (`template`, `file`, `tu`, `directory`, `target`). `name` is a qualified name at the symbol level and the group's key above it: a path, or a target name. |
| `value` | The measurement: one integer, in `unit`. The number records are ordered by. |
| `unit` | What `value` counts. |
| `components` | Optional. Named integers that break the value down or sit beside it. Each metric's page defines its own. A rolled-up record always carries `entities`, the number of measured entities in the group. |
| `witness` | Optional. Evidence for the value: a call chain, an include chain. Free-form per metric and defined on its page. |

## Roll-ups

`--rollup <level>` chooses what a record stands for.

| Level | A record is | Grouped by |
|---|---|---|
| `symbol` (default) | one measured entity | — |
| `template` | a template pattern, or a non-template function | the pattern the entity instantiates (metrics whose samples name one; otherwise the entity itself) |
| `file` | a file | the file the entity is written in |
| `tu` | a translation unit | the TUs the entity belongs to |
| `directory` | a directory | the directory of each of those TUs |
| `target` | a link target | the targets that list each of those TUs as a source |

An entity can belong to several TUs: an initializer written in a header
runs in every TU that includes it, and counts in each of their rows.
The same holds one level up for a TU that several targets compile. Group
rows therefore do not sum to the total.

How a group's value is derived from its members is the metric's
decision, because it depends on what the value means: additive metrics
sum, a reach is a union. The metric's page says which.

A metric measured at a coarser level than the one asked for reports at
its own level. The `metrics` member says which level each metric's
records are at.

### Target map

The `target` roll-up needs to know which target owns each TU, which a
compilation database does not say. `--target-map <file>` names a JSON
document that does:

```json
{
  "schema": 1,
  "root": ".",
  "targets": [
    {"name": "core", "kind": "static_library", "sources": ["core/registry.cpp"]},
    {"name": "plugins", "kind": "executable",
     "sources": ["plugins/plugins.cpp", "plugins/codecs.cpp"], "deps": ["core"]}
  ]
}
```

Roll-ups read `targets[].name`, `targets[].sources` and `root`; other
members are ignored here. Relative sources resolve against `root`, which
itself resolves against the map file's directory (the default when there
is no `root`). Paths are compared absolute and lexically normalized. A
TU no target lists is grouped as `(no target)`. A TU several targets
list counts in each.

`--target-map` without `--rollup target` is a usage error, as is
`--rollup target` without a map.

## The payload

```json
{
  "status": "ok",
  "indexScope": {"freshness": "baked", "complete": true, "requested": 3, "indexed": 3, "partial": 0, "failed": 0},
  "rollup": "symbol",
  "metrics": [
    {"metric": "static-init-cost", "unit": "functions", "level": "symbol", "records": 8,
     "overall": {"value": 12, "components": {"dynamicInitializers": 7, "constructorFunctions": 1, "directCalls": 8,
                                             "allocation": 4, "lock": 3, "io": 3, "entities": 8}}}
  ],
  "records": [ ... ],
  "total": 8, "offset": 0, "limit": 200, "returned": 8, "truncated": false
}
```

- `status` and `indexScope` are on every payload, errors included
  (`docs/result-contract.md`). `indexScope` says how much of the
  requested TU set the measurement covers: a metric over an index with
  failed TUs measures less than the program.
- `metrics` has one entry per selected metric, in name order: its unit,
  the level its records are at, how many records it has, and `overall`,
  the measurement over everything, derived the way a group's is. It
  does not depend on the roll-up or on paging.
- `records` is one list for all selected metrics: grouped by metric
  name, and within a metric by `value` descending, then entity file,
  line, name, usr and kind (`docs/deterministic-output.md`). The same
  index gives the same bytes whatever the thread count or TU order of
  the bake.
- `--limit` / `--offset` page through `records` (default limit 200).
  `total` is the full count and `nextOffset` where the next page starts.

`--format ndjson` writes a `{"_summary": ...}` line carrying everything
but the records, then one record per line. `--format tsv` writes one row
per record, with `entity`, `components` and `witness` as JSON cells.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Records returned |
| 1 | Nothing measured: no records |
| 2 | Usage error: unknown metric, unknown roll-up, missing or unreadable target map |
| 3 | `unavailable`: the index is missing or does not hold a selected metric's facts |

There is no failing threshold. A metric run never exits non-zero because
a value is large.

## Where the facts come from

A metric reduces facts the megascope bake already collected; it does not
parse. Each metric's page names the facts it reads and whether a saved
index carries them. `redundant-instantiation` reads the emission facts
of the references section (format v20), which every bake records. `static-init-cost` reads facts that today exist only
in an in-memory bake, so it is run with `--source`, `--source-list` or
`--source-re` rather than against a saved index.

A request that cannot be answered whatever the index holds (an unknown
metric, a malformed roll-up) is answered before any index is loaded or
baked, as is `--list-metrics`.

## Organization metrics

Metrics register the way organization checks do
([docs/EXTENDING.md](../EXTENDING.md)). Implement `vycor::Metric`
(`include/vycor/metrics/Metric.h`) in `ext/` and register it:

```cpp
#include "vycor/ext/Extensions.h"

class VtableCountMetric : public vycor::Metric {
public:
  std::string name() const override { return "myorg-vtable-count"; }
  std::string summary() const override { return "Polymorphic classes per TU"; }
  std::string unit() const override { return "classes"; }

  std::unique_ptr<vycor::MetricRun>
  measure(const vycor::MetricContext &ctx) const override;
};

VYCOR_REGISTER_METRIC(VtableCountMetric)
```

`measure` returns a `MetricRun` holding one `MetricSample` per measured
entity: its entity, value, components, witness, and the TUs it belongs
to. The framework orders the records, rolls them up, pages them and
stamps the result contract. Override `MetricRun::rollUp` when a group's
value is not the sum of its members'; override `Metric::unavailable`
when the measurement needs facts an index may lack; override
`Metric::nativeLevel` when the samples are per file or per TU rather
than per symbol.

A registered metric appears in `--list-metrics`, is selected by name in
`--metrics`, and runs by default like the built-in ones. A name that
collides with a built-in metric is ignored. Document it as
`docs/metrics/<name>.md` in your fork.

Metrics that ask "does this reach an allocation, a lock, I/O" share one
effect table (`include/vycor/callgraph/Effects.h`), extended through the
org config's `effects` key or `registry.addEffectFunctions(...)`.
