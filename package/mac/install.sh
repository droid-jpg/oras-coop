#!/bin/bash
# oras-mmo installer for Azahar (macOS). Run through "Install oras-mmo.command".
#   --dir <path>   Azahar user folder (default ~/Library/Application Support/Azahar)
#   --uninstall    remove the plugin (keeps oras-mmo.txt and Azahar's settings)
# Written for the bash 3.2 that ships with macOS.
set -euo pipefail

TITLE_ID=000400000011C500   # Pokemon Alpha Sapphire
HERE="$(cd "$(dirname "$0")" && pwd)"
AZAHAR_DIR=""
UNINSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) AZAHAR_DIR="$2"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

ask() {  # ask "Prompt" default -> echoes the answer
  local v
  read -r -p "$1 [$2]: " v || true
  if [ -z "${v// /}" ]; then echo "$2"; else echo "$v"; fi
}

# The frames carry plain ASCII: name[16] and room[12], NUL-padded.
clean() {
  printf '%s' "$1" | LC_ALL=C tr -cd '\040-\176' | sed 's/^ *//; s/ *$//' | cut -c "1-$2"
}

# --- Find Azahar's user folder ----------------------------------------------------------------
if [ -z "$AZAHAR_DIR" ]; then
  AZAHAR_DIR="$HOME/Library/Application Support/Azahar"
  if [ ! -d "$AZAHAR_DIR" ]; then
    echo "Azahar's folder wasn't found at $AZAHAR_DIR."
    echo "Start Azahar once (so it creates it), or drag Azahar's user folder into this window."
    read -r -p "Azahar user folder: " AZAHAR_DIR
  fi
fi
AZAHAR_DIR="$(printf '%s' "$AZAHAR_DIR" | sed "s/^[ '\"]*//; s/[ '\"]*$//; s/\\\\ / /g")"
mkdir -p "$AZAHAR_DIR"

PLUGIN_DIR="$AZAHAR_DIR/sdmc/luma/plugins/$TITLE_ID"
PLUGIN="$PLUGIN_DIR/oras-mmo.3gx"
CONFIG_TXT="$PLUGIN_DIR/oras-mmo.txt"
QT_CONFIG="$AZAHAR_DIR/config/qt-config.ini"
OLD_PLUGIN="$PLUGIN_DIR/oras-coop.3gx"   # the mod's name before it became oras-mmo

# Azahar rewrites qt-config.ini on exit, which would undo the plugin-loader switch.
while pgrep -i azahar >/dev/null 2>&1; do
  read -r -p "Azahar is running. Quit it (Cmd+Q), then press Return " _ || true
done

if [ "$UNINSTALL" = 1 ]; then
  [ -f "$OLD_PLUGIN" ] && rm "$OLD_PLUGIN" && echo "Removed $OLD_PLUGIN"
  if [ -f "$PLUGIN" ]; then rm "$PLUGIN"; echo "Removed $PLUGIN"; else echo "oras-mmo is not installed."; fi
  exit 0
fi

# --- Copy the plugin --------------------------------------------------------------------------
[ -f "$HERE/oras-mmo.3gx" ] || { echo "oras-mmo.3gx is missing next to the installer ($HERE)." >&2; exit 1; }
mkdir -p "$PLUGIN_DIR"
# Replace an install from before the rename, keeping its settings and model cache.
if [ -f "$OLD_PLUGIN" ]; then rm "$OLD_PLUGIN"; echo "Removed the old $OLD_PLUGIN"; fi
if [ -f "$PLUGIN_DIR/oras-coop.txt" ] && [ ! -f "$CONFIG_TXT" ]; then
  mv "$PLUGIN_DIR/oras-coop.txt" "$CONFIG_TXT"
fi
if [ -f "$PLUGIN_DIR/oras-coop-models.bin" ] && [ ! -f "$PLUGIN_DIR/oras-mmo-models.bin" ]; then
  mv "$PLUGIN_DIR/oras-coop-models.bin" "$PLUGIN_DIR/oras-mmo-models.bin"
fi
cp "$HERE/oras-mmo.3gx" "$PLUGIN"
echo "Installed plugin to $PLUGIN"

# --- Player settings (oras-mmo.txt, read by the plugin) --------------------------------------
write_config=1
if [ -f "$CONFIG_TXT" ]; then
  case "$(ask 'Settings already exist. Change them? (y/n)' n)" in [yY]*) ;; *) write_config=0 ;; esac
fi
if [ "$write_config" = 1 ]; then
  echo
  echo "Settings (you can change them later in the game: press SELECT for the plugin menu)."
  name="$(clean "$(ask 'Your name' "$(clean "$(id -un)" 15)")" 15)"
  [ -n "$name" ] || name=Trainer
  server="$(ask 'Server address (whoever hosts runs "Start server" and gives you their IP)' 127.0.0.1)"
  # The plugin only takes an IPv4 address, so resolve a hostname here.
  if ! printf '%s' "$server" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'; then
    ip=""
    if command -v dscacheutil >/dev/null; then
      ip="$(dscacheutil -q host -a name "$server" | awk '/^ip_address:/{print $2; exit}')"
    elif command -v getent >/dev/null; then
      ip="$(getent ahostsv4 "$server" | awk '{print $1; exit}')"
    fi
    [ -n "$ip" ] || { echo "Couldn't find an IPv4 address for '$server'." >&2; exit 1; }
    echo "  $server -> $ip (if the host's IP changes, run the installer again)"
    server="$ip"
  fi
  port="$(ask 'Server port' 7777)"
  room="$(clean "$(ask 'Room code (everyone in the same room sees each other)' HOENN)" 12 | tr '[:lower:]' '[:upper:]')"
  [ -n "$room" ] || room=HOENN
  case "$(ask 'Show friends as real trainers instead of name tags? (y/n)' y)" in [yY]*) puppets=1 ;; *) puppets=0 ;; esac

  # Keep settings the installer doesn't ask about (greeting, scale, overlay).
  keep=""
  [ -f "$CONFIG_TXT" ] && keep="$(grep -E '^(greeting|scale|overlay)=' "$CONFIG_TXT" | tr -d '\r' || true)"
  {
    printf 'server=%s\nport=%s\nroom=%s\nname=%s\npuppets=%s\n' "$server" "$port" "$room" "$name" "$puppets"
    [ -z "$keep" ] || printf '%s\n' "$keep"
  } > "$CONFIG_TXT"
  echo "Saved settings to $CONFIG_TXT"
fi

# --- Turn on Azahar's 3GX plugin loader -------------------------------------------------------
# qt-config.ini is a QSettings file: each setting has "key\default=true|false" plus "key=value".
mkdir -p "$(dirname "$QT_CONFIG")"
if [ -f "$QT_CONFIG" ]; then cp "$QT_CONFIG" "$QT_CONFIG.bak-oras-mmo"; else : > "$QT_CONFIG"; fi
tmp="$(mktemp)"
awk '
  BEGIN {
    n = split("plugin_loader\\default=false plugin_loader=true allow_plugin_loader\\default=true allow_plugin_loader=true", kv, " ")
    for (i = 1; i <= n; i++) { k[i] = substr(kv[i], 1, index(kv[i], "=") - 1) }
  }
  function flush(   i) { for (i = 1; i <= n; i++) if (!done[i]) print kv[i]; }
  /^\[.*\][ \t\r]*$/ {
    if (sys) flush()
    sys = ($0 ~ /^\[System\]/); if (sys) saw = 1
    print; next
  }
  sys {
    key = $0; sub(/=.*/, "", key)
    for (i = 1; i <= n; i++) if (key == k[i]) { print kv[i]; done[i] = 1; next }
  }
  { print }
  END {
    if (sys) flush()
    if (!saw) { print "[System]"; for (i = 1; i <= n; i++) print kv[i] }
  }
' "$QT_CONFIG" > "$tmp"
mv "$tmp" "$QT_CONFIG"
echo "Enabled the 3GX plugin loader in $QT_CONFIG"

echo
echo "Done! Start Pokemon Alpha Sapphire (v1.4) in Azahar. In game, press SELECT for the oras-mmo menu."
echo "If friends do not show up, check Azahar > Settings > System > \"Enable 3GX plugin loader\"."
