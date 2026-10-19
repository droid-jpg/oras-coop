# oras-coop

Co-op mod for **Pokémon Alpha Sapphire v1.4** (Azahar emulator): see your friends in Hoenn.

- `plugin/` — CTRPluginFramework 3GX plugin (C++). Overlay, chat, radar, and an experimental
  "Real models" mode that spawns a trainer for each friend via the game's own spawn code.
- `server/` — Node relay server (no dependencies). `node server/server.mjs [port]`
- `tools/fakefriend.mjs` — test client that joins a room and walks in a circle.
- `scripts/` — WSL toolchain setup, build and packaging.
- `package/` — installers shipped in the zips: Windows (`install.ps1` + `.bat`) and `mac/` (`install.sh` + `.command`).
- `re/` — reverse-engineering notes (`NOTES.md`), Azahar GDB memory tools, Ghidra scripts.
- `PROTOCOL.md` — wire format. `STATUS.md` — current state and how to pick the work back up.

No game code, assets or memory dumps are included; you need your own legally obtained copy.

## Install (players)

Download `oras-coop-<version>-windows.zip` or `-mac.zip` from the repo's Releases page (or the latest **Package** workflow
run under Actions), unzip it, close Azahar and double-click **Install oras-coop.bat** (Mac: **Install oras-coop.command**). It copies the
plugin, asks for your name / server / room, and turns on Azahar's 3GX plugin loader. One player
hosts with **Start server** (Node.js 18+). Details in `package/README.txt` and `package/mac/README.txt`.

## Build

```bash
bash scripts/wsl-setup.sh   # once, inside WSL Ubuntu
bash scripts/build.sh
bash scripts/package.sh     # optional: dist/oras-coop-<version>.zip with the installer
```

GitHub Actions (`.github/workflows/package.yml`) builds the same zip on every push; pushing a
`v*` tag attaches it to a release.

Copy `plugin/oras-coop.3gx` to `%APPDATA%\Azahar\sdmc\luma\plugins\000400000011C500\` and enable
Azahar's 3GX plugin loader.

## Third-party

`plugin/3gx.ld` and `plugin/Includes/types.h` come from the CTRPluginFramework blank template
(PabloMK7 / ThePixellizerOSS). Links against libctru and libctrpf.
