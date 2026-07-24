# dead-code

**Default:** off · **Groups:** compute-heavy · **Diagnostics:** `DeadCode_Pessimistic`, `DeadCode_Optimistic`

Reports functions unreachable from the program's entry points, using the
cross-TU call graph.

## Why no other tool catches this

Per-TU tools can only flag unused `static`/anonymous-namespace functions —
for anything with external linkage, "unused in this TU" proves nothing.
Reachability is a whole-program property: it needs every call edge from
every TU, plus virtual-dispatch and function-pointer resolution. anneal
builds the same cross-TU call graph megascope serves and walks it from the
declared entry points:

- **`DeadCode_Pessimistic`** — unreachable even through *Plausible* edges
  (virtual dispatch fan-out, function pointers): strong dead-code signal.
- **`DeadCode_Optimistic`** — reachable only through Plausible edges:
  review before deleting.

## Usage

```bash
vycor-cpp anneal --build-path build --source ... \
  --checks=dead-code --entry-point main --entry-point "PluginRegistry::init"
```

Entry points default to `main`; list every external entry (plugin hooks,
test frameworks, exported APIs) to avoid false positives. Grouped as
**compute-heavy**: enabling it builds the full call graph in addition to
the anneal index.
