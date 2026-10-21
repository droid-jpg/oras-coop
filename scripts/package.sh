#!/usr/bin/env bash
# Bundle the built plugin with the installers and relay server, one zip per OS:
#   dist/oras-mmo-<version>-windows.zip   dist/oras-mmo-<version>-mac.zip
# (and the unzipped folders next to them). Run after scripts/build.sh: bash scripts/package.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

plg=plugin/CTRPluginFramework.plgInfo
ver="$(awk '/Major:/{a=$2} /Minor:/{b=$2} /Revision:/{c=$2} END{print a"."b"."c}' "$plg")"

[ -f plugin/oras-mmo.3gx ] || { echo "plugin/oras-mmo.3gx not found; run scripts/build.sh first" >&2; exit 1; }

bundle() {  # bundle <os> <files...>
  local name="oras-mmo-$ver-$1"; shift
  local out="dist/$name"
  rm -rf "$out" "dist/$name.zip"
  mkdir -p "$out/server"
  cp plugin/oras-mmo.3gx "$@" "$out/"
  cp server/server.mjs "$out/server/"
  # zip keeps the executable bit on the .command files, which macOS needs to run them.
  if command -v zip >/dev/null; then
    (cd dist && zip -qr "$name.zip" "$name")
  else
    (cd dist && python3 -m zipfile -c "$name.zip" "$name")
  fi
  ls -l "dist/$name.zip"
}

bundle windows package/install.ps1 package/*.bat package/README.txt
bundle mac package/mac/install.sh package/mac/*.command package/mac/README.txt
