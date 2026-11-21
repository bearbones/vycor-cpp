# redundant-instantiation

**Unit:** statements · **Native level:** symbol · **Roll-ups:** template, file, tu, directory, target · **Facts:** the index's emission facts

The same function body compiled again and again. For every inline
function, implicit template instantiation, implicitly defined member
and header-defined `static` function that two or more TUs emit, how
many TUs emit it and how big it is. The linker keeps one copy of a
vague-linkage function and throws the others away; the compiler
generated, optimized and wrote every one of them first.

## Why measure it

A template instantiated in a header is compiled once per TU that uses
it. `std::vector<Widget>::push_back` used by 300 TUs is 300 copies of
the same code generation, optimization and object-file bytes, and 299
of them are discarded at link time. Nothing in a build log shows it:
each TU's compile is a little slower, and no single change looks
responsible.

The fixes are known — an `extern template` declaration in the header
with one explicit instantiation in a source file, moving a large body
out of line, a non-template base for the code that does not depend on
the template parameter — and each one is work. The metric says where
that work pays: the bodies whose copies cost the most, summed over the
TUs that make them.

## Usage

```bash
# The index records emission facts on every bake.
vycor-cpp megascope index --build-path build --source-re 'src/.*\.cpp$'
vycor-cpp metrics --metrics=redundant-instantiation --build-path build

# Per template: every instantiation of one pattern together.
vycor-cpp metrics --metrics=redundant-instantiation --rollup template \
    --build-path build

# Which headers, and which link targets, pay most.
vycor-cpp metrics --metrics=redundant-instantiation --rollup file --build-path build
vycor-cpp metrics --metrics=redundant-instantiation --rollup target \
    --target-map build/targets.json --build-path build
```

An index whose references section has no emission facts (one baked
before format v20, or with reference facts off) answers `unavailable`
(exit 3), never zero.

## What is measured

One record per **linkage name** (the mangled name the object file
holds, so each instantiation is a record of its own) that two or more
TUs emit. An internal function's name is not unique (two headers'
`static helper()` mangle alike), so its record is the name at one
definition: copies of one header's function are one record, another
header's namesake is another. A TU *emits* a function when its code generation hands the
body to the optimizer: the definition the emission facts record
(docs/symbol-references.md, "Emission facts"), which follows Clang's
CodeGen from what the TU must emit through every body that emitted
code references. A function no emitted code reaches (an inline helper
nothing calls, a body only a `constexpr` evaluation or an unevaluated
operand uses, the dead branch of `if constexpr`) is not emitted, and is
not counted however many TUs include it.

| Counted | Not counted |
|---|---|
| implicit instantiations of function templates and of members of class templates | explicit instantiation definitions (one strong copy, where they are written) |
| inline functions, including member functions defined in the class | an instantiation an `extern template` declaration covers: the TU keeps the body only to inline it (`available_externally`), and emits no copy of its own |
| implicitly defined special members (constructors, destructors, assignments) of classes written in a header | anything declared in a system header (the standard library's templates instantiated for your types *are* counted: they are written in your code's terms only through the instantiation, which belongs to the system header — see Limitations) |
| `static` and anonymous-namespace functions written in a header: each TU emits a private copy, which the linker cannot merge | functions emitted by one TU only |
| | trivial special members (a defaulted copy that is a byte copy, an empty default constructor): CodeGen emits no body for them |

`value` is **copies × statements**: the number of TUs that emit the
function times its size. `statements` is the number of statements and
expressions code generation lowers for the body: every node of the
body's syntax tree the emission walk visits, implicit ones
(conversions, temporaries, default arguments, member initializers,
destructor calls) included, the code a folded condition discards
excluded. It is a proxy for the size of the emitted code, chosen
because the walk that decides emission computes it for free; see
"Reading the numbers" for how well it tracks object bytes.

### Components (symbol level)

| Component | Meaning |
|---|---|
| `copies` | Number of TUs that emit the function. |
| `statements` | Size of one copy. |
| `internal` | 1 for a `static` or anonymous-namespace function written in a header (one private copy per TU, never merged by the linker), else 0. |

The entity is the function: `kind` `function`, `usr` its mangled name,
`name` the demangled one, and `file` / `line` its definition (for an
instantiation, the pattern's).

### Roll-ups

A group's value is the **sum** of its members' values, as are
`copies` and `statements`; `entities` counts the members.

| Level | A record is |
|---|---|
| `template` | one template pattern (or one non-template function) with every instantiation of it: `std::vector<int>::push_back` and `std::vector<Widget>::push_back` are one row. `kind` `template`, `usr` the pattern's USR, `name` its qualified name. |
| `file` | the file the definition is written in: which headers cost most |
| `tu` | a TU, counting every redundant body it emits |
| `directory` | the directory of each emitting TU |
| `target` | the link targets that list each emitting TU (`--target-map`) |

At `tu`, `directory` and `target` a record counts in the row of every
TU that emits it, so the rows do not sum to the total.

## Reading the numbers

The value ranks; it is not a time or a byte count. A body of 400
statements emitted by 50 TUs (20,000) is a better `extern template` or
out-of-line candidate than one of 10 statements emitted by 900 (9,000),
even though the second is in more TUs, because code generation and
optimization scale with the code, not with the number of functions.

How well `statements` tracks the code, measured on 60 TUs of LLVM's
`lib/Support` compiled by Clang 20 against the emission facts of the
same TUs: over the 3,390 recorded bodies found in the `-O0` objects,
the Spearman rank correlation of `statements` with the function's size
in the object is 0.82; at `-O3`, over the 369 that keep an out-of-line
copy, 0.73. Over the 666 bodies two or more of those TUs emit, the
metric's value ranks with the summed `-O0` bytes of every copy at 0.85.
At `-O3` most of those copies are inlined into their callers and
deleted, so the bytes that reach the object no longer follow the value
(0.10): what the value ranks is the code each TU generates and
optimizes, not what survives into the object.

Small functions every TU calls rank on copies alone:
`StringRef(const char *)`, 14 statements in 595 of LLVM's 938 library
TUs, sits among the top fifteen bodies. Their copies do cost code
generation, but there is little to move out of line; the actionable
rows are the large bodies (a cold error path, an allocator's slow path,
a big `switch`) and the single instantiations many TUs repeat. Every
implicit node counts, so straight-line arithmetic scores higher than
its machine code: `NextPowerOf2`, six shifts and ors, is 49 statements.

Use the template roll-up first. An `extern template` declaration
covers one instantiation; moving a body out of line covers them all.
Then open the symbol rows of the top templates to see which
instantiations carry the cost.

## Limitations

- **Emission is decided per TU, before optimization.** A body the
  optimizer inlines into every caller and then deletes still cost its
  code generation and optimization; it is counted. Whether its copy
  reaches the object file is not what is measured.
- **The emission walk is CodeGen's, approximately.** It follows what
  Clang emits on its own for the TU (strong definitions, global
  variables `DeclMustBeEmitted` keeps, vtables the TU homes, explicit
  instantiation definitions) through every function a body or a
  constant initializer references; a virtual call reaches its target
  only where CodeGen devirtualizes it; a key-less vtable is emitted
  where a constructor or a vptr-storing destructor is. What it does not
  model: a call CodeGen folds that Sema left as a call (a
  `__builtin_constant_p` branch, for instance), the deferred emission
  of a function only another deferred function's discarded code uses,
  `-fno-inline-functions` style flags that change linkage, and
  target-specific emission (multiversioned functions). On the same 60
  LLVM TUs at `-O0`, 99.0% of the bodies recorded for a TU are defined
  in its object, and 96.5% of the object's vague-linkage LLVM functions
  are recorded; most of the rest are explicit instantiation
  definitions, which are strong and deliberately not recorded, and
  virtual members of class templates reached only through a vtable
  the walk does not see emitted.
- **System headers are out.** An instantiation of a system template
  (`std::vector<Widget>`) is attributed to the system header, which the
  facts exclude, so the cost of the standard library's templates
  instantiated for your types is not reported. The project's own code
  inside them (a `Widget` constructor `emplace_back` calls) is.
- **One build configuration.** The facts are those of the compile
  commands baked. A body emitted only under a macro another
  configuration defines is not seen.
- **Records with one name and two bodies.** Two TUs whose copies of one
  linkage name differ (an ODR violation, or a body that a macro
  changes) are one record with the larger size.
- **Coverage.** A TU that did not parse is not counted. `indexScope` on
  the payload says how many requested TUs the measurement covers.
