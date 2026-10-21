#!/usr/bin/env bash
# Build oras-mmo.3gx inside WSL. Run: bash /mnt/e/oras-mmo/scripts/build.sh
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

cd "$(dirname "$0")/../plugin"
make "$@"
ls -l oras-mmo.3gx
