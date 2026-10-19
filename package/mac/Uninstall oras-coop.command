#!/bin/bash
# Removes the oras-coop plugin from Azahar (keeps your settings file).
bash "$(dirname "$0")/install.sh" --uninstall "$@"
read -r -p "Press Return to close." _
