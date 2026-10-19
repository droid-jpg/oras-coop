#!/usr/bin/env bash
# Bundle the built plugin with the Windows installer and relay server:
#   dist/oras-coop-<version>/  and  dist/oras-coop-<version>.zip
# Run after scripts/build.sh: bash scripts/package.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

plg=plugin/CTRPluginFramework.plgInfo
ver="$(awk '/Major:/{a=$2} /Minor:/{b=$2} /Revision:/{c=$2} END{print a"."b"."c}' "$plg")"
name="oras-coop-$ver"
out="dist/$name"

[ -f plugin/oras-coop.3gx ] || { echo "plugin/oras-coop.3gx not found; run scripts/build.sh first" >&2; exit 1; }

rm -rf "$out" "dist/$name.zip"
mkdir -p "$out/server"
cp plugin/oras-coop.3gx package/install.ps1 package/*.bat package/README.txt "$out/"
cp server/server.mjs "$out/server/"

if command -v zip >/dev/null; then
  (cd dist && zip -qr "$name.zip" "$name")
else
  (cd dist && python3 -m zipfile -c "$name.zip" "$name")
fi
ls -l "dist/$name.zip"
