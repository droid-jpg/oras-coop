#!/usr/bin/env bash
# Build oras-coop.3gx inside WSL. Run: bash /mnt/e/oras-coop/scripts/build.sh
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

cd "$(dirname "$0")/../plugin"
make "$@"
ls -l oras-coop.3gx
