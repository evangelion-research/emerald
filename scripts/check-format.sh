#!/usr/bin/env bash
# Checks that clang-format is installed and new enough, then formats
# (or, with --check, verifies formatting of) the given C/C++ files
# using this repository's .clang-format rules (or the nearest one
# found by walking up from the file's directory).
#
# Usage:
#   check-format.sh <file.c> [file.h ...]        # format files in place
#   check-format.sh --check <file.c> [file.h ...] # verify only, no writes; exits 1 if not compliant

set -euo pipefail

MIN_MAJOR=20

print_install_help() {
    echo "clang-format was not found (or is too old) on PATH." >&2
    echo "" >&2
    echo "Install it:" >&2
    case "$(uname -s)" in
        Darwin)
            echo "  macOS:   brew install llvm" >&2
            echo "           then add \$(brew --prefix llvm)/bin to PATH" >&2
            ;;
        Linux)
            echo "  Linux:   install the clang-format package from your distribution" >&2
            echo "           (e.g. apt-get install clang-format), or install LLVM from" >&2
            echo "           the official LLVM apt repository for a newer version" >&2
            ;;
        *)
            echo "  Other:   install LLVM from https://releases.llvm.org/ and ensure" >&2
            echo "           clang-format(.exe) is on PATH" >&2
            ;;
    esac
    echo "" >&2
    echo "This repository requires clang-format >= ${MIN_MAJOR}.x." >&2
}

if ! command -v clang-format >/dev/null 2>&1; then
    print_install_help
    exit 1
fi

version_line="$(clang-format --version)"
major="$(echo "$version_line" | grep -oE '[0-9]+' | head -n1)"

if [ -z "$major" ] || [ "$major" -lt "$MIN_MAJOR" ]; then
    echo "Found: $version_line" >&2
    print_install_help
    exit 1
fi

mode="format"
if [ "${1:-}" = "--check" ]; then
    mode="check"
    shift
fi

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 [--check] <file.c|file.h> [...]" >&2
    exit 1
fi

if [ "$mode" = "check" ]; then
    clang-format --dry-run --Werror "$@"
else
    clang-format -i "$@"
    echo "Formatted with clang-format $major: $*"
fi
