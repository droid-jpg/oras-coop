# oras-coop

Co-op mod for **Pokémon Alpha Sapphire v1.4** (Azahar emulator): see your friends in Hoenn.

- `plugin/` — CTRPluginFramework 3GX plugin (C++). Overlay, chat, radar, and an experimental
  "Real models" mode that spawns a trainer for each friend via the game's own spawn code.
- `server/` — Node relay server (no dependencies). `node server/server.mjs [port]`
- `tools/fakefriend.mjs` — test client that joins a room and walks in a circle.
- `scripts/` — WSL toolchain setup and build.
- `re/` — reverse-engineering notes (`NOTES.md`), Azahar GDB memory tools, Ghidra scripts.
- `PROTOCOL.md` — wire format. `STATUS.md` — current state and how to pick the work back up.

No game code, assets or memory dumps are included; you need your own legally obtained copy.

## Build

```bash
bash scripts/wsl-setup.sh   # once, inside WSL Ubuntu
bash scripts/build.sh
```

Copy `plugin/oras-coop.3gx` to `%APPDATA%\Azahar\sdmc\luma\plugins\000400000011C500\` and enable
Azahar's 3GX plugin loader.

## Third-party

`plugin/3gx.ld` and `plugin/Includes/types.h` come from the CTRPluginFramework blank template
(PabloMK7 / ThePixellizerOSS). Links against libctru and libctrpf.
