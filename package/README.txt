oras-mmo - play Alpha Sapphire online with your friends
=======================================================

Online (MMO) mod for Pokemon Alpha Sapphire v1.4 on the Azahar emulator (Windows).
You need your own copy of the game, updated to v1.4.

Install
-------
1. Close Azahar.
2. Double-click "Install oras-mmo.bat" and answer the questions
   (your name, the server address, a room code).
3. Start Alpha Sapphire in Azahar. Press SELECT in game for the oras-mmo menu.
4. Walk up to a friend and press A to talk: Battle (their team, under their
   name; losing is fine, your party is put back afterwards) or Trade (pick a
   Pokemon, and the friend answers by talking to you).
5. SELECT > Send chat > "/who" lists who's online and where. A friend inside a
   building shows up as "Name is inside" on its door once someone has walked
   through it.

The installer copies the plugin to
  %APPDATA%\Azahar\sdmc\luma\plugins\000400000011C500\
saves your settings next to it (oras-mmo.txt), and turns on Azahar's
3GX plugin loader (Emulation > Configure > System). A backup of Azahar's
settings is kept as qt-config.ini.bak-oras-mmo.

Hosting
-------
One player runs "Start server.bat" (needs Node.js 18+ from https://nodejs.org).
It prints the addresses friends should enter. Over the internet, forward TCP
port 7777 on the host's router, or use a VPN such as Tailscale.

Uninstall
---------
Double-click "Uninstall oras-mmo.bat".

Manual install (portable Azahar, Linux)
---------------------------------------
Copy oras-mmo.3gx into <Azahar user folder>/sdmc/luma/plugins/000400000011C500/
and turn on Emulation > Configure > System > "Enable 3GX plugin loader".
Settings go in oras-mmo.txt in the same folder, for example:
  server=192.168.1.20
  port=7777
  room=HOENN
  name=Sami
  puppets=1
