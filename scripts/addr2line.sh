#!/usr/bin/env bash
# Usage: bash addr2line.sh <addr>...   (plugin addresses, e.g. 0x0700D6A4)
/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -f -C -i -e "$(dirname "$0")/../plugin/oras-mmo.elf" "$@"
