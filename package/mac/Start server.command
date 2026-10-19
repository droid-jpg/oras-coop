#!/bin/bash
# Runs the oras-coop relay server (needs Node.js 18+ from https://nodejs.org).
# Only one player needs to run this; everyone else enters this Mac's address when installing.
if ! command -v node >/dev/null; then
  echo "Node.js is not installed. Get it from https://nodejs.org"
else
  node "$(dirname "$0")/server/server.mjs" "$@"
fi
read -r -p "Press Return to close." _
