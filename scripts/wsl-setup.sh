#!/usr/bin/env bash
# One-time toolchain setup inside WSL Ubuntu: devkitPro (3DS) + libctrpf + 3gxtool.
# Run: bash /mnt/e/oras-coop/scripts/wsl-setup.sh   (asks for your Ubuntu sudo password)
set -euo pipefail

sudo apt-get update
sudo apt-get install -y wget make ca-certificates

if ! command -v dkp-pacman >/dev/null 2>&1; then
  tmp=$(mktemp -d)
  # devkitPro's Cloudflare blocks wget's default user agent; use the one their own script uses.
  wget -U "dkp apt" -O "$tmp/install-devkitpro-pacman" https://apt.devkitpro.org/install-devkitpro-pacman
  chmod +x "$tmp/install-devkitpro-pacman"
  sudo "$tmp/install-devkitpro-pacman"
  rm -rf "$tmp"
fi

conf=/opt/devkitpro/pacman/etc/pacman.conf
if ! grep -q '^\[thepixellizeross-lib\]' "$conf"; then
  sudo cp "$conf" "$conf.bak-oras-coop"
  sudo tee -a "$conf" >/dev/null <<'EOF'

[thepixellizeross-lib]
Server = https://thepixellizeross.gitlab.io/packages/any
SigLevel = Optional

[thepixellizeross-linux]
Server = https://thepixellizeross.gitlab.io/packages/x86_64/linux
SigLevel = Optional
EOF
fi

sudo dkp-pacman -Sy --noconfirm
sudo dkp-pacman -S --noconfirm --needed 3ds-dev libctrpf 3gxtool

grep -q DEVKITPRO ~/.bashrc || cat >> ~/.bashrc <<'EOF'
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
EOF

echo "Done. Installed:"
dkp-pacman -Q devkitARM libctru libctrpf 3gxtool
