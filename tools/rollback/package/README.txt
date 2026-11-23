Fate/unlimited codes - Online with rollback (test version)
==========================================================

This is a modified PCSX2 (PS2 emulator) with rollback netcode for
Fate/unlimited codes (PS2, SLPM-55108). Two players connect directly by IP
address. One hosts (player 1), the other joins (player 2).


1. One-time setup
-----------------

Easiest: run "FUC-Netplay-Setup-<date>.exe". It installs without admin
rights, asks for your BIOS and your disc image, and creates shortcuts.
Running a newer setup later updates the program and keeps your settings.
With the zip package instead, do a) and b) below (or choose both files in
the start window with "Browse...").

a) BIOS: copy the BIOS file of your own PS2 (4 MB, usually .bin) into the
   "bios" folder.

b) Game: copy your disc image of Fate/unlimited codes (SLPM-55108, from your
   own disc) into the "game" folder. .iso, .chd, .cso and .bin/.cue work.
   The file name does not matter.

c) HOST only: set up port forwarding in your router:
   UDP port 7000 to the local IP address of your PC.
   The joining player needs no port forwarding.

The first time you play online, Windows Firewall asks whether PCSX2 may use
the network: choose "Allow" (private and public networks).

Windows SmartScreen may warn on the first start because the files are not
signed ("More info" > "Run anyway").


2. Starting
-----------

Run "pcsx2-qtx64-clang.exe". The start window opens:

- BUTTON CONFIG:
    The game's button config, like its own CONTROLLER SETTINGS menu: click
    an action (L - ATTACK, REFLECT GUARD, SP ATTACK A, ...), then press the
    button for it on your controller. The selection then moves on to the
    next action. D-pad: move, hold right for a second: clear the action,
    Esc: stop. Set it once; it is saved and used for training and online,
    whether you are player 1 or player 2.
    Map controller: if a button is not recognized, first set which button of
    your controller is which PS2 button (controller port 1). An Xbox/XInput
    controller is usually detected automatically.

- ONLINE:
    Name: your player name (up to 20 characters), shown above your health
    bar in the character select and the fight together with your wins,
    e.g. "MAWARU / 2WINS". Long names are drawn narrower.
    Delay: input delay, 1 frame recommended. Both players should pick the
    same value. A higher value helps against rollback stutter on high ping.
    Host: click "HOST (PLAYER 1)" and tell the other player your public IP
    address (e.g. from whatismyipaddress.com).
    Join: enter the host's IP address and click "JOIN (PLAYER 2)".

- TRAINING: practice on your own, offline.

- DISPLAY: resolution (common 4:3 sizes, 640 x 480 to 2880 x 2160) and
  window or fullscreen. The game is rendered at least as sharp as the
  chosen resolution; in a window it gets exactly that size (or the largest
  4:3 size that fits your screen).

The game boots automatically at high speed to the character select
(online: "Vs. 2P", training: "Practice"), all characters unlocked. This
takes a few seconds, without sound. Online, it then waits for the
connection: "Netplay: waiting for player 2 ..." counts the seconds in the
top left. That is normal. Once both are connected, "Netplay: connected"
appears and the game runs.

Fullscreen: Alt+Enter.
To end a game: close the game window; the start window comes back.


3. Good to know
---------------

- Both players boot the game from their own copy; no game data is sent. So
  both need the same BIOS version and game. The start window warns if your
  BIOS is another version than the organizer's. If the players' BIOS
  versions differ, a warning appears right after connecting.
- Memory cards are off. That is intentional: online, both games must be
  exactly the same. Settings changed in the game's menus are therefore not
  saved; the button config in the start window is.
- Run-ahead is on: the game reacts about one frame faster to your input. If
  the PC is too slow for it, it switches itself off (message at the top) and
  the game keeps running normally.
- If "desync detected" appears at the top, the two games ran apart. Please
  report it.
- During an online game, do not press F1, F3, F4, Space or Tab (save states,
  pause, speed): they disturb the session.


4. Security
-----------

- The program downloads nothing, starts no other programs and changes
  nothing in Windows (no registry, no autostart).
- Over the network only the other player's controller input arrives (which
  buttons are pressed), and only during a session.
- Only accept the package from the organizer. A SHA-256 checksum
  (".sha256.txt" next to the zip) lets you check it.


5. After a test
---------------

Please send the files in the "logs" folder to the organizer
("emulog.txt" and the "emulog-<date>-<time>.txt" of the session). They
contain connection and desync information (no personal data except file
names).


License and source code
-----------------------

PCSX2 is licensed under the GNU GPL version 3 (see docs\GPL.html). This
package is based on PCSX2 v2.8.2 (https://github.com/PCSX2/pcsx2, tag
v2.8.2). All changes are in the "source" folder as a patch. GGRS (rollback
library) is licensed under MIT/Apache-2.0 (docs\ggrs).
