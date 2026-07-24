# adl-visibility

**Default:** on · **Groups:** — · **Diagnostics:** `ADL_Fallback`, `ADL_Ambiguity`, `ADL_SameScore` (opt-in)

Flags call sites whose argument-dependent lookup would resolve differently
— or become ambiguous — if an overload that exists elsewhere in the
project were visible in this translation unit.

## Why no other tool catches this

Overload resolution happens per TU against whatever declarations the
included headers provide. A compiler can only rank the candidates it can
see; it cannot know that `MathLib::scale(Vector, double)` exists in a
header this TU didn't include and would have won. The program compiles,
links, and calls the wrong overload — silently, and differently per TU
depending on include order.

anneal's phase 1 indexes every overload in the project; phase 2 re-ranks
each ADL call site against the *global* candidate set:

- **`ADL_Fallback`** — an invisible overload Pareto-dominates the one the
  compiler chose (it would win if visible).
- **`ADL_Ambiguity`** — including the missing header would make the call
  ambiguous.
- **`ADL_SameScore`** (needs `--warn-same-score`) — an invisible candidate
  ties the resolved one on every argument: inclusion order silently
  decides. Noisier signal, hence opt-in.

## Example

```
src/logic.cpp:42:5: Fragile ADL resolution: MathLib::scale(Vector, double) exists in
    Extension.hpp but is not visible here. The current call resolves to
    MathLib::scale(Vector, int). Include Extension.hpp or explicitly qualify the call.
```

## Options

- `--warn-same-score` — also emit `ADL_SameScore` diagnostics.
- `--model-convertibility` — consult the indexed type-relation model
  (inheritance, converting constructors, conversion operators) when
  judging whether an invisible overload is viable, instead of the stricter
  arithmetic-or-exact heuristic.

## Remediation

Include the header that declares the better overload, or qualify the call
to pin the intended function.
