PSSwanStation
=============

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the
libretro fork of DuckStation, with a controller interface of its own, drawing
with Vulkan (the RADV driver, linked into the title). PSSwanStation is the
title; SwanStation is the emulator inside it.

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
  cheats/       your own cheat files, <serial>.cht (SLUS-12345.cht)
  textures/     texture packs, one folder for each game: textures/<serial>/
  layouts/      Aurora cover layouts (.cfljson), offered as library views
  borders/      pictures for the sides of a 4:3 picture: <serial>.png or
                default.png
  music/        your own menu music: menu.ogg, menu.mp3 or menu.wav
  memcards/     import/ for cards and saves to bring in, export/ for saves
                put out
  network.cfg   the network share or FTP server to read games from
  data/         memory cards (data/saves) and their backups, save states,
                settings: written by the title
  logs/         the emulator's log of the last two runs

All of these but games/ can live in /data/psswanstation/ instead, where
replacing or deleting the title's folder cannot touch them: see "Keeping your
files outside the title folder" below.

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
  Cross / Circle        confirm / back (they can be swapped in Settings); back
                        in the library asks whether to close PSSwanStation
  L1, R1                the library's tabs; a settings section
  L2, R2                in the library, the letter before and the letter after;
                        in other lists, a page up or down
  Touch pad             in the library, search
  Square                in the library, sorting, filters and scanning; in
                        settings, back to the default; in a game's details,
                        the next disc
  Triangle              a game's details
  OPTIONS               the menu

In a game
  The DualSense is the PlayStation controller, button for button.
  Players 2 to 4 are the other users logged in on the console, each with
  their own controller (see "More players" below).
  Touch pad, left half   SELECT   (press the pad down)
  Touch pad, right half  START
  OPTIONS, tapped        PSSwanStation's menu: save and load states, change
                         disc, cheats and patches, netplay, achievements,
                         settings, reset, close
  OPTIONS held, and      R2 fast forward        L2 rewind
                         R1 save a state        L1 load it
                         Left, Right another slot
  ("Shortcuts", in Settings under Shortcuts and rewind, switches these off;
  the menu then opens the moment OPTIONS is pressed.)


The library
-----------

Games come from three places: Internal (the games folder), USB and Network.
Each has a tab at the top once it has games, and a place with none is not
shown: with games in the folder only there is the one tab, and L1 and R1
appear when there is a second. With no games anywhere the screen says where
each place's games go.

Square opens "Sort and filter": the order (by name, last played, most played,
year, size), what is shown (everything, favourites, games not played yet,
hidden games), one region alone, the view, and "Scan for games", which looks
through all three places, so a drive plugged in or a share filled since gets
its tab. The header shows the time (Settings, Interface, "Clock").

The views: Covers (a grid, with what was played lately on a shelf above it),
List (names, with the cover and the description beside them), and five that
stand the covers in space as cases, the way Aurora, the Xbox 360's homebrew
dashboard, shows games:

  Flow      the case under the cursor faces you, the others lean towards it
            from both sides, on a floor that mirrors them
  Row       a flat row, the one under the cursor nearer
  Wall      three rows across the screen that bend away at the ends
  Cascade   the one under the cursor at the left, the next ones behind one
            another away to the right, with its description
  Wheel     a wheel at the right turning past the one under the cursor (Up
            and Down turn it), with its description at the left

Left and Right move through them (Up and Down too, in the Wall and the
Wheel); L2, R2, the search and everything else work as in the grid. The view
is also chosen in Settings, Interface, "Library view".

Aurora's own layouts work too, as an experiment: copy a layout file
(.cfljson, from Aurora's Media/Layouts folder or one of the collections of
them) to layouts/ and it is in the list of views by its name. Aurora's code
is not public, so I worked out what a layout's numbers mean from the layouts
themselves: one may look somewhat different here, and a layout made for the
Xbox's tall cases stands PlayStation covers a little smaller.

Finding a game: L2 and R2 jump to the letter before and the letter after
(the letters stand down the right edge for a moment). Pressing the touch pad
opens the search: a keyboard for the D-pad on the left (Cross types, Square
deletes, Triangle is a space) and, on the right, every game from all three
places whose name has what was typed, as it is typed. Right goes over to the
games; Cross plays one, Triangle opens its details.

The games played lately are on a shelf of their own at the top ("Continue
playing"), with when and for how long; a game on several discs starts from
the disc last in the tray.

Triangle opens a game's details: its description, who made it and when (from
the game database, by the disc's serial number), how long it was played, and
five things to do:

  Play          starts it (from the disc chosen with Square, when it has several)
  Load state    starts it from a state saved earlier (each with its picture)
  Options       the emulator's settings for this game alone
  Cheats        its cheats and patches, to switch on before it starts
  More          favourite (a heart on its cover), hidden (out of the library
                until the filter shows hidden games), another cover, and
                whether it has a texture pack

Another cover: its box, its title screen or a moment of the game, fetched from
the libretro thumbnails collection by the game's file name.

A game's settings and cheats are kept by its disc's serial number. For a game
on the network that has not been played yet, that number is read from the
share the first time Options or Cheats is opened; the screen says so while
the share answers (a NAS whose disks sleep takes a few seconds) and can be
left with Circle meanwhile.

USB: off until "USB drives" is switched on in Settings, Games and network; it
takes effect at the next start and needs a resident Lapy service or the ELF
loader listening on port 9021 (the title leaves its sandbox with the Lapy
helper beside eboot.bin). Games are read from a folder named psx, ps1 or
playstation at the top of the drive.

Network: an SMB share (Windows sharing) or an FTP server, or several of
either. Name each folder in network.cfg with a "path" line, for example
  path = 192.168.1.10/Games/PSX
  path = ftp://192.168.1.10/games/psx
with the server by its IP address, and restart PSSwanStation. An FTP server on
another port is ftp://192.168.1.10:2121/games; one with an account of its
own is ftp://user:password@192.168.1.10/games (otherwise the "user" and
"password" lines are used, and guest with no password logs in to an FTP
server as anonymous). FTP is plain FTP, in passive mode: the password
crosses your network unhidden, as FTP sends it.

The folders are scanned at that start and again with Square; the list is
kept, so later starts show it without asking the server (it is made again
when network.cfg names other folders). If a server does not answer, the
library's bottom line says so; with no games found there is no Network tab.
A network game is read into memory before it starts ("Load network games into
memory"; switch it off to start at once and read while playing).

A server that sleeps: add its network card's hardware address to network.cfg,
  wake = 00:11:32:AA:BB:CC
and the wake-up packet (Wake-on-LAN) is sent when PSSwanStation starts,
whenever the share is scanned, and from Settings, Games and network, "Wake the
server". The server has to have Wake-on-LAN switched on in its own settings.

Covers: a cover is looked for in covers/ by the game's file name. With
"Download covers" on, missing ones are fetched from the libretro thumbnails
collection, which names them as the No-Intro and Redump sets name games.


Settings
--------

Every setting of the emulator is in Settings, in the emulator's own sections
(Console, Enhancement, Display, Port, Advanced), next to the title's own
(Interface, Picture, Sound, Controllers, Shortcuts and rewind, Games and
network, RetroAchievements).

A game's own settings are in its details (Options) and, while it runs, in
the menu: "Settings" changes them for every game, "Game settings" for this
game only. A dot marks a value that is the game's own; Square gives it back
to the general value. A game can have its own of every setting of the
emulator and of the Picture page: scaling (FSR 1 too) and its sharpening,
smooth scaling, the picture preset, the picture tube, what is beside the
picture, frame pacing, black frame insertion and frame generation. They are
kept with the game (data/game-options/<serial>.cfg) and in force whenever it
runs; other games keep the general ones. Display output alone is for every
game: the console reads it when PSSwanStation starts, before any game is
chosen.

Closing PSSwanStation: Circle in the library asks, and Cross answers yes; or
OPTIONS, "Close PSSwanStation".

More players: player 1 is whoever started PSSwanStation. A second person
presses the PS button on their controller and chooses a user; they are
player 2, in port 2, within a couple of seconds, with no restart. For three
or four, switch the Multitap on (Settings, Controllers): "In port 1" puts all
four on it, which is what most four-player games expect; "In port 2" leaves
player 1 in port 1. Leave it off otherwise: some games for one or two do not
see a controller behind a multitap. Each player's controller type is set on
the same page, where the first line shows who is connected.

The swan: with Animations (Interface) on Full, PSSwanStation opens with its
head coming up in the bottom right corner and paddling along the bottom of
the screen to the middle, where it goes under and takes the lift: the box
comes up between its rails to the middle of the screen, its bell rings, its
doors open, and there is the swan in its box with the name under it. Then it
flies to its box in the library's corner, looks about while it sits there,
and flies at the screen when a game starts. Any button skips the start-up
animation. Reduced keeps the swan still (the start-up screen is the finished
picture, with the bell) and starts games at once; Off stops every movement
of the interface and skips the start-up screen.

"Interface sounds" (Interface) are the small sounds of the menus: the cursor
moving, a choice, a step back, a letter typed.

"Start-up sound" (Interface) is what that animation sounds like - water, the
lift, the bell, wings - at the Volume set under Sound. The sounds are made by
the title when it starts; none is a recording.

The picture: the internal resolution starts at 8x. Lower it (Enhancement,
Internal Resolution Scale) if a game does not hold its speed; "Show frame
rate" (Interface) tells.

Picture (Settings):
  Scaling            fit the screen, whole multiples, stretch, or fit the
                     screen with FSR 1 (below).
  Picture preset     Original (the PlayStation's own resolution, with a picture
                     tube's lines), Sharp (8x, full colour, PGXP) or Enhanced
                     (Sharp with xBR textures and 4x MSAA), set for every game
                     at once; each setting can still be changed by itself.
  Picture tube       scanlines over the game's picture, soft or full, and the
                     tube's mask.
  Beside the picture what fills the sides of a 4:3 picture: black, the
                     picture's own light, a gradient, or a picture file from
                     borders/.
  Frame pacing       by the display (the usual), the game's own speed, or the
                     game's own speed by the clock (for a display with a
                     variable refresh rate).
  Display output     60 Hz, 120 Hz, or 120 Hz with a variable refresh rate
                     left on; from the next start.
  Black frame insertion   at 120 Hz, a black refresh between a 60 fps game's
                     frames: clearer movement, a darker picture.
  Frame generation   pictures of the title's own between the game's: off,
                     on, or on and lighter (below).

Display output is kept in the title's own sce_sys/param.json, which is where
the console reads it when the title starts. If the screen stays dark after it
was changed: start PSSwanStation holding L1 and R1, wait ten seconds, close it
and start it again. Holding the two puts the output back to 60 Hz for the
next start without anything having to be seen. An update puts the file back
too; the first start after it writes the setting again and the one after that
has it. 120 Hz and the variable refresh rate have not run on a console from
this title yet.

FSR 1 ("Scaling", "Fit the screen, FSR 1") enlarges the game's picture to the
screen with AMD's FidelityFX Super Resolution 1 instead of a plain filter:
edges stay clean, and the picture is sharpened ("FSR sharpening": soft,
normal, sharp). It is worth most with a lower Internal Resolution Scale
(Enhancement): 3x or 4x with FSR looks close to a higher scale and leaves the
console more time for everything else. A picture already as large as the
screen is left alone. This is FSR 1, the kind that works on one finished
picture. FSR 2, 3 and 4 rebuild a picture from several frames and need each
pixel's depth and movement, which an emulated PlayStation does not give, and
the FSR 4 that exists for PS5 homebrew runs on another Vulkan driver than
this title's. Tested on a PC; not yet seen on a console in this title.

Frame generation ("Off", "On", "On, lighter") draws pictures of the title's
own between the game's, worked out from how the picture moved, so that
movement is smoother than the game makes it. The title counts the pictures
the game really draws, which is rarely as many as the PlayStation sends to
the screen: most games draw 30 a second, many 20 or fewer, a few 60. What
that gives:

  a game that draws 30   at 60 Hz one picture is made between each two
                         (60 shown); at 120 Hz three (120 shown)
  a game that draws 20   two are made between each two at 60 Hz, five at
                         120 Hz
  a game that draws 60   left alone at 60 Hz: every refresh has a picture of
                         the game's own. At 120 Hz one is made between each
                         two.
  a PAL game that draws  its pictures are spread evenly over the
  50, at 60 Hz           refreshes, without the stutter that has otherwise

A game counts when it draws into one part of the PlayStation's picture memory
while it shows another and then swaps them, as nearly all 3D games do. One
that draws straight into what is shown cannot be counted: each of its frames
is then taken for a new picture, which is right for a game that draws 60 and
gains nothing for one that draws fewer. Settings, Picture, "Frame generation"
says what the running game is counted at.

How a picture is made: the two game pictures are compared at four sizes,
from small to large, to find how each part moved; where two things meet, each
place of the picture then tries its neighbours' movements, and standing still
(which is what writing over the picture does), and keeps the one under which
the two game pictures agree; and the made picture takes each place from the
nearer of the two game pictures only, moved. Where no movement makes the two
agree (something came into view, or the scene changed) the nearer game
picture is shown in that place as it is: a moment's stutter there and no
ghost.

Build 12 smeared, for two reasons, both gone. It took each of the game's
frames for a new picture, so that at 60 Hz a game was mostly shown in made
pictures, its own left out; and it mixed the two game pictures in every made
one, which shows the smallest error as a doubled, blurred edge. On my test
scene (a street scrolling behind a walking figure, a turning crate and
writing on top, where the true picture between is known) the made pictures
are now 26.4 dB from the true ones, where build 12's were 21.5 and the nearer
game picture shown twice is 15.9 (more is nearer the true picture).

What is still wrong in a made picture: a thin fringe where something moves
across a background that moves another way, a few pixels wide, on the side it
moves towards; stripes and other even patterns can break when they move by
about their own spacing; things entering at the picture's edge appear a
picture late. Lossless Scaling's guide says of its own frame generation that
it wants 40 pictures a second to go by at the least and 60 for good results,
and that ghosting falls with more: a PlayStation game's 30 or 20 are under
that, and no method makes them clean. At 20 the made pictures outnumber the
game's two to one and the errors show most.

"On, lighter" decides the movement on a smaller picture and skips the last
refinement: less work for the graphics processor, coarser edges. It is for
when the frame rate drops with "On" (the frame counter under Interface
tells), which a high Internal Resolution Scale makes likelier.

The game answers the pad a refresh of the screen later with one picture made
between two, two refreshes later with two. Frame generation is off while fast
forwarding and rewinding, and black frame insertion is off while it is on. It
works with FSR 1 (the enlarged pictures are what it works on). This is the
title's own way of doing it, not AMD's or Nvidia's, which need more than an
emulated PlayStation gives. An experiment: build 12's ran on a console and
smeared; this one has run on a PC only.

Sound: "Menu music" plays in the library and the menus: the title's own quiet
piece, or your file (music/menu.ogg, .mp3 or .wav).

Controllers: "Buttons" changes which button is which and makes buttons fire
again and again while held (turbo). The controller types now include the
neGcon (the triggers are its I and II) and the GunCon (aimed with the sticks,
and by turning the pad; R2 or Cross shoots, L2 or Circle reloads). "Tilt
steering" leans the first pad like a steering wheel. "Player lights" gives
each pad's light bar its player's colour. A pad's charge is not shown: I know
of no call a title can make that gives it.

Interface: "The swan takes the screen" when nothing was pressed for a while in
the menus (never while a game is on the screen), to spare the display.


Cheats and patches
------------------

The cheats and patches are the DuckStation project's chtdb: cheat codes for
about 4500 games and patches (widescreen, 60 frames a second and others) for
about 140. A release of PSSwanStation does not carry the database, because
its entries belong to their authors and the project gives no licence to pass
them on: Settings, Games and network, "Fetch the newest cheat database" gets
it once from the project's own releases (and again whenever you ask, as it
grows); it is kept in data/ and used from then on. A game's entries are in
its details (Triangle in the library) and in its menu while it runs, under
"Cheats and patches", found by the disc's serial number. Cross switches one
on or off; where a cheat has a value to choose (a character, a car), Left
and Right choose it. What is switched on is kept for that game. A widescreen
patch also sets the aspect ratio it needs while it is on.

Your own codes: cheats/<serial>.cht in the same format, for example

  [Infinite Health]
  Type = Gameshark
  Activation = EndFrame
  800B7526 03E7



Save states
-----------

Ten slots for each game (menu, Save state and Load state), each shown with
the picture it was saved at, and, if "Save when a game is closed" is on, one
more that is written when the game is closed; "Continue where I left off"
starts from it.

Rewind (Settings, Shortcuts and rewind; off at first): keeps the last while of
play so that OPTIONS and L2 go back through it. It is kept in memory only,
128 MB to 1 GB of it, and never written to the console's storage; it is gone
when the game closes. Fast forward (OPTIONS and R2) runs at 2x to 8x, or as
fast as the console manages.

Screenshots and video are the console's own (the Create button).


Memory cards
------------

Memory cards work as on the console and are kept one for each game, in
data/saves. Menu, "Memory cards" (with no game running) shows every card and
the saves on it, with their pictures: copy a save to another card, delete it,
put it out as a file (memcards/export, .mcs), and bring cards or saves in from
memcards/import: whole cards (.mcd .mcr .mc .gme .vmp and other raw images)
and single saves (.mcs .psx .psv and others). I have tested the raw card and
.mcs forms most; the rarer ones follow their descriptions.

When a game closes and its card changed, a copy of the card is kept (the last
ten of each; Settings, Games and network, "Card backups"), and the manager can
go back to one.


Texture packs
-------------

Replacement pictures for a game's 2D art, as made for DuckStation (files named
vram-write-<number>.png), go in textures/<serial>/ and are used when "Enable
VRAM Write Texture Replacement" is on (Enhancement). This emulator knows that
one kind of pack, not the newer kind that replaces a 3D game's textures.

They can stay on a USB drive: a folder named textures inside the drive's games
folder, as psx/textures/<serial>/ (with "USB drives" on). A game's pack in the
title's own textures/ comes first; a game's details, under More, say where
the one in use is.


RetroAchievements
-----------------

Settings, RetroAchievements: switch it on and sign in with your account from
retroachievements.org (the password is typed once; only the token the server
gives back is kept, in data/). A game's achievements are then in its menu, and
earning one is shown as it happens. Hardcore mode earns more and switches off
loading states, cheats and rewind.

It works on the console against retroachievements.org (build 10).


Netplay
-------

Two consoles, one game, players 1 and 2: start the game, then Menu, Netplay.
One console hosts and shows its address; the other chooses "Join a game" and
types it (port 28800 on the host). Both need the same disc, the same BIOS and
this build; the host's emulator settings are used on both. Memory cards,
cheats, save states, fast forward and rewind are out while it lasts.

This is an experiment. It held 3300 frames in step between two PCs here; it
has not run between two consoles.


Updates
-------

Menu, "Update" asks github.com/Press5elect/PSSwanStation's releases for a
newer build, shows what changed, fetches it, checks it, and replaces the
program's files (eboot.bin, sce_sys, sce_module, assets, licences, the texts).
Your games, saves, states, covers and settings are not touched. PSSwanStation
closes when it is done; start it again from the home screen. With "Ask for
updates at start" (Interface) the library's header says when a build is out;
nothing is fetched until you say so.

Tested on a PC, against a stand-in for GitHub and against the real releases
page (as build 13 it found build 14, fetched it, checked it and put it in
place, leaving network.cfg and the games alone); not yet on a console. Should
an update go wrong half way, the next start puts the earlier files back.


Keeping your files outside the title folder
-------------------------------------------

Settings, Games and network, "Keep my files outside the title folder": BIOS
files, covers, cheats, memory cards, states, layouts and settings then
live in /data/psswanstation/, where replacing or deleting
/data/homebrew/PPSA99248 cannot take them along. From the next start, what
the title's folder holds is copied over once (and left where it is). The
console hides that folder from a title, so PSSwanStation has to leave its
sandbox each time it starts: that needs a resident Lapy service or the ELF
loader on port 9021, as USB drives do. Without either it says so and uses the
title's folder. Not yet run on a console.


Safe start
----------

Hold L1 and R1 while PSSwanStation starts for "Safe start". The display
output goes back to 60 Hz at once (for the next start), and the page offers
the rest: no leaving the sandbox (USB drives and files outside off), the
interface's settings or the emulator's back to the usual. If a start does not
get as far as the library, the next one stays in the sandbox and at 60 Hz by
itself, for that one start, and says so.


When something goes wrong
-------------------------

/data/homebrew/PPSA99248/psswanstation-boot.log is written from the first
moment of every start (the two starts before it are kept as .1.log and
.2.log); a crash writes its report there. logs/psswanstation.log is the
emulator's own log. Those two files say what happened. If the picture ever
stands still, the first one has a line saying when and for how long ("the
screen stood still for ..."), after the lines that say what was being done.

  The screen stays black after the icon is chosen: look at
  psswanstation-boot.log; if there is none, the title did not start at all
  (kstuff, the folder's place and name).

  A game stops or crashes at once: try CPU Execution Mode "Cached
  Interpreter" (Settings, Console) and GPU Renderer "Software"
  (Enhancement). If one of the two helps, the log of the crash tells which
  part to fix.

  "CPU Recompiler Fast Memory Access" (Advanced) is off: its faster setting,
  LUT, is not proven on the console.

This is a work in progress: the build's number and date are in the top right
corner of the library and in Settings, About.
