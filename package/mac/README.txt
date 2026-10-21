oras-mmo - play Alpha Sapphire online with your friends (macOS)
===============================================================

Online (MMO) mod for Pokemon Alpha Sapphire v1.4 on the Azahar emulator.
You need your own copy of the game, updated to v1.4.

Install
-------
1. Quit Azahar (Cmd+Q).
2. Double-click "Install oras-mmo.command" and answer the questions
   (your name, the server address, a room code).
   The first time, macOS may say it can't verify the developer: right-click the
   file, choose Open, then Open again. (Or: System Settings > Privacy & Security >
   "Open Anyway".)
3. Start Alpha Sapphire in Azahar. Press SELECT in game for the oras-mmo menu.
4. Walk up to a friend and press A to talk: Battle (their team, under their
   name; losing is fine, your party is put back afterwards) or Trade (pick a
   Pokemon, and the friend answers by talking to you).
5. SELECT > Send chat > "/who" lists who's online and where. A friend inside a
   building shows up as "Name is inside" on its door once someone has walked
   through it.

The installer copies the plugin to
  ~/Library/Application Support/Azahar/sdmc/luma/plugins/000400000011C500/
saves your settings next to it (oras-mmo.txt), and turns on Azahar's
3GX plugin loader (Settings > System). A backup of Azahar's settings is kept as
qt-config.ini.bak-oras-mmo. If your Azahar keeps its files somewhere else, drag
that folder into the Terminal window when the installer asks for it.

Hosting
-------
One player runs "Start server.command" (needs Node.js 18+ from https://nodejs.org).
It prints the addresses friends should enter. Over the internet, use a VPN such as
Tailscale, or forward TCP port 7777 on the host's router.

Uninstall
---------
Double-click "Uninstall oras-mmo.command".
