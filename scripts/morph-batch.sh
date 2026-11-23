#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [OPTIONS] --build-path <dir> --rules-json <file> --pattern <regex> [search-dir]

Search for source files matching a regex pattern and run one morph pipeline over the matching files.

Required:
  --build-path <dir>    Directory containing compile_commands.json
                        (repeatable; first match wins)
  --rules-json <file>   JSON file with transform rules
  --pattern <regex>     grep -E pattern to match in file contents

Optional:
  --dry-run             Preview replacements without applying
  --glob <pattern>      File glob filter (default: "*.cpp")
  --jobs 1              Accepted for compatibility; morph processes TUs serially
  search-dir            Directory to search (default: cwd)

Example:
  $(basename "$0") \\
    --build-path ./build/release \\
    --rules-json examples/macro_split/rules.json \\
    --pattern 'ASSERT\(.*&&' \\
    --glob '*.test.cpp' \\
    --dry-run \\
    tests/
EOF
  exit "${1:-1}"
}

VYCOR_CPP="${VYCOR_CPP:-$(cd "$(dirname "$0")/.." && pwd)/build/src/vycor-cpp}"
BUILD_PATHS=()
RULES_JSON=""
PATTERN=""
GLOB="*.cpp"
DRY_RUN=0
JOBS=1
SEARCH_DIR="."

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-path) BUILD_PATHS+=("$2"); shift 2 ;;
    --rules-json) RULES_JSON="$2"; shift 2 ;;
    --pattern) PATTERN="$2"; shift 2 ;;
    --glob) GLOB="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --help|-h) usage 0 ;;
    -*) echo "Unknown option: $1" >&2; usage ;;
    *) SEARCH_DIR="$1"; shift ;;
  esac
done

[[ ${#BUILD_PATHS[@]} -eq 0 ]] && { echo "Error: --build-path required" >&2; usage; }
[[ -z "$RULES_JSON" ]] && { echo "Error: --rules-json required" >&2; usage; }
[[ -z "$PATTERN" ]] && { echo "Error: --pattern required" >&2; usage; }
[[ -x "$VYCOR_CPP" ]] || { echo "Error: binary not found at $VYCOR_CPP" >&2; exit 1; }
[[ "$JOBS" == 1 ]] || { echo "Error: only --jobs 1 is supported" >&2; exit 1; }

# Preserve names containing whitespace or newlines, and distinguish grep's
# no-match status (1) from an unreadable input or invalid pattern (2).
work_dir="$(mktemp -d)"
trap 'rm -f "$work_dir/matches" "$work_dir/sources.rsp"; rmdir "$work_dir"' EXIT
matches="$work_dir/matches"
status=0
grep -rlE --null --include="$GLOB" -- "$PATTERN" "$SEARCH_DIR" > "$matches" || status=$?
if (( status != 0 )); then
  if [[ $status -eq 1 ]]; then
    echo "No files matching pattern '$PATTERN' in $SEARCH_DIR"
    exit 0
  fi
  exit "$status"
fi

args=(morph "--rules-json=$RULES_JSON")
for path in "${BUILD_PATHS[@]}"; do
  args+=("--build-path=$path")
done
# LLVM reads a quoted argument per line from the response file. This keeps
# even a project-wide selection below the OS command-line length limit.
while IFS= read -r -d '' file; do
  file=${file//\\/\\\\}
  file=${file//\"/\\\"}
  printf '"--source=%s"\n' "$file"
done < "$matches" > "$work_dir/sources.rsp"
if (( DRY_RUN )); then
  args+=(--dry-run)
fi
"$VYCOR_CPP" "${args[@]}" "@$work_dir/sources.rsp"
