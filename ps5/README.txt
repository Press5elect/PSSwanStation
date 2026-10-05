SwanStation for PS5
===================

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the
libretro fork of DuckStation, with a controller interface of its own, drawing
with Vulkan (the RADV driver, linked into the title).

It contains no games and no original BIOS. Use games you own, from your own
discs.


Installing
----------

Copy the folder PPSA99248 to /data/homebrew/ on the console (FTP), so that
/data/homebrew/PPSA99248/eboot.bin exists, and let your homebrew loader
(ShadowMount, or whichever you use) register it. The swan icon appears on the
home screen.

The title needs a jailbroken console with kstuff running.


Where things go (all inside /data/homebrew/PPSA99248/)
------------------------------------------------------

  games/        your games; folders inside it are scanned too
  bios/         optional BIOS files of an original console
  covers/       cover pictures, named as the game's file: <name>.png or .jpg
  cheats/       your own cheat files, <serial>.cht (SLUS-00594.cht)
  network.cfg   the network share to read games from
  data/         memory cards (data/saves), save states, settings: written by
                the title
  logs/         the emulator's log of the last two runs

Game formats: .chd, .cue with its .bin files, .iso, .img, .pbp, .ecm, .mds,
and PlayStation programs (.exe, .psexe). A game on several discs named
"... (Disc 1)", "... (Disc 2)" is shown once; its discs share one memory card,
its details choose the disc it starts from, and the menu changes disc while
playing. Playlist files (.m3u) are not shown: the discs they list are grouped
by their names anyway.

BIOS: none is needed. The built-in OpenBIOS starts most games. An original
BIOS is more compatible: scph5500.bin (Japan), scph5501.bin (America),
scph5502.bin (Europe), in bios/.


Controls
--------

In the menus
  D-pad, left stick     move
  Cross / Circle        confirm / back (they can be swapped in Settings)
  L1, R1                the library's tabs; a settings section
  L2, R2                a page up or down
  Square                scan for games; in settings, back to the default;
                        in a game's details, the next disc
  Triangle              a game's details
  OPTIONS               the menu

In a game
  The DualSense is the PlayStation controller, button for button.
  Touch pad, left half   SELECT   (press the pad down)
  Touch pad, right half  START
  OPTIONS                SwanStation's menu: save and load states, change
                         disc, cheats and patches, settings, reset, close


The library
-----------

Games come from three places: Internal (the games folder), USB and Network.
Each has a tab at the top once it has games, and a place with none is not
shown: with games in the folder only there is the one tab, and L1 and R1
appear when there is a second. Square looks through all three again, so a
drive plugged in or a share filled since gets its tab. With no games anywhere
the screen says where each place's games go.

The games played lately are on a shelf of their own at the top ("Continue
playing"), with when and for how long; a game on several discs starts from
the disc last in the tray.

Triangle opens a game's details: its description, who made it and when (from
the game database, by the disc's serial number), how long it was played, and
four things to do:

  Play          starts it (from the disc chosen with Square, when it has several)
  Load state    starts it from a state saved earlier
  Options       the emulator's settings for this game alone
  Cheats        its cheats and patches, to switch on before it starts

USB: off until "USB drives" is switched on in Settings, Games and network; it
takes effect at the next start and needs the ELF loader listening on port
9021. Games are read from a folder named psx, ps1 or playstation at the top
of the drive.

Network (SMB, Windows sharing): name the share in network.cfg, for example
  path = 192.168.1.10/Games/PSX
with the server by its IP address, and restart SwanStation. The share is
scanned at that start and again with Square; the list is kept, so later
starts show it without asking the share. If the share does not answer, the
library's bottom line says so and there is no Network tab. A network game is read into memory before it starts ("Load network games into
memory"; switch it off to start at once and read while playing).

Covers: a cover is looked for in covers/ by the game's file name. With
"Download covers" on, missing ones are fetched from the libretro thumbnails
collection, which names them as the No-Intro and Redump sets name games.


Settings
--------

Every setting of the emulator is in Settings, in the emulator's own sections
(Console, Enhancement, Display, Port, Advanced), next to the title's own
(Interface, Picture, Sound, Controllers, Games and network).

A game's own settings are in its details (Options) and, while it runs, in
the menu: "Settings" changes them for every game, "Game settings" for this
game only. A dot marks a value that is the game's own; Square gives it back
to the general value.

The swan: with Animations (Interface) on Full, SwanStation opens with its
head coming up in the bottom right corner and paddling along the bottom of
the screen to the middle, where it goes under and takes the lift: the box
comes up between its rails to the middle of the screen, its bell rings, its
doors open, and there is the swan in its box with the name under it. Then it
flies to its box in the library's corner, looks about while it sits there,
and flies at the screen when a game starts. Any button skips the start-up
animation. Reduced keeps the swan still (the start-up screen is the finished
picture, with the bell) and starts games at once; Off stops every movement
of the interface and skips the start-up screen.

"Start-up sound" (Interface) is what that animation sounds like - water, the
lift, the bell, wings - at the Volume set under Sound. The sounds are made by
the title when it starts; none is a recording.

The picture: the internal resolution starts at 8x. Lower it (Enhancement,
Internal Resolution Scale) if a game does not hold its speed; "Show frame
rate" (Interface) tells.


Cheats and patches
------------------

The title carries the DuckStation cheat and patch database (assets/cheats.zip
and assets/patches.zip): cheat codes for about 4500 games and patches
(widescreen, 60 frames a second and others) for about 140. A game's entries
are in its details (Triangle in the library) and in its menu while it runs,
under "Cheats and patches", found by the disc's serial number. Cross switches one on or off; where a cheat has a value to choose
(a character, a car), Left and Right choose it. What is switched on is kept
for that game. A widescreen patch also sets the aspect ratio it needs while
it is on.

Your own codes: cheats/<serial>.cht in the same format, for example

  [Infinite Health]
  Type = Gameshark
  Activation = EndFrame
  800B7526 03E7


Save states
-----------

Ten slots for each game (menu, Save state and Load state), and, if "Save when
a game is closed" is on, one more that is written when the game is closed;
"Continue where I left off" starts from it. Memory cards work as on the
console and are kept one for each game.


When something goes wrong
-------------------------

/data/homebrew/PPSA99248/swanstation-boot.log is written from the first
moment of every start (the two starts before it are kept as .1.log and
.2.log); a crash writes its report there. logs/swanstation.log is the
emulator's own log. Those two files say what happened.

  The screen stays black after the icon is chosen: look at
  swanstation-boot.log; if there is none, the title did not start at all
  (kstuff, the folder's place and name).

  A game stops or crashes at once: try CPU Execution Mode "Cached
  Interpreter" (Settings, Console) and GPU Renderer "Software"
  (Enhancement). If one of the two helps, the log of the crash tells which
  part to fix.

  "CPU Recompiler Fast Memory Access" (Advanced) is off: its faster setting,
  LUT, is not proven on the console.

This is a work in progress: the build's number and date are in the top right
corner of the library and in Settings, About.
