#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [OPTIONS] --build-path <dir> --rules-json <file> --pattern <regex> [search-dir]

Search for source files matching a regex pattern and run morph on each.

Required:
  --build-path <dir>    Directory containing compile_commands.json
                        (repeatable; first match wins)
  --rules-json <file>   JSON file with transform rules
  --pattern <regex>     grep -E pattern to match in file contents

Optional:
  --dry-run             Preview replacements without applying
  --glob <pattern>      File glob filter (default: "*.cpp")
  --jobs <n>            Max parallel jobs (default: 1)
  search-dir            Directory to search (default: cwd)

Example:
  $(basename "$0") \\
    --build-path ./build/release \\
    --rules-json rules.json \\
    --pattern 'RBX_CHECK\(.*&&' \\
    --glob '*.test.cpp' \\
    --dry-run \\
    Client/
EOF
  exit 1
}

DRILL="$(cd "$(dirname "$0")/.." && pwd)/build/src/vycor-cpp"
BUILD_PATHS=()
RULES_JSON=""
PATTERN=""
GLOB="*.cpp"
DRY_RUN=""
JOBS=1
SEARCH_DIR="."

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-path) BUILD_PATHS+=("$2"); shift 2 ;;
    --rules-json) RULES_JSON="$2"; shift 2 ;;
    --pattern) PATTERN="$2"; shift 2 ;;
    --glob) GLOB="$2"; shift 2 ;;
    --dry-run) DRY_RUN="--dry-run"; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --help|-h) usage ;;
    -*) echo "Unknown option: $1" >&2; usage ;;
    *) SEARCH_DIR="$1"; shift ;;
  esac
done

[[ ${#BUILD_PATHS[@]} -eq 0 ]] && { echo "Error: --build-path required" >&2; usage; }
[[ -z "$RULES_JSON" ]] && { echo "Error: --rules-json required" >&2; usage; }
[[ -z "$PATTERN" ]] && { echo "Error: --pattern required" >&2; usage; }
[[ -x "$DRILL" ]] || { echo "Error: binary not found at $DRILL" >&2; exit 1; }

# Find matching files
FILES=()
while IFS= read -r line; do
  FILES+=("$line")
done < <(grep -rlE "$PATTERN" --include="$GLOB" "$SEARCH_DIR" 2>/dev/null | sort)

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No files matching pattern '$PATTERN' in $SEARCH_DIR"
  exit 0
fi

echo "Found ${#FILES[@]} file(s) matching '$PATTERN'"
[[ -n "$DRY_RUN" ]] && echo "(dry-run mode)"
echo "---"

FAIL=0
for f in "${FILES[@]}"; do
  ABS="$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
  echo ">>> $ABS"
  BP_ARGS=()
  for bp in "${BUILD_PATHS[@]}"; do
    BP_ARGS+=("--build-path=$bp")
  done
  if ! "$DRILL" morph \
    "${BP_ARGS[@]}" \
    --rules-json="$RULES_JSON" \
    --source="$ABS" \
    $DRY_RUN; then
    echo "  FAILED" >&2
    ((FAIL++))
  fi
done

echo "---"
echo "Processed ${#FILES[@]} file(s), $FAIL failure(s)"
exit $FAIL
