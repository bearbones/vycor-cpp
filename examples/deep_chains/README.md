# deep_chains

A call-graph fixture designed to exercise navigation across chains that are
at least five layers deep, where **every** layer emits a mix of **certain**
(`Confidence::Proven`) and **uncertain** (`Confidence::Plausible`) edges.

Complements `examples/dead_code/` (which tops out at ~4 layers and is organized
around liveness, not path navigation).

## Chains

### Chain A — concrete-to-virtual pipeline (6 layers)

```
main
 └── Pipeline::run                   [DirectCall, Proven]
      └── stage1_ingest              [DirectCall, Proven]     + &defaultHasher stored [FnPtr, Plausible]
           └── stage2_parse          [DirectCall, Proven]     + &logAfter stored     [FnPtr, Plausible]
                └── stage3_transform [DirectCall, Proven]     + &normalizePayload    [FnPtr, Plausible]
                     └── stage4_dispatch [DirectCall, Proven] + Plugin::handle via base ptr
                                                                [VirtualDispatch, Plausible → Alpha/Beta/Gamma]
                          └── stage5_sink  [DirectCall, Proven] + &finalFormat     [FnPtr, Plausible]
```

### Chain B — virtual-scheduler chain (6 layers)

```
main
 └── Pipeline::runAsync              [DirectCall, Proven]     + &asyncCompleted      [FnPtr, Plausible]
      └── Scheduler::schedule        [DirectCall, Proven]
           └── Worker::execute       [VirtualDispatch, Plausible — Worker& parameter]
                └── NetworkWorker::execute                    (also DiskWorker::execute via fan-out)
                     └── tcpWriteBytes [DirectCall, Proven]   + &finalFormat        [FnPtr, Plausible]
```

## Files

| File | Purpose |
|---|---|
| `main.cpp` | Entry. Kicks off both chains. |
| `pipeline.{hpp,cpp}` | `Pipeline::run` (A) and `Pipeline::runAsync` (B). |
| `stage1_ingest` … `stage5_sink` | Chain A stages. |
| `scheduler.{hpp,cpp}` | Chain B scheduler holding a `Worker&`. |
| `workers.{hpp,cpp}` | `Worker` base + `NetworkWorker` / `DiskWorker`. |
| `plugins.{hpp,cpp}` | `Plugin` base + `PluginAlpha` / `PluginBeta` / `PluginGamma`. Used by stage4 through a `vector<unique_ptr<Plugin>>`. |
| `tokenizer.{hpp,cpp}` | `Tokenizer` base + `JsonTokenizer` / `TextTokenizer`. Used by stage2 helper. |
| `callbacks.{hpp,cpp}` | Free functions (`&cbs::defaultHasher` etc.) + a `Registry` struct whose members stash fn pointers — the primary way Plausible FunctionPointer edges are generated. |
| `expected_chains.json` | Test oracle: per-chain paths, required edges with `kind`+`confidence`, and per-layer "must have Proven + Plausible" assertions. |
| `gen_compile_commands.sh` | Writes `compile_commands.json` at runtime with absolute paths. The MCP CLI takes `--build-path` to this directory. |

## How to analyze

```bash
# From repo root, after building vycor-cpp:
( cd examples/deep_chains && ./gen_compile_commands.sh )

./build/vycor-cpp megascope \
  --build-path examples/deep_chains \
  --source examples/deep_chains/main.cpp \
  --source examples/deep_chains/pipeline.cpp \
  --source examples/deep_chains/stage1_ingest.cpp \
  --source examples/deep_chains/stage2_parse.cpp \
  --source examples/deep_chains/stage3_transform.cpp \
  --source examples/deep_chains/stage4_dispatch.cpp \
  --source examples/deep_chains/stage5_sink.cpp \
  --source examples/deep_chains/plugins.cpp \
  --source examples/deep_chains/workers.cpp \
  --source examples/deep_chains/tokenizer.cpp \
  --source examples/deep_chains/scheduler.cpp \
  --source examples/deep_chains/callbacks.cpp \
  --entry-point main
```

Or use `scripts/mcp-smoke.py` which does this automatically and dumps
request/response artifacts under `scripts/mcp-smoke-out/`.

## Confidence invariants

This fixture relies on exact behavior of `CallGraphBuilder.cpp`:

- `DirectCall` / `ConstructorCall` / `OperatorCall` / `DestructorCall` → **Proven**.
- `VirtualDispatch` via a base ref/ptr or a `vector<unique_ptr<Base>>` → **Plausible** (fans out to base + overrides).
- `VirtualDispatch` via a concrete local var → **Proven** (extra edges from `addConcreteTypeEdges`; the call itself also emits Plausible).
- `FunctionPointer` taken as `&freeFn` in a non-call, non-argument context → **Plausible**.
- `FunctionPointer` passed directly as a call argument, or through a tracked return value → **Proven**.

If you change a stage, confirm that the new edge kinds still hit both Proven
and Plausible; `expected_chains.json` encodes the contract.
