# Writing C++ rewrites with morph

`vycor-cpp morph` takes a JSON rule file, parses C++ with the flags you supply,
and replaces source ranges selected by Clang AST matchers. A rule can match a
call to a particular function, bind one of its arguments, and reuse the
argument's original spelling in the replacement. Rule authors can edit or
generate JSON and run it with the same binary.

## Try the macro example

Use the same checkout for the binary and the example. With CMake, Ninja, and
supported LLVM/Clang development packages installed, build from the repository
root. This configuration also builds on machines without Catch2 or libmegascope.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVYCOR_BUILD_TESTS=OFF
cmake --build build --target vycor-cpp
```

The sample is a self-contained C++17 translation unit. Copy it to a temporary
directory so you can compare the result with the checked-in expected output.
[Toolchain and build options](building.md#build-from-source)

```sh
work=$(mktemp -d)
cp examples/macro_split/input.cpp "$work/input.cpp"
rules=examples/macro_split/rules.json

build/src/vycor-cpp morph --rules-json "$rules" --source "$work/input.cpp" \
  --dry-run -- -std=c++17

build/src/vycor-cpp morph --rules-json "$rules" --source "$work/input.cpp" \
  -- -std=c++17
diff -u examples/macro_split/expected.cpp "$work/input.cpp"
```

The preview reports three locations. At the three-operand call it prints
`replace with 'ASSERT_ALL(a > 0, b > 0, c > 0)'`. The two-operand rule emits
two assertions inside a single `do ... while (0)` statement. The nested
conjunction in `ASSERT(a == 0 || (a > 0 && b > 0))` stays intact. The final
`diff` exits successfully with empty output.

The sample's `ASSERT_ALL` evaluates function arguments eagerly. Its operands
are independent integer comparisons. A migration of expressions such as
`p && *p` needs a destination that preserves short-circuit evaluation.
[Input](../examples/macro_split/input.cpp) · [Rules](../examples/macro_split/rules.json) · [Expected output](../examples/macro_split/expected.cpp)

## Write a first rule

Suppose a project has both `legacy::emit(int)` and `modern::emit(int)`.
This rule changes a direct call such as `legacy::emit(count + 1)` to
`modern::emit(count + 1)`. Declarations and calls to other functions stay intact.

```json
{
  "passes": [{
    "context": "isExpansionInMainFile()",
    "rules": [{
      "find": {
        "matcher": "callExpr(callee(functionDecl(hasName(\"::legacy::emit\"))), argumentCountIs(1), hasArgument(0, expr().bind(\"arg\"))).bind(\"call\")"
      },
      "replace": { "target": "call", "with": "modern::emit({{arg}})" }
    }]
  }]
}
```

`callExpr` selects a call, `callee` checks the resolved declaration, and
`.bind("arg")` gives the argument a name. The target is the range to overwrite.
The template supplies its new contents. This matcher checks the qualified
function name and argument count. If the old name has several one-argument
overloads, add a parameter type constraint to select the intended overload.
Use `clang-query` with the project's compilation database to try a matcher
and inspect the bindings. [Clang AST matcher reference](https://clang.llvm.org/docs/LibASTMatchersReference.html)

## Migrating field assignments to a builder API

A `statement-subset` rule can turn field-by-field initialization into one
builder expression. Write one case per field. At each declaration, morph
emits just the cases it matches, in the order the assignments appear.

```cpp
Options cfg{};
cfg.verbose = true;
cfg.port = base + 1;
```

With the example rule, those three statements become one declaration.

```cpp
Options cfg = OptionsBuilder{}.verbose(true).port(base + 1).build();
```

At a site that sets only `timeout`, the same rule produces
`Options cfg = OptionsBuilder{}.timeout(10).build();`. A declaration with
zero matching writes stays as written. The example builder uses the same
field defaults as the struct. For a real migration, check that the setters,
construction, and `build()` preserve the behavior of the original code.

Run the example from the repository root.

```sh
work=$(mktemp -d)
cp examples/struct_to_builder/input.cpp "$work/config.cpp"
rules=examples/struct_to_builder/rules.json
build/src/vycor-cpp morph --rules-json "$rules" --source "$work/config.cpp" --dry-run -- -std=c++17
build/src/vycor-cpp morph --rules-json "$rules" --source "$work/config.cpp" -- -std=c++17
diff -u examples/struct_to_builder/expected.cpp "$work/config.cpp"
c++ -std=c++17 "$work/config.cpp" -o "$work/config" && "$work/config"
```

[Complete JSON rule](../examples/struct_to_builder/rules.json) · [Input](../examples/struct_to_builder/input.cpp) · [Expected output](../examples/struct_to_builder/expected.cpp)

### Writing a statement-subset rule

Put `statement-subset` in a pass's `rules` array. The rule accepts the following fields.

| Field | Meaning |
|---|---|
| `anchor` | AST matcher for the first direct child of a compound statement. The pass's `context` applies to the enclosing compound statement. |
| `cases` | Nonempty array of objects with a statement `matcher` and an `emit` template. Morph tries cases in listed order for each subsequent sibling. The first matching case supplies that statement's bindings to its template. |
| `prefix`, `suffix` | Templates evaluated with the anchor's bindings. Output is the prefix, all emitted fragments in source order, and the suffix. Include the required punctuation and final semicolon. |
| `fresh-local` | Optional anchor binding naming the single declared variable. Preserves the declaration through the variable name and replaces its initialization and matched writes. The example prefix starts with ` = `. |

Morph consumes consecutive expression or declaration statements within one
block. Repeated cases emit repeatedly. The first unmatched statement ends the
run. An anchor needs at least one follower, and each participating matcher must
give one unambiguous binding set. Comments or preprocessing directives in the
consumed spelling or between statements end the run. A later anchor can start
another run. With `fresh-local` omitted, the replacement includes the entire
anchor, so design the matchers to express the required operand relationships.

For a C++17 or later builder migration, bind an automatic local record declared
alone with default initialization or empty braces. The guard accepts consecutive
direct member assignments on that same variable. An intervening call such as
`observe(cfg)`, a write to another object, or a right-hand side that reads
`cfg` ends the run. A safe prefix can still become a chain. The guard also skips
writes requiring expression cleanups, such as a temporary with a nontrivial
destructor, and implicit template instantiations cannot trigger a rewrite of
the shared template source.

### Rewriting each assignment in place

When the existing receiver already provides equivalent setters, an ordinary
single-node rule can change each assignment in place.

For example, save this translation unit as `config.cpp`.

```cpp
struct Options {
  int port = 0;
  bool verbose = false;
  Options& setPort(int);
};
void observe(const Options&);

void configure(Options& cfg, int base) {
  cfg.port = base + 1;
  observe(cfg);
}
```

Save this rule as `setters.json` and run
`vycor-cpp morph --rules-json setters.json --source config.cpp --dry-run -- -std=c++17`.

```json
{
  "passes": [{
    "context": "isExpansionInMainFile()",
    "rules": [{
      "find": {
        "matcher": "binaryOperator(hasOperatorName(\"=\"), hasParent(compoundStmt()), hasLHS(memberExpr(unless(isArrow()), member(fieldDecl(hasName(\"::Options::port\"))), hasObjectExpression(declRefExpr().bind(\"object\")))), hasRHS(expr().bind(\"value\"))).bind(\"assign\")"
      },
      "replace": { "target": "assign", "with": "{{object}}.setPort({{value}})" }
    }]
  }]
}
```

The preview shows `cfg.setPort(base + 1)` at the assignment. Its semicolon
stays in place, and `observe(cfg)` sees the write at the same point. The
matcher selects a standalone built-in assignment to the declared
`Options::port` field on a named object. To cover `verbose` when the API
also has `setVerbose(bool)`, add a second rule with that field and setter.
An omitted field produces no setter call.

The older example illustrates the reverse migration, from setter calls to
struct assignments. It contains a hand-written before/after pair.
[Builder-to-struct fixture](../examples/builder_to_struct)

## Run on a project

```sh
build/src/vycor-cpp morph --rules-json migrate.json --build-path build \
  --source src/reader.cpp --source src/writer.cpp --dry-run
```

Point `--build-path` at the directory containing `compile_commands.json`.
Repeat it to try databases in order. For standalone files, put compiler flags
after `--`. An empty `--` uses the compiler defaults. When supplying both
forms, fixed flags serve as the fallback. Pass one `--source` for each
translation unit. Includes are part of its AST and can also receive edits.
`isExpansionInMainFile()` restricts the **matched root** to the main file's
expansion locations. A replacement targeting another binding can select a
different file.

The batch helper prefilters files by a content regular expression and runs
them in one invocation, so shared headers participate in the same pass.
Set `VYCOR_CPP` to use a binary outside the default build directory.
[Batch helper and options](../scripts/morph-batch.sh)

## Rule fields and templates

Morph rejects unknown JSON fields and macro filters used with `node` scope.
A misspelled or misplaced filter produces a diagnostic with the field path.

| Field | Meaning |
|---|---|
| `passes` | Ordered array. Each pass contains a `rules` array and may have a descriptive `name`. |
| `context` | Optional predicate on each rule's root. To constrain an enclosing declaration use an explicit matcher such as `hasAncestor(...)`. |
| `find.matcher` | A Clang dynamic matcher expression. Bind nodes inline with `.bind("id")`. |
| `replace.target` | Required binding for the range to edit. |
| `replace.with` | Required string. Literal text is copied verbatim. An empty string deletes the selected range. |
| `replace.scope` | Defaults to `node`, the bound node's token range. `macro-expansion` selects the enclosing invocation as written by the caller. |
| `replace.macro-names` | Optional list of call-site macro names for `macro-expansion`. An empty or omitted list accepts any name. |
| `replace.arg-only` | Defaults to `true`, requiring the target's first token to come from a macro argument. Set to `false` to also accept a target beginning in the macro body. |

For `macro-expansion`, a target spelled entirely inside the invocation must
span the argument text between its outer parentheses. This keeps a match on a
nested subexpression or just one of several arguments from replacing the whole
invocation. A structural expression supplied by a macro body can have tokens
spelled outside the invocation. Filter by `macro-names` when matching those
expressions, especially with forwarding wrappers.

| Template expression | Result |
|---|---|
| `{{x}}` or `{{x \| text}}` | Source spelling of the bound node. |
| `{{x \| name}}` | Qualified name of a named declaration. |
| `{{x \| flatten("&&") \| join(", ")}}` | Leaf spellings of a built-in binary operator chain, in source order, separated by the given string. Parenthesized subexpressions, overloaded operators, and other operators remain whole. |
| `{{x \| flatten("&&")}}` | The same leaves concatenated with an empty separator. |

Use double quotes for pipe arguments and escape them inside JSON strings.
A template generates source text, including any parentheses, separators,
whitespace, or semicolons needed by the replacement. A statement's trailing
semicolon often lies outside its AST node range.

## Passes, previews, and failures

Rules in a pass see the same AST. Identical edits from repeated matches or
shared headers coalesce. An order-dependent overlap within a pass is an error.
Use separate passes when a later rule should match code created by an earlier
rule. Each pass parses the selected translation units once per compile command.
Group independent rules in one pass to share that parse.

Preview and apply run the same passes with intermediate contents in memory.
Preview prints the composed edits at their original 1-based line and byte
column. A preview with zero matches succeeds and prints no replacements.
Apply writes after matching and composing all passes. A matcher, template,
conflict, or Clang parse error gives a nonzero exit and discards staged edits.
Writing is per file. A filesystem failure during publication can leave a
partially written change. Build and test the result of the last pass to check
that the generated code compiles and preserves the behavior you need.

## For contributors

The CLI calls `parseRulesFile`, `buildPipeline`, and
`TransformPipeline::execute`. To add a JSON field, update the schema structs
and deserialization in `RulesParser`. Add single-node text operations in `TemplateEngine` and sibling matching or
range policies in `StatementSubset`. `MatcherEngine` registers dynamic matchers,
collects compatible edits, and supplies staged source to Clang on later
passes. `TransformPipeline` composes the edits in original-file coordinates.
[Public headers](../include/vycor/morph) · [Implementations](../src/morph)

For programmatic transforms, construct a `vycor::TransformRule` with a matcher
string, an optional root `bindId`, and a callback taking a
`MatchFinder::MatchResult`. Return a vector of `clang::tooling::Replacement`
objects. Call `pipeline.addPass({rule})`, then
`pipeline.execute(buildPaths, sourceFiles, /*dryRun=*/true)`.
After success, `getReplacements()` holds that run's combined edits. Single-node JSON rules target one binding. Statement-subset rules replace a
run of siblings. A C++ callback can emit several edits from one match.

To run the tests from the minimal build above, enable Catch2 and reconfigure.

```sh
git submodule update --init extern/Catch2
cmake -S . -B build -DVYCOR_BUILD_TESTS=ON
cmake --build build --target vycor_tests
build/tests/vycor_tests '[transforms],[rules],[template],[parse],[addRule]'
```

For each rule, check an intended match and a nearby construct that should
survive unchanged. For macro rules, include a nested expression and a
different macro with a similar expansion. The integration tests exercise the
published JSON and compare transformed bytes with the expected file.
[Pipeline tests](../tests/test_transforms.cpp) · [Rule and template tests](../tests/test_rules_parser.cpp)

### Why use Clang's matcher and replacement APIs directly?

The original project README describes an external program, such as a Python DSL
translator, generating rule specifications. The first implementation already
parsed matcher strings dynamically. The later JSON implementation coupled
those runtime bindings to templates and added call-site macro selection.
[Original README](https://github.com/bearbones/vycor-cpp/blob/bce648b91935318c1b591f500a55beb077b169e6/README.md) · [JSON rules implementation](https://github.com/bearbones/vycor-cpp/commit/1d0f80cef33528cb4a92be2c8cf569e48c837214)

Clang Transformer supplies C++ builders for rules, range selectors, and
replacement stencils. It could serve as an execution backend. A morph frontend
would still have to interpret the JSON, resolve bindings, and implement policies
such as flattening operands and selecting the outer macro invocation. The
history suggests that the direct LibTooling path already matched
the desired authoring workflow, and routing the same rules through Transformer
would have added an adapter. The cited commits record that workflow but give
no explicit rejection of Transformer. [Clang Transformer tutorial](https://clang.llvm.org/docs/ClangTransformerTutorial.html)
