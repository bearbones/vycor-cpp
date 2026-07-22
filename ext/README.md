# ext/ — organization extension slot-in

This directory is the designated place for **fork/mirror-local
customizations**. Upstream ships it effectively empty (this README and
`examples/` only), and upstream code never depends on its contents — so an
organization fork can fill it with proprietary checks and annotations and
still merge upstream cleanly, forever.

## What gets built

- `ext/*.cpp` — compiled **directly into** the `vycor-cpp` executable and
  the `vycor_tests` binary (picked up by a CMake glob; re-run of CMake is
  triggered automatically via `CONFIGURE_DEPENDS`).
- `ext/tests/*.cpp` — compiled into `vycor_tests` only. Write ordinary
  Catch2 `TEST_CASE`s here for your own checks.
- Anything else (including `ext/examples/`) is **not** compiled.

Extension sources are attached to the executable targets rather than
`vycor_lib` on purpose: registration happens via static initializers, and
the linker would drop an unreferenced member of a static archive.

## What you can register

See `include/vycor/ext/Extensions.h` for the full API and
`docs/EXTENDING.md` for the guide. In short:

| Hook | Registered via | Effect |
|---|---|---|
| Custom anneal check | `VYCOR_REGISTER_ANNEAL_CHECK(MyCheck)` | Runs per TU after the built-in analyzer; emits `Diagnostic::Custom` |
| Lock types | `registry.addLockTypes({...})` | Recognized as `RaiiKind::Lock` in RAII/lock tracking (megascope/prism) |
| Channel types | `registry.addChannelTypes({...})` | Producer/consumer call-site tracing (ChannelIndex) |
| Guard classifiers / feature flags | `registry.addFeatureFlagPattern(...)` / `addGuardClassifier(...)` | Annotates conditional guards in prism dump + MCP responses (e.g. "this path only runs with FFlag::X on") |

Everything except the custom checks can also be configured **without
code** through an org config JSON file passed as `--org-config` — see
`examples/vycor.org.json`. A typical fork checks that file in here and
wires it into its build/CI invocations.

## Getting started

```bash
cp ext/examples/ExampleOrgExtension.cpp ext/MyOrgExtension.cpp
# edit, then re-run your normal build; CMake picks it up automatically
cmake --build build --target vycor-cpp vycor_tests
```

Keep each check's `name()` stable: it is the key used by
`disabledAnnealChecks` in the org config and is surfaced in diagnostics.
