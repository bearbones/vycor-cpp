# static-init-hazards

**Default:** off · **Groups:** compute-heavy · **Diagnostics:** `StaticInit_Hazard`

Walks the cross-TU call graph from every static-initialization root —
dynamic initializers of globals, and `__attribute__((constructor))`
functions — and flags paths that reach **loader-hostile work**: the
dynamic linker (`dlopen`, `dlsym`, `dlclose`, `dladdr`), thread
creation/joining (`pthread_create`/`join`, `std::thread`
construction/join), `std::async`, `std::call_once`.

## Why this matters (a war story shape)

When a shared library is loaded with `dlopen` — which is exactly what
Android's `System.loadLibrary` does for JNI native libraries, and what
every plugin system does — its static initializers run **while the dynamic
linker's global lock is held**. If an initializer:

- re-enters the loader (`dlopen`/`dlsym` of another library), or
- spawns a thread and waits on it, where the new thread itself needs the
  loader lock (first TLS access, its own `dlopen`, `dlerror`…),

the process can deadlock. Worse, whether it fires is **link-order
dependent**: initializer order follows `DT_NEEDED`/link order, so an
innocent refactor of *how a library is linked* can flip a working build
into one that hangs at `System.loadLibrary` — and the investigation
usually burns days, because nothing crashed and the stack is inside the
loader.

No single-TU tool can see this: the `dlopen` is typically several calls
deep, across files, from an initializer that looks harmless. anneal has
the initializer roots (phase-1 index) and the cross-TU call graph, so the
reachability question is answerable at PR time.

## Example

```cpp
namespace myorg {
void *loadPlugin() { return dlopen("plugin.so", RTLD_NOW); }
struct Loader { Loader() { handle = loadPlugin(); } void *handle; };
Loader gLoader;   // runs under the loader lock when this .so is dlopen'd
}
```

```
loader.cpp:9: Static-init hazard: the dynamic initializer of 'myorg::gLoader'
    (loader.cpp:9) reaches 'dlopen' (myorg::Loader::Loader -> myorg::loadPlugin
    -> dlopen). Static initializers run under the dynamic linker's global lock
    when this library is loaded via dlopen/System.loadLibrary; re-entering the
    loader or creating-and-waiting-on threads there can deadlock, and whether
    it fires depends on link order. Defer this work to an explicit init call or
    a function-local static.
```

## Usage

```bash
vycor-cpp anneal --build-path build --source ... --checks=static-init-hazards
```

Grouped **compute-heavy**: it builds the full cross-TU call graph (shared
with `dead-code` when both are enabled). One diagnostic per root, showing
the shortest hazard chain.

## Extending the hazard list

Organizations can add their own "never during static init" functions —
JNI attach helpers, lock-taking loggers, service registries — to the
built-in dlopen/thread set, either declaratively in the org config:

```json
{ "staticInitHazards": ["myorg::JniEnv::attach", "myorg::Registry::get"] }
```

or in code from `ext/`:

```cpp
VYCOR_EXTENSION_SETUP(MyOrgHazards) {
  registry.addStaticInitHazards({"myorg::JniEnv::attach"});
}
```

## Limitations

- Hazards inside **prebuilt** libraries (a vendored `.a`/`.so` whose
  sources are not in the compilation database) are invisible — but your
  own initializers calling into them are not, and those are usually the
  actionable end of the chain.
- Function pointers and virtual dispatch follow the call graph's
  Plausible-edge semantics; a hazard reached only through an unresolvable
  pointer may be missed.

## Remediation

Move the work behind an explicit `init()` called after load, or a
construct-on-first-use function-local static — anything that runs *after*
`dlopen` returns and the loader lock is released.
