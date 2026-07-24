# Extending vycor-cpp for your organization

This guide is for organizations that fork or mirror this repository and
want to add their own analysis semantics — custom anneal checks, in-house
lock types, feature-flag conventions, channel/queue types — **without
editing any upstream file**. Everything org-specific lives in two places
you own outright:

1. **An org config JSON file** (`--org-config`) — declarative, no
   compilation needed. Covers lock types, channel types, feature-flag
   patterns, collapse paths, and disabling checks.
2. **The `ext/` directory** — compiled C++ slot-in for anything that needs
   AST access (custom anneal checks, arbitrary guard classifiers).

Because upstream never touches `ext/` contents (only `ext/README.md` and
`ext/examples/` are tracked) and never claims the `--org-config` file
name, pulling upstream `main` into a fork carrying both never conflicts.

The ad-hoc CLI flags (`--lock-types`, `--channel-types-json`,
`--collapse-paths`) still exist and are merged with — never replaced by —
the org config, so one-off experiments don't require editing the checked-in
config.

---

## Tier 1: the org config file (`--org-config`)

A single JSON file, typically checked into your fork (convention:
`ext/vycor.org.json`, but any path works) and passed to `anneal`, `prism`,
and `megascope`:

```bash
vycor-cpp megascope --build-path build --source ... --org-config ext/vycor.org.json
```

Schema (every key optional; unknown keys are an error, so typos fail
loudly — see `include/vycor/ext/OrgConfig.h`):

```json
{
  "lockTypes": ["myorg::SpinLockGuard", "myorg::SharedStateGuard"],
  "channelTypes": [
    {
      "type": "myorg::EventBus",
      "produce": ["post", "postDeferred"],
      "consume": ["drain", "poll"],
      "category": "bus"
    }
  ],
  "featureFlags": [
    { "pattern": "FFlag::([A-Za-z0-9_]+)", "nameGroup": 1 },
    { "pattern": "isFeatureEnabled\\(\"([A-Za-z0-9_.-]+)\"\\)", "nameGroup": 1 }
  ],
  "collapsePaths": ["ThirdParty/Math"],
  "disabledAnnealChecks": ["some-ext-check"]
}
```

### `staticInitHazards`

Qualified function names the [static-init-hazards](checks/static-init-hazards.md)
check treats as loader-hostile in addition to its built-in
dlopen/thread set — your "never during static initialization" bank.
Also registrable in code: `registry.addStaticInitHazards({...})`.

### `lockTypes`

Qualified type names treated as locks (`RaiiKind::Lock`) by the RAII/lock
tracking that backs `query_raii_scopes_at_callsite`, `query_locks_held`,
and `query_same_lock`. Merged into the built-in allowlist
(`std::lock_guard`, `absl::MutexLock`, ...) and any `--lock-types` flags.
Types annotated with Clang's `ScopedLockableAttr` (Thread Safety
Annotations) are recognized automatically and don't need listing.

### `channelTypes`

Producer/consumer call-site tracing for your queues/buses/maps — the same
schema as `--channel-types-json` (see `ChannelIndex.h`). `type` is the
canonical type name without the `struct`/`class` keyword; template
registrations are per instantiation.

### `featureFlags`

Patterns that classify **conditional guards** as feature-flag checks. Each
`pattern` is an extended regex searched (not anchored) against a guard's
condition source text; `nameGroup` selects the capture group holding the
flag name (`0` = whole match). Matching guards gain an `annotation` object
in every place guards are reported:

```json
{
  "conditionText": "FFlag::NewNav && user.isAdmin()",
  "inTrueBranch": true,
  "isAssertion": false,
  "annotation": { "kind": "feature-flag", "name": "NewNav" }
}
```

Combined with `inTrueBranch`, this answers "this call site / channel send
runs only with `NewNav` **on**" (or off, when `inTrueBranch` is false).
Surfaced in `prism --mode dump`, `prism --query-type call-site-context`
(via the MCP-equivalent field), the `query_call_site_context` MCP tool, and
channel-site listings.

Classification happens at **query/serialization time**, not at index time —
adding or changing patterns does not invalidate megascope `--snapshot`
warm starts. (Changing `lockTypes`/`channelTypes` *does* invalidate a
snapshot, deliberately: they alter what gets indexed.)

### `collapsePaths`, `disabledAnnealChecks`

`collapsePaths` entries merge with `--collapse-paths` flags.
`disabledAnnealChecks` suppresses compiled `ext/` checks by their
`name()` — useful for staged rollouts of a new check across a large fork.

---

## Tier 2: compiled extensions in `ext/`

For hooks that need AST access, drop `.cpp` files directly in `ext/`:

- `ext/*.cpp` → compiled into the `vycor-cpp` executable **and**
  `vycor_tests` (CMake glob, `CONFIGURE_DEPENDS` — no build-file edits).
- `ext/tests/*.cpp` → compiled into `vycor_tests` only; write ordinary
  Catch2 `TEST_CASE`s for your checks there.

A complete working example lives at `ext/examples/ExampleOrgExtension.cpp`
(the `examples/` subdirectory is deliberately outside the glob; copy the
file up one level to activate it).

### Custom anneal checks

Implement `vycor::AnnealCheck` (`include/vycor/ext/Extensions.h`) and
register it:

```cpp
#include "vycor/ext/Extensions.h"

class NoLegacyAllocatorCheck : public vycor::AnnealCheck {
public:
  std::string name() const override { return "no-legacy-allocator"; }
  void checkTU(clang::ASTContext &context, const vycor::GlobalIndex &index,
               std::vector<vycor::Diagnostic> &out) override {
    // Walk the TU (e.g. with a RecursiveASTVisitor member) and push
    // Diagnostic entries with kind = Diagnostic::Custom,
    // checkName = name().
  }
};

VYCOR_REGISTER_ANNEAL_CHECK(NoLegacyAllocatorCheck)
```

Semantics:

- One fresh instance per TU (the factory is invoked per TU), so member
  state needs no reset between TUs.
- Runs inside `vycor-cpp anneal`, per TU, **after** the built-in
  ADL/CTAD analysis of that TU. The two-phase pipeline still applies: the
  `GlobalIndex` you receive was fully populated by phase 1 across all TUs.
- Diagnostics print through the standard anneal output
  (`location: message`), so downstream tooling needs no changes.
- `name()` is the stable ID used by `disabledAnnealChecks`; keep it
  kebab-case and don't rename casually.

### Cross-TU checks over the merged index

When the invariant is "must agree across TUs" rather than "walk one TU's
AST", implement `vycor::IndexCheck` instead: it runs once, after phase 1,
against the merged project-wide `GlobalIndex` (the same vantage point as
the built-in ODR and coverage analyses):

```cpp
class IpcStructParity : public vycor::IndexCheck {
public:
  std::string name() const override { return "ipc-struct-parity"; }
  void check(const vycor::GlobalIndex &index,
             std::vector<vycor::Diagnostic> &out) override {
    // e.g. compare OdrEntry hashes of client/ vs server/ definitions,
    // or verify every registered message type has a serializer overload.
  }
};

VYCOR_REGISTER_INDEX_CHECK(IpcStructParity)
```

Both check kinds participate in the `--checks` selection by their
`name()` (default enabled), and both should ship a documentation page at
`docs/checks/<name>.md` in your fork — same convention as the built-in
checks (docs/checks/README.md). Orgs can also define selection groups:

```cpp
VYCOR_EXTENSION_SETUP(MyOrgGroups) {
  registry.addCheckGroup("myorg-strict",
                         {"ipc-struct-parity", "odr-violations"});
}
```

### Lock types, channel types, feature flags — in code

For hooks that are code-level policy rather than per-repo config, register
them at static-init time:

```cpp
VYCOR_EXTENSION_SETUP(MyOrgHooks) {
  registry.addLockTypes({"myorg::SpinLockGuard"});
  registry.addChannelTypes({...});                       // ChannelTypeSpec
  registry.addFeatureFlagPattern("FFlag::([A-Za-z0-9_]+)", 1);
}
```

### Arbitrary guard classifiers

`featureFlags` patterns cover the regex-shaped cases. When classification
needs real logic, register a `GuardClassifier` — any callable from
`ConditionalGuard` to `std::optional<GuardAnnotation>`:

```cpp
VYCOR_EXTENSION_SETUP(KillSwitchClassifier) {
  registry.addGuardClassifier(
      [](const vycor::ConditionalGuard &g)
          -> std::optional<vycor::GuardAnnotation> {
        if (g.isAssertion && g.conditionText.find("kill_switch") !=
                                 std::string::npos)
          return vycor::GuardAnnotation{"kill-switch", g.conditionText};
        return std::nullopt;
      });
}
```

Classifiers run in registration order; the first non-`nullopt` result wins.
The `kind` string is free-form — `"feature-flag"` is the convention used by
`featureFlags` patterns, but org classifiers can invent their own kinds.

### Why `ext/` sources are linked into the executable, not `vycor_lib`

Both macros register through static initializers. `vycor_lib` is a static
archive, and linkers drop archive members nothing references — which would
silently discard your registrations. The build therefore attaches
`ext/*.cpp` object files directly to the `vycor-cpp` and `vycor_tests`
targets, where they are always linked. If you build a custom binary
against `vycor_lib`, do the same.

---

## How the pieces meet at runtime

```
static init:        ext/*.cpp registrars fill ExtensionRegistry
CLI startup:        --org-config parsed -> applyOrgConfig() adds its
                    lock/channel types + feature-flag patterns to the same
                    registry
composition (main): registry lock/channel types merged into the CLI-built
                    LockTypeConfig / ChannelTypeConfig (CLI entries first,
                    duplicates dropped); org collapsePaths appended
index build:        merged configs drive RAII/lock and channel indexing
                    (and land in snapshot meta for warm-start validation)
anneal per TU:      built-in analyzer, then each registered AnnealCheck
                    not listed in disabledAnnealChecks
query/serialize:    guard classifiers annotate guards in prism dump,
                    query_call_site_context, and channel-site listings
```

`--isolate-workers` note: worker processes receive the already-merged lock
list via their spawn flags, so org lock types work there too. Channel
types remain unsupported with `--isolate-workers` (pre-existing
limitation, warned at startup).

## Testing your extensions

Put Catch2 tests in `ext/tests/*.cpp`; they build into `vycor_tests`
alongside upstream tests. Pattern to follow:
`tests/test_extensions.cpp` (upstream's own coverage of this machinery)
runs `AnalyzerAction` over in-memory code with
`clang::tooling::runToolOnCodeWithArgs` — cheap per-check unit tests with
no compilation database required.

One caveat: upstream's `test_extensions.cpp` clears the process-wide
`ExtensionRegistry` in sections needing isolation. Org tests should
register the hooks they exercise explicitly instead of relying on
static-init registrations surviving the whole test-binary run.

## Fork upgrade checklist

- Keep everything under `ext/` + your org config file; never patch
  upstream sources for org semantics. If an org need can't be expressed
  through these hooks, prefer upstreaming a new hook over carrying a
  diff.
- After pulling upstream: rebuild, run `vycor_tests` (your `ext/tests/`
  run in the same binary), and re-check `docs/EXTENDING.md` /
  `include/vycor/ext/Extensions.h` for new hook points.
