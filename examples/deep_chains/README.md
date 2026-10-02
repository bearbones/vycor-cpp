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
| `gen_compile_commands.sh` | Writes `compile_commands.json` at runtime with absolute paths. `megascope` takes `--build-path` to this directory. |

## How to analyze

```bash
# From repo root, after building vycor-cpp:
( cd examples/deep_chains && ./gen_compile_commands.sh )

./build/src/vycor-cpp megascope index \
  --build-path examples/deep_chains \
  --source-re 'examples/deep_chains/.*\.cpp$' \
  --entry-point main

./build/src/vycor-cpp megascope find-call-chain \
  --build-path examples/deep_chains --from main --to stage5_sink --pretty
```

`scripts/cli-golden.py` runs a fixed set of queries over this fixture and
compares them with `cli-golden/`.

## Confidence invariants

This fixture relies on exact behavior of `CallGraphBuilder.cpp`:

- `DirectCall` / `ConstructorCall` / `OperatorCall` / `DestructorCall` → **Proven**.
- `VirtualDispatch` via a base ref/ptr or a `vector<unique_ptr<Base>>` → **Plausible** (fans out to base + overrides).
- `VirtualDispatch` via a concrete local var → **Proven** (extra edges from `addConcreteTypeEdges`; the call itself also emits Plausible).
- `FunctionPointer` taken as `&freeFn` in a non-call, non-argument context → **Plausible**.
- `FunctionPointer` passed directly as a call argument, or through a tracked return value → **Proven**.

If you change a stage, confirm that the new edge kinds still hit both Proven
and Plausible; `expected_chains.json` encodes the contract.
