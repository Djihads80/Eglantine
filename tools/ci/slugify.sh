#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: slugify.sh <branch name>
# Returns a short, collision-resistant slug shared by branch release and cleanup jobs.
set -euo pipefail

raw="${1:-}"
slug=$(LC_ALL=C printf '%s' "$raw" |
  tr '[:upper:]' '[:lower:]' |
  sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//' |
  cut -c1-40 |
  sed -E 's/-+$//')
[[ -n "$slug" ]] || slug=branch
suffix=$(printf '%s' "$raw" | sha256sum | cut -c1-8)
printf '%s-%s\n' "$slug" "$suffix"
