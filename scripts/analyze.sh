#!/bin/bash

# Run all static analysis tools against the meteor codebase.
#
# Usage: analyze.sh
#
# Prerequisites:
#   apt install cppcheck clang-tidy-20 flawfinder
#
# Requires compile_commands.json at the project root (symlink from build dir).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."

cd "$PROJECT_DIR"

if [ ! -f compile_commands.json ]; then
    echo "Error: compile_commands.json not found at project root."
    echo "Run cmake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON and symlink:"
    echo "  ln -sf build-t31/compile_commands.json compile_commands.json"
    exit 1
fi

FAIL=0

echo "=== cppcheck ==="
if ! cppcheck --enable=warning,performance,portability \
    --suppress=unusedFunction \
    --suppress=missingIncludeSystem \
    --project=compile_commands.json \
    --error-exitcode=1 \
    --quiet 2>&1; then
    FAIL=1
fi

echo ""
echo "=== clang-tidy ==="
if ! clang-tidy-20 -p compile_commands.json src/*.c 2>&1 \
    | grep -E "^.*\.(c|h):[0-9]+:[0-9]+: (warning|error):" \
    | grep -v "prudynt-t-"; then
    echo "  No warnings."
else
    FAIL=1
fi

echo ""
echo "=== flawfinder ==="
if ! flawfinder --minlevel=2 --error-level=2 --quiet src/ include/ 2>&1; then
    FAIL=1
fi

echo ""
if [ $FAIL -ne 0 ]; then
    echo "=== FAILED: static analysis found issues ==="
    exit 1
else
    echo "=== PASSED: all static analysis checks clean ==="
fi
