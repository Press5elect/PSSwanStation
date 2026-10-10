# PSSwanStation Manual

For build 17 · October 2026

## About PSSwanStation

PSSwanStation plays PlayStation (PS1) games on a jailbroken PS5, with an interface made for the controller. Inside it is SwanStation, the libretro fork of DuckStation; it draws with Vulkan through the RADV driver built into the title. This manual covers build 17.

**What you need**

- A PS5 that runs homebrew, with kstuff running and a homebrew loader (ShadowMount or another) that registers title folders.
- An FTP client on a computer, to copy the title folder to the console.
- Your own games, as disc images you made from discs you own.
- Optionally, a BIOS dumped from a PlayStation you own. None is needed: the built-in OpenBIOS starts most games.

**What is never included.** PSSwanStation contains no games, no BIOS files, no console firmware and no keys, and none will ever be provided or linked to. Please don't ask for them, or post links to them, in issues or discussions.

PSSwanStation is free software under the GNU General Public License, version 3. It is an unofficial port, not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment.

## Installing and updating

Copy the `PPSA99248` folder to the console and let your homebrew loader register it; the swan icon then appears on the home screen.

1. Unpack the release ZIP on your computer.
2. Copy the whole `PPSA99248` folder to `/data/homebrew/` over FTP, so that `/data/homebrew/PPSA99248/eboot.bin` exists.
3. Let your homebrew loader register it.
4. Put your games in `games/` inside that folder (or use a USB drive or a network share, see Your files).
5. Start PSSwanStation from the home screen. It writes `network.cfg`, with an explanation inside, at its first start.

**Updating.** Copying a newer build over the old folder keeps your games, saves, states, covers and settings: the ZIP holds only the program's files. From the title itself, Menu, "Update" asks the project's GitHub releases for a newer build, shows what changed, fetches it, checks it and replaces the program's files. PSSwanStation closes when it is done; start it again. If an update stops half way, the next start puts the earlier files back. With "Ask for updates at start" (Settings, Interface) the library's header tells you when a build is out.

**Install again.** When you already have the newest build, Square on the Update page fetches it again and puts it in place, which mends a damaged title folder; your games, saves and settings stay.

**From build 17 or earlier, copy the folder.** The updater in builds 14 to 17 downloads and checks a new build, then stops with "The unpacked update is no longer there" and changes nothing. Copy the new `PPSA99248` folder over the old one by FTP instead; from build 18 on, the updater works.

**What's new.** The first start after an update, by the updater or by copying the folder over, opens a list of what was added, changed and fixed since the build you had, build by build, with a count of each at the top. Up and Down or a stick scroll it, L1 and R1 a page at a time, and Cross closes it. Settings, About, "What's new" shows every build's changes again. A first install skips it.

**Safe start.** Hold L1 and R1 while PSSwanStation starts. The display goes back to 60 Hz for the next start, and a page offers to turn off leaving the sandbox (USB drives and files kept outside), or to put the interface's or the emulator's settings back to the usual. If a start never reaches the library, the next start does this by itself, once, and says so.

**Cheats and patches, once.** A release does not carry the cheat database. Settings, Games and network, "Fetch the newest cheat database" downloads it from its project when you want it.

## Your files

Everything lives in `/data/homebrew/PPSA99248/` unless you choose to keep your files elsewhere.

| Folder or file | What goes there |
| --- | --- |
| `games/` | Your games; folders inside it are scanned too |
| `bios/` | Optional BIOS files: `scph5500.bin` (Japan), `scph5501.bin` (America), `scph5502.bin` (Europe) |
| `covers/` | Cover pictures named as the game's file: `<name>.png` or `.jpg` |
| `cheats/` | Your own cheat files, `<serial>.cht` (for example `SLUS-12345.cht`) |
| `textures/` | Texture packs, one folder per game: `textures/<serial>/` |
| `layouts/` | Aurora cover layouts (`.cfljson`), offered as library views |
| `borders/` | Pictures beside a 4:3 picture: `<serial>.png` or `default.png` |
| `music/` | Your own menu music: `menu.ogg`, `menu.mp3` or `menu.wav` |
| `memcards/` | `import/` for cards and saves to bring in, `export/` for saves put out |
| `network.cfg` | The network shares or FTP servers to read games from |
| `data/` | Memory cards, their backups, save states and settings (written by the title) |
| `logs/` | The emulator's log of the last two runs |

**Game formats:** `.chd`, `.cue` with its `.bin` files, `.iso`, `.img`, `.pbp`, `.ecm`, `.mds`, and PlayStation programs (`.exe`, `.psexe`). A game on several discs named "... (Disc 1)", "... (Disc 2)" shows once, and its discs share one memory card.

**USB drives.** Switch on "USB drives" in Settings, Games and network; it takes effect at the next start. It needs a resident Lapy service, or the ELF loader listening on port 9021. Games are read from a folder named `psx`, `ps1` or `playstation` at the top of the drive.

### Network shares: network.cfg

PSSwanStation reads games from SMB shares (Windows sharing), NFS exports and FTP servers named in `network.cfg`. The file is in the title's folder (or wherever "Where my files are kept" puts your files). The title writes it at its first start, with every line explained and the example lines switched off.

**The rules of the file**

- One setting per line, as `name = value`. A line starting with `#` is switched off: remove the `#` to use it.
- Give every server by its **IP address** (`192.168.1.10`), never its name. Names are refused.
- Restart PSSwanStation after editing it. The folders are scanned at that start; the list of games is kept, and Square, "Scan for games" scans again.

**The settings**

| Line | What it does | Example |
| --- | --- | --- |
| `path` | A folder to scan for games; one line per folder, as many as you like | `path = 192.168.1.10/Games/PSX` |
| `user` | The account for SMB shares (and for FTP servers whose address names none); `guest` for an open share | `user = jacob` |
| `password` | That account's password; empty for guest | `password = secret` |
| `domain` | The SMB domain or workgroup, if the server wants one | `domain = WORKGROUP` |
| `files` | A writable SMB or NFS folder for PSSwanStation's own files (shared memory cards, covers, or all your files) | `files = 192.168.1.10/Games/PSSwanStation` |
| `wake` | The server's network card hardware address, to wake it (Wake-on-LAN) | `wake = 00:11:32:AA:BB:CC` |

**How a `path` is written**

| Kind | Form | Notes |
| --- | --- | --- |
| SMB | `server/share/folder` | `smb://` in front and backslashes are accepted too. SMB 2 and 3; SMB 1 is not supported, so a very old NAS may need SMB 2 switched on in its settings |
| NFS | `nfs://server/export/folder` | Version 3 when offered, else 4. A NAS that checks users: `?uid=1026&gid=100` (0 when not given); `&version=4` skips version 3. The export must let the console's IP address in |
| FTP | `ftp://server/folder` | Another port: `ftp://server:2121/folder`. Its own account: `ftp://user:password@server/folder`. Plain FTP, passive mode; FTP servers are only read, never written |

**An example**, for a NAS with an account, an SMB games folder, an NFS one, and your files on the share:

```
path = 192.168.1.10/Games/PSX
path = nfs://192.168.1.10/volume1/games/psx?uid=1026&gid=100
user = jacob
password = secret
files = 192.168.1.10/Games/PSSwanStation
wake = 00:11:32:AA:BB:CC
```

The one `user` and `password` are used for every SMB share, so put shares that need different accounts behind one account on the server. The password is kept unhidden in `network.cfg`, and FTP sends it unhidden over the network; the phone and web panel never gives out `network.cfg`.

**When it doesn't work**, the library's bottom line says why: a name instead of an IP address, a refused account (check `user`, `password` and `domain`), or a server that did not answer (switch it on, or set `wake`). With no games found there is no Network tab. A network game is read into memory before it starts; switch off "Load network games into memory" (Games and network) to start at once and read while playing.

### Your files on the NAS

A NAS can hold PSSwanStation's own files as well as games, so every console at home shares the same memory cards and covers, and card backups have a copy off the console.

**Setting it up**

1. On the NAS, make a folder the console can write to, on an SMB share or an NFS export (an FTP server can't be written). For example a folder `PSSwanStation` in the share `Games`.
2. In `network.cfg` on each console, add a `files` line pointing at it, with the account lines if the share needs them:

```
files = 192.168.1.10/Games/PSSwanStation
user = jacob
password = secret
```

3. Restart PSSwanStation.
4. In Settings, Games and network, choose what goes there (next table): switch on "Memory cards on the network share" and "Covers on the network share", or set "Where my files are kept" to the network share for everything.
5. Do the same on each console. Choose "Bring my files up to date now" on the first console, so the NAS has its cards before the others look.

**What is kept there**

| What | With "Memory cards" and "Covers on the network share" | With "Where my files are kept: the network share" |
| --- | --- | --- |
| Memory cards | Both ways, shared between consoles (`memory cards/`) | Both ways |
| Covers | Both ways (`covers/`): downloaded or chosen on one console, on every console | Both ways |
| Card backups | Sent to `card backups/<console>/`, a copy off each console | Sent the same way |
| Save states, settings, cheats, BIOS, textures, music, borders, layouts | Stay on each console | Both ways |

The NAS is brought up to date when PSSwanStation starts, when a game closes, and when you choose "Bring my files up to date now"; never while a game runs. With "Where my files are kept" on the network share, the console works on its own copy, so games still start, play and save when the NAS is off.

**Two consoles, one game.** If two consoles played the same game on their own copy of a card before they met, the one played last is kept and the other is saved beside it as `<card>.conflict-<date>-<time>`, so nothing is lost. A card or cover removed on one console is removed on the others; the console keeps its copy in `data/sync/removed/`.

**Card backups.** Each console sends the backups it keeps (the last ten of each card, Settings, Games and network, "Card backups") to a folder of its own, so consoles never write over each other's. The NAS follows the console: a backup the console trims goes from the NAS too. Nothing comes back from that folder by itself. To restore a backup on a new or reset console, copy it from `card backups/<console>/<card>/` into `data/saves/backups/<card>/`, then go back to it in the memory card manager.

**Naming your consoles.** A console's name is made at its first sync, as `console-` and six letters, and kept in `data/console-name.txt`. Change it there (letters, digits, `-` and `_`), for example `living-room`, to tell the folders apart; it is never copied to the NAS or to other consoles.

**Other places for your files.** "Where my files are kept" can also choose `/data/psswanstation/` on the console, or `PSSwanStation/` on a USB drive, so that replacing or deleting the title's folder can't take your files along. Every place but the title's folder needs a resident Lapy service or the ELF loader on port 9021, as USB drives do.

## The library

The library shows your games from three places, each a tab once it has games: Internal (the `games` folder), USB and Network. L1 and R1 change tab.

| Button | In the library |
| --- | --- |
| D-pad, left stick | Move |
| Cross | Play (Cross and Circle can be swapped in Settings) |
| Circle | Asks whether to close PSSwanStation |
| Triangle | The game's details |
| Square | Sort and filter, views, "Scan for games" |
| L2, R2 | The letter before, the letter after |
| Touch pad | Search |
| OPTIONS | The menu: Search, Scan for games, Memory cards, Start the BIOS; Settings, Update, About, Close PSSwanStation |

**Views.** Shelves (the game under the cursor large at the top, with shelves of lately played games, favourites and every game under it; Up and Down change shelf), Covers (a grid, with lately played games on a shelf above), List, and five that stand the covers in space as cases: Flow, Row, Wall, Cascade and Wheel. Aurora layout files (`.cfljson`) in `layouts/` are offered too. Choose a view under Square or in Settings, Interface, "Library view".

**Search.** Press the touch pad: type with the D-pad keyboard on the left (Cross types, Square deletes, Triangle is a space). Every game from all three places whose name has those letters is listed on the right as you type.

**Sort and filter (Square).** Order by name, last played, most played, year or size; show everything, favourites, games not played yet or hidden games; one region alone; and "Scan for games", which looks through all three places again.

**A game's details (Triangle).** Its description, maker and date from the game database, and how long you have played. Then:

- **Play**, from the disc chosen with Square when it has several;
- **Load state**, from a state saved earlier;
- **Options**, the emulator's settings for this game alone;
- **Cheats**, its cheats and patches;
- **More**: favourite, hidden, another cover (box, title screen or a moment of the game) and its texture pack.

Covers are looked for in `covers/` by the game's file name. With "Download covers" on, missing ones are fetched from the libretro thumbnails collection.

## Playing

The DualSense works as a PlayStation controller, button for button; the touch pad stands in for SELECT and START, and OPTIONS opens PSSwanStation's menu.

| Button | In a game |
| --- | --- |
| Touch pad, left half (press) | SELECT |
| Touch pad, right half (press) | START |
| OPTIONS, tapped | PSSwanStation's menu |
| OPTIONS held + R2 | Fast forward |
| OPTIONS held + L2 | Rewind (when switched on) |
| OPTIONS held + R1 / L1 | Save a state / load it |
| OPTIONS held + Left / Right | The slot before / after |

"Shortcuts" (Settings, Shortcuts and rewind) switches the held-OPTIONS shortcuts off; the menu then opens the moment OPTIONS is pressed. When the speedrun timer is on, OPTIONS held with Cross, Square, Triangle and Circle are its controls instead (see Extras).

**The menu (OPTIONS).** Resume, then three groups. The game: Save state, Load state, Change disc (a game on several discs), Cheats and patches, Game settings. Extras: Achievements (when RetroAchievements is on), Netplay, Speedrun timer, Find in memory, Shortcuts. PSSwanStation: Settings, Reset the game, Close the game. "Settings" changes a setting for every game; "Game settings" for this game only.

**Fast forward and rewind.** Fast forward runs at 2x to 8x, or as fast as the console manages (Settings, Shortcuts and rewind). Rewind is off at first. When on, it keeps the last while of play in memory (128 MB to 1 GB), never on the console's storage; it is gone when the game closes.

**More players.** Player 1 is whoever started PSSwanStation. A second person presses the PS button on their controller and chooses a user, and becomes player 2 within a couple of seconds. For three or four players, switch the Multitap on in Settings, Controllers: "In port 1" suits most four-player games. Leave it off otherwise, as some one- or two-player games don't see a controller behind a multitap.

**Closing.** Menu, Close ends the game. In the library, Circle asks whether to close PSSwanStation; Cross answers yes.

**Screenshots and video** are the console's own: use the Create button.

## Saving

Each game has ten save-state slots and its own memory card; both are kept in `data/`.

**Save states.** Menu, Save state and Load state, each slot shown with the picture it was saved at. With "Save when a game is closed" on (Settings, Games and network), one more state is written when you close a game, and "Continue where I left off" starts from it next time. A game's details (Triangle) can still start it from the beginning.

**Sleep-safe saving** (Settings, Games and network; off at first). While a game runs, its resume state is saved every five minutes of play, and whenever the console's own menu (the PS button) opens over it, quietly and without a pause. If the console then closes PSSwanStation (rest mode, or closing it from that menu), the next start offers to continue. It uses the same state as "Save when a game is closed". If PSSwanStation stops when a game starts with this on, switch it off: it has not run on a console yet.

**Memory cards** work as on the console, one for each game. Menu, "Memory cards" (with no game running) shows every card and the saves on it, with their pictures. There you can:

- copy a save to another card, or delete it;
- put a save out as a file (`memcards/export/`, `.mcs`);
- bring in whole cards (`.mcd`, `.mcr`, `.mc`, `.gme`, `.vmp` and other raw images) and single saves (`.mcs`, `.psx`, `.psv` and others) from `memcards/import/`.

**Card backups.** When a game closes and its card has changed, a copy is kept: the last ten of each card (Settings, Games and network, "Card backups"). The manager can go back to one.

## The picture

Games are drawn at 8x the PlayStation's resolution at first. Lower it under Settings, Enhancement, "Internal Resolution Scale" if a game does not hold its speed; "Show frame rate" (Interface) tells you. Every Picture setting except Display output can be a game's own (its details, Options; or Game settings, Picture while it runs).

| Setting (Settings, Picture) | Choices |
| --- | --- |
| Picture preset | Original (its own resolution, with a picture tube's lines), Sharp (8x, full colour, PGXP), Enhanced (Sharp with xBR textures and 4x MSAA), Speedrun (the picture as the PlayStation drew it) |
| Picture size | Fit the screen, whole multiples, or stretch |
| Scaling filter | Smooth, square pixels, sharp bilinear, FSR 1, NIS, CAS; "Sharpening" for the last three |
| Picture tube | Scanlines (soft or full), a shadow mask, or crt-guest-advanced in five kinds; and presets brought in from USB |
| Video signal | As it is, dither smoothed, S-Video or composite |
| Brightness, Contrast, Colour, Gamma | 50% to 150% |
| Beside the picture | Black, the picture's own light, a gradient, or a picture from `borders/` |
| Frame pacing | By the display, the game's own speed, or the game's own speed by the clock (for variable refresh rate) |
| Display output | 60 Hz, 120 Hz, or 120 Hz with variable refresh rate; from the next start |
| Black frame insertion | At 120 Hz, a black refresh between a 60 fps game's frames |
| Frame generation | Pictures made between the game's, with quality, mode and rate settings |

**Scaling filters.** Smooth blends neighbouring pixels; square pixels shows them as squares; sharp bilinear keeps pixels even and crisp at any size. FSR 1 and NIS follow edges, then sharpen; CAS sharpens fine detail without halos. FSR and NIS pay off most at a lower internal resolution (3x or 4x). A picture larger than the screen (8x on a 1080p screen) is made smaller by averaging, whichever filter is chosen.

**Gaps between polygons.** Above 1x, "Close Gaps Between Polygons" (Settings, Enhancement; on at first) covers the hairline gaps where a game's polygons meet unevenly: the sparkles along their edges. 3D objects' outlines become a quarter of a pixel fuller; 2D pictures and see-through polygons are left as they are. The Speedrun preset switches it off.

**Shader presets from USB.** Settings, Picture, "Shader presets from USB" brings in a libretro slang preset (`.slangp`) from `PSSwanStation/shaders/` on a USB drive. It is compiled on the console and becomes one of the Picture tube's choices. Not every preset runs fast enough at 4K.

**120 Hz.** Display output is read when PSSwanStation starts. If the screen stays dark after choosing 120 Hz, start PSSwanStation holding L1 and R1, wait ten seconds, close it and start it again.

**Frame generation** (Off or On) makes pictures between the game's own: a 30 fps game moves at 60 on a 60 Hz screen, or up to 120 at 120 Hz. It never changes the game's speed or sound. Its settings:

- **Quality:** Performance, Balanced, or Quality (checks each movement from both pictures' side);
- **Mode:** between the game's last two pictures, or ahead of the latest (no waiting, more mistakes where movement turns);
- **Up to the screen's rate**, or up to 60 a second at 120 Hz;
- **Videos:** included, or left as they are;
- **Take back the delay:** one frame of run-ahead.

It makes nothing across a cut to another scene, starts again after a state is loaded, and is off while fast forwarding or rewinding. Expect a thin fringe where something moves across a background moving the other way.

## Extras

**Cheats and patches.** After the cheat database is fetched (Settings, Games and network), a game's cheats and patches are in its details and in the menu under Cheats, found by the disc's serial number. Cross switches one on or off; Left and Right choose a value where a cheat has one. What is on is kept for that game. A game the database has no codes for gets the libretro database's cheats instead; they may not fit your version. Your own codes go in `cheats/<serial>.cht`:

```
[Infinite Health]
Type = Gameshark
Activation = EndFrame
800B7526 03E7
```

**Find in memory** (menu, while a game runs). The classic cheat search: look for a value (a byte, two or four), or one that went up, down, changed or stayed the same, and narrow it down while you play. What is left can be watched over the game, set, frozen at a value, or made a cheat in the game's own cheat file. Not available in RetroAchievements' hardcore mode.

**Speedrun timer** (menu, "Speedrun timer"). A timer at the top right, in game time (pauses and loading don't count) or real time. OPTIONS held with Cross splits (and starts), with Square takes the last split back, with Triangle stops and resets. Splits can trigger by themselves when a value in the game's memory becomes a number (found with Find in memory), and the timer can start the same way. The best run and best splits are kept for each game; a run with fast forward, rewind or a state load counts as practice.

**Recordings** (Settings, Debug). "Record what I press" keeps every player's buttons, frame by frame, from a state taken when it starts. "Play back a recording" starts the game from that state and runs it the same way again: to show a run, or to bring back the moment something went wrong. Playback needs the same settings, cheats and build.

**Texture packs.** Replacement 2D art in the vram-write format (files named `vram-write-<number>.png`) goes in `textures/<serial>/`, or in `PSSwanStation/textures/<serial>/` on a USB drive. Switch on "Enable VRAM Write Texture Replacement" (Enhancement). Packs that replace a 3D game's textures are not supported by this emulator.

**RetroAchievements.** Settings, RetroAchievements: switch it on and sign in with your retroachievements.org account. The password is typed once; only the token the server returns is kept. Hardcore mode earns more, and switches off state loading, cheats and rewind.

**Netplay** (an experiment). Two consoles, one game, players 1 and 2: start the game, then menu, Netplay. One console hosts and shows its address; the other chooses "Join a game" and types it (port 28800). Both need the same disc, BIOS and build. Memory cards, cheats, states, fast forward and rewind are out while it lasts. It has only run between two PCs.

**Phone and web control** (Settings, Games and network). Switch it on and scan the code with a phone, or type the address on a computer on the same network (port 3311). The page starts and closes games, resets, saves and loads states, runs the speedrun timer, changes some settings, and lets you put discs and covers in or take saves out. The address carries a key; "Make a new key" shuts out every device that had the old one.

## Settings reference

Settings (in the menu) holds PSSwanStation's own sections and every setting of the emulator. L1 and R1 move between sections; Square puts a setting back to its default. A dot marks a value that is a game's own, set under Game settings or in its details.

| Section | What it holds |
| --- | --- |
| Interface | Library: view, cover downloads, clock, update check. Look: theme, accent colour, size (up to 140%), high contrast, colour-blind safe colours, animations. The swan: lively, calm or still; dressed for the theme; seasonal touches; the start-up animation; the swan screen saver. Buttons and notices: confirm button, frame rate, console notices |
| Picture | Presets, picture size, scaling filter and sharpening, picture tube, shader presets from USB, video signal, colours, beside the picture, frame pacing, display output, black frame insertion, frame generation |
| Sound | The game's volume; the menus' sound set, interface sounds volume and start-up sound; menu music (the kit's songs, or your own .ogg files in `music/`) and its volume |
| Controllers | Players: who holds a pad, multitap, each player's controller type. The pad: buttons (remapping, turbo), stick dead zone, vibration, player lights, tilt steering |
| Shortcuts and rewind | The held-OPTIONS shortcuts, fast forward speed, rewind and its memory |
| Games and network | Where the games are: folders, USB drives, the network share, games into memory, Wake-on-LAN. Saving: save when closed, continue where I left off, sleep-safe saving, card backups. My files: where they are kept, memory cards and covers on the share. More: phone and web control, the cheat database |
| RetroAchievements | Sign in, hardcore mode |
| Console, Enhancement, Display, Port, Advanced | The emulator's own settings: CPU and BIOS options, internal resolution, texture filtering, PGXP, Close Gaps Between Polygons, widescreen hack, texture replacement and the rest |
| Debug | Verbose logging, recordings of what you press |
| About | Build number and date, licences |

Display output is the one Picture setting that cannot be a game's own: the console reads it when PSSwanStation starts.

## Troubleshooting

Two logs say what happened: `psswanstation-boot.log` in the title's folder (written from the first moment of every start, with the two starts before kept as `.1.log` and `.2.log`) and `logs/psswanstation.log`, the emulator's own.

| Problem | What to do |
| --- | --- |
| Black screen after choosing the icon | Look for `psswanstation-boot.log`. None means the title did not start at all: check kstuff, and the folder's place and name |
| Dark screen after choosing 120 Hz | Start holding L1 and R1, wait ten seconds, close it and start it again |
| PSSwanStation stops when a game starts | Switch off sleep-safe saving; then try Safe start (hold L1 and R1) |
| A game crashes at once | Try CPU Execution Mode "Cached Interpreter" (Settings, Console) and GPU Renderer "Software" (Enhancement) |
| A game is too slow | Lower the Internal Resolution Scale (Enhancement); FSR 1 or NIS keeps it sharp |
| No USB tab | Switch on "USB drives" and restart; games go in `psx`, `ps1` or `playstation` at the top of the drive; a Lapy service or the ELF loader must be running |
| No Network tab | Check the `path` lines in `network.cfg` (server by IP address), then restart or press Square, "Scan for games" |
| Sparkles along polygon edges | Check "Close Gaps Between Polygons" is on (Enhancement); it only works above 1x |
| The picture stood still for a moment | The boot log has a line "the screen stood still for ..." after the lines saying what was being done |

**Reporting a problem.** Say what you did, what you saw, which build (top right of the library, or Settings, About) and which firmware. Copy the logs before starting the title again. Never attach game files, BIOS files, firmware or keys, and please leave game titles out: "a 30 fps racing game" is enough.
