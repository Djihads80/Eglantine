#!/usr/bin/env bash
# usage: changelog-section.sh <name>
# Prints the body of the exact "## [<name>]" or "## [<name>] - <date>" section.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
name="${1:?usage: changelog-section.sh <name>}"

awk -v name="$name" '
  !found && ($0 == "## [" name "]" || index($0, "## [" name "] - ") == 1) {
    found = 1
    next
  }
  found && /^## / { exit }
  found { print }
' CHANGELOG.md
