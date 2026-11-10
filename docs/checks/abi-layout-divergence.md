# abi-layout-divergence

**Default:** off · **Severity:** error · **Diagnostics:** `AbiLayout_Divergent`

Compares Clang-computed physical layouts of the same externally visible C++
record across translation units with the same target ABI. This catches layout
changes caused by packing/alignment flags or pragmas, including cases where the
record's source text is unchanged.

```cpp
// packet.h — same declaration text in both TUs
struct Packet { char tag; long payload; };

// a.cpp
#pragma pack(push, 1)
#include "packet.h"
#pragma pack(pop)

// b.cpp
#include "packet.h"
```

Both files can compile independently, but code compiled against them disagrees
on the offset of `payload` and the record's size/alignment. Passing pointers or
objects between the TUs can then access the wrong bytes.

```sh
vycor-cpp check --build-path build --source a.cpp --source b.cpp \
  --checks=-all,abi-layout-divergence --threads=1
```

The diagnostic identifies the record, both definition sites and contributing
TUs, the target triple, and the first differing physical-layout fact. There is
one deterministic witness per record/target, rather than one report per pair of
header sightings. The same check selection works with `--isolate-workers`.

## Compared facts and identity

The check records size, alignment, data size, nonvirtual size/alignment, field
count, field offsets and storage sizes, bitfield widths, base/virtual-base
offsets, and own vptr/vbptr layout facts. All sizes/offsets are in bits, respecting
the target's character width. Equal total size does not establish equal layout.

Records are grouped by Clang USR, not printed name. Function-local classes,
anonymous/internal-linkage types, lambdas, incomplete/invalid records, dependent
templates, implicit template instantiations and system-header definitions are
excluded. Named nested records and explicit concrete specializations are eligible.
A finding requires different layouts contributed by two distinct TUs.

Target triple, target data-layout string and C++ ABI kind partition comparisons.
A 32-bit and a 64-bit build of a record are not diagnosed as a cross-TU conflict.
Within one target ABI, packing flags intentionally remain comparable: inconsistent
packing among linked TUs is the condition being checked. Select a coherent link
unit; the tool cannot infer whether two independent executables are ever linked.

## Relationship to ODR checking

`odr-violations` compares definition hashes. This check compares physical layout;
it does not depend on the ODR check being enabled. It does not prove complete ABI
compatibility: calling conventions, vtable slot order, field meaning, ownership,
and arbitrary template ABI behavior are outside its scope. Different field types
with identical physical layouts are not generally diagnosed.

## Freshness and persistence

ABI checking always uses fresh parses. When it is enabled, `--checkpoint` reuse
is disabled with an explanatory message: the legacy check journal validates
main-file stamps but does not yet track header/compile-command changes (issue
[#84](https://github.com/bearbones/vycor-cpp/issues/84)). Reusing such layouts could
hide precisely the flag-driven differences this check is intended to detect.
Other check runs keep their existing checkpoint behavior.

Layout facts survive the isolated-worker shard and merged-index handoff formats.
The shared journal/shard format is version 12; older caches are discarded. No
megascope snapshot-format change is required because these are check facts.

Selected TUs must parse successfully. The current CLI reports partial/failed TUs and exits 3 unless explicitly allowed. No source rewrite is generated.
