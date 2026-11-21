#!/bin/sh
# Point this clone at the tracked .githooks directory.
set -eu
root=$(git rev-parse --show-toplevel)
git -C "$root" config core.hooksPath .githooks
echo "core.hooksPath=.githooks (pre-commit junk check enabled)"
