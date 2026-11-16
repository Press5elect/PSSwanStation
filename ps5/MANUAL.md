# PSSwanStation Manual

For build 27 · November 2026

## About PSSwanStation

PSSwanStation plays PlayStation (PS1) games on a jailbroken PS5, with an interface made for the controller. Inside it is SwanStation, the libretro fork of DuckStation; it draws with Vulkan through the RADV driver built into the title. This manual covers build 27.

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

**Updating.** Copying a newer build over the old folder keeps your games, saves, states, covers and settings: the ZIP holds only the program's files. From the title itself, Menu, "Update" asks the project's GitHub releases for a newer build, shows what changed, fetches it, checks it and replaces the program's files. PSSwanStation closes when it is done; start it again. If an update stops half way, the next start puts the earlier files back. With "Check For Updates" (Settings, Library) the library's header tells you when a build is out.

**Install again.** When you already have the newest build, Square on the Update page fetches it again and puts it in place, which mends a damaged title folder; your games, saves and settings stay.

**From build 17 or earlier, copy the folder.** The updater in builds 14 to 17 downloads and checks a new build, then stops with "The unpacked update is no longer there" and changes nothing. Copy the new `PPSA99248` folder over the old one by FTP instead; from build 18 on, the updater works.

**What's new.** The first start after an update, by the updater or by copying the folder over, opens a list of what was added, changed and fixed since the build you had, build by build, with a count of each at the top. Up and Down or a stick scroll it, L1 and R1 a page at a time, and Cross closes it. Settings, System, About, "What's new" shows every build's changes again. A first install skips it.

**Safe start.** Hold L1 and R1 while PSSwanStation starts. The display goes back to 60 Hz and HDR off for the next start, and a page offers to turn off leaving the sandbox (USB drives, the disc drive, USB controllers and files kept outside), or to put the interface's or the emulator's settings back to the usual. If a start never reaches the library, the next start does this by itself, once, and says so.

**Cheats and patches, once.** A release does not carry the cheat database. Settings, Network, "Update Cheat Database" downloads it from its project when you want it.

## Your files

**Folders of your choice** (Settings, Library, Folders; after Crux2's). *Game Folders* adds folders of games anywhere on the console's storage or a USB drive: each is searched with the folders inside it, and the games stay where they are (a folder on a USB drive shows under the USB tab, the others under Internal; Square takes one off the list). *BIOS*, *Memory Cards*, *Save States*, *Cheats*, *Covers* and *Texture Packs Folder* each take a folder of your own instead of the usual place below, picked with a folder browser (the title's folder, the folder for your files, `/data` and each USB drive); Square goes back to the usual place. A folder PSSwanStation writes to is tried for writing first. The BIOS, memory card and texture pack folders are used from the next start; the others at once. Files already there are not moved. The choices are kept in `folders.cfg` in the title folder.

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
| `music/` | Your own menu music: any `.ogg`, `.mp3` or `.wav` files, played shuffled with Menu Music "My music folder" (Audio) |
| `mods/`, `themes/`, `media/` | Mods for games; ES-DE themes; ES-DE's pictures and videos |
| `memcards/` | `import/` for cards and saves to bring in, `export/` for saves put out |
| `network.cfg` | The network shares or FTP servers to read games from |
| `data/` | Memory cards, their backups, save states and settings (written by the title) |
| `logs/` | The emulator's log of the last two runs |

**Game formats:** `.chd`, `.cue` with its `.bin` files, `.iso`, `.img`, `.pbp`, `.ecm`, `.mds`, and PlayStation programs (`.exe`, `.psexe`). A game on several discs shows once, and its discs share one memory card ("Group Multi-Disc Games", Settings, Library, View; on). Its discs are found by their names, "... (Disc 1)", "... [CD 2]", "... (Disc 2 of 3)", or "... CD2" or "... Disc 2" at the end, in one folder, each in a folder named after the disc, or in folders named only `CD1`, `CD2` inside the game's folder. Change disc in the menu over the game.

**USB drives.** Switch on "Scan USB Drives" in Settings, Library; it takes effect at the next start. It needs the ELF loader listening on port 9021. Games are read from a folder named `psx`, `ps1` or `playstation` at the top of the drive.

### Network shares: network.cfg

PSSwanStation reads games from SMB shares (Windows sharing), NFS exports and FTP servers named in `network.cfg`. The file is in the title's folder (or wherever "Data Location" puts your files). The title writes it at its first start, with every line explained and the example lines switched off.

**From the console.** Settings, Library, "Network Shares" fills in the file for you: add, change or remove the folders with games, set the account and the folder for your files. As the page opens, the console looks for the SMB servers on its network (every address of its /24 asked on port 445, with the NetBIOS name of each that gives one); choose one and type the share and folder after its address. NFS and FTP folders are added by hand in the same list. A change is used at once: the Network tab is scanned again, and the next request to a share logs in with the new account. The file's comments and the lines the page does not show (`domain`, `wake`) are kept.

**The rules of the file**

- One setting per line, as `name = value`. A line starting with `#` is switched off: remove the `#` to use it.
- Give every server by its **IP address** (`192.168.1.10`), never its name. Names are refused.
- Restart PSSwanStation after editing it by hand (the page above needs no restart). The folders are scanned at that start; the list of games is kept, and Square, "Scan For Games" scans again.

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

**When it doesn't work**, the library's bottom line says why: a name instead of an IP address, a refused account (check `user`, `password` and `domain`), or a server that did not answer (switch it on, or set `wake`). With no games found there is no Network tab. A network game is read into memory before it starts; switch off "Preload Network Images To RAM" (Settings, Console, Disc) to start at once and read while playing. As PSSwanStation starts, each share with games is read a little in the background (the game played last from it, else the first large file on it), so that a NAS's disks are already turning when you start a game. A game whose server does not answer when you start it (the network still coming up, a NAS waking) waits for it, the seconds counted on the screen, and starts as soon as the server answers; Circle goes back to the games. A server that answers but refuses the account says so at once.

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
4. In Settings, Saving, Storage, choose what goes there (next table): switch on "Sync Memory Cards" and "Sync Covers", or set "Data Location" to the network share for everything.
5. Do the same on each console. Choose "Sync Now" on the first console, so the NAS has its cards before the others look.

**What is kept there**

| What | With "Sync Memory Cards" and "Sync Covers" | With "Data Location" set to the network share |
| --- | --- | --- |
| Memory cards | Both ways, shared between consoles (`memory cards/`) | Both ways |
| Covers | Both ways (`covers/`): downloaded or chosen on one console, on every console | Both ways |
| Memory Card Backups | Sent to `card backups/<console>/`, a copy off each console | Sent the same way |
| Save states, settings, cheats, BIOS, textures, music, borders, layouts | Stay on each console | Both ways |

The NAS is brought up to date when PSSwanStation starts, when a game closes, and when you choose "Sync Now"; never while a game runs. With "Data Location" on the network share, the console works on its own copy, so games still start, play and save when the NAS is off. Closing PSSwanStation while it is being brought up to date first finishes the file being copied, a bar along the bottom saying which, and the rest is brought up to date at the next start.

**Two consoles, one game.** If two consoles played the same game on their own copy of a card before they met, the one played last is kept and the other is saved beside it as `<card>.conflict-<date>-<time>`, so nothing is lost. A card or cover removed on one console is removed on the others; the console keeps its copy in `data/sync/removed/`.

**Card backups.** Each console sends the backups it keeps (the last ten of each card, Settings, Console, "Memory Card Backups") to a folder of its own, so consoles never write over each other's. The NAS follows the console: a backup the console trims goes from the NAS too. Nothing comes back from that folder by itself. To restore a backup on a new or reset console, copy it from `card backups/<console>/<card>/` into `data/saves/backups/<card>/`, then go back to it in the memory card manager.

**Naming your consoles.** A console's name is made at its first sync, as `console-` and six letters, and kept in `data/console-name.txt`. Change it there (letters, digits, `-` and `_`), for example `living-room`, to tell the folders apart; it is never copied to the NAS or to other consoles.

**Other places for your files.** "Data Location" can also choose `/data/psswanstation/` on the console, or `PSSwanStation/` on a USB drive, so that replacing or deleting the title's folder can't take your files along. Every place but the title's folder needs the ELF loader on port 9021, as USB drives do.

## Discs from a USB drive

PSSwanStation plays your PlayStation discs from a USB CD or DVD drive plugged into the console. The console's own drive cannot read CDs, and every PlayStation disc is one.

1. The drive is watched whenever PSSwanStation leaves its sandbox for USB: with "Scan USB Drives" (Settings, Library) or "USB Disc Drive" (Settings, Console) on (it takes effect at the next start). Leaving the sandbox needs the ELF loader on port 9021.
2. Put the disc in the drive. A few seconds later it is under the library's **Disc Drive** tab, named from its serial (the game database's name when it knows the disc). A drive with no disc, or with one it cannot read, is listed too ("USB disc drive - no disc"); choosing it says why.
3. Press Cross. PSSwanStation asks how to play it:

| Choice | What it does |
| --- | --- |
| Copy it, then play the copy | Reads the whole disc, as fast as the drive goes, and writes it as a `.bin` and `.cue` to the place you pick: the games folder, a USB drive (its `psx` folder), the SMB share or the NFS share (the first games folder of that kind in `network.cfg` that can be written). A LibCrypt-protected disc also gets an `.sbi`. The copy is then in the library and plays without the disc |
| Copy it into memory and play it | Reads the whole disc into memory first (a minute or two). The game then never waits for the drive, and the memory is given back when the game closes |
| Play it from the drive | Starts at once. Behind the game the whole disc is read into memory, as fast as the drive goes (a few minutes for a full CD; the boot log says when it is done), and the drive then rests. Until then a jump to another part of the disc takes the drive a fraction of a second, which the game waits for as it would for a PlayStation's own drive while the picture and sound go on. The memory is given back when the game closes |

**Turn the console on with the drive empty.** Put the disc in after the console has started. A console started with a disc in a USB drive locks the tray before any homebrew runs, and later it cannot turn itself off: Turn Off PS5 never finishes, and only holding the power button turns it off. PSSwanStation unlocks the tray as soon as it sees the drive, so the drive's own button works again, but it cannot undo the console's trouble turning off.

**Eject.** The drive's page has "Eject the disc", which opens the tray.

Copying shows how far it has got; Circle stops it. A copy never replaces a file: a second copy of the same disc gets " (2)" after its name.

**How the drive reads.** The best is whole sectors with their sub-channel, which is everything a PlayStation disc holds: videos, XA sound, CD music and LibCrypt's protection. That needs the console to let PSSwanStation send the drive SCSI commands. If it does not, the disc is read 2048 bytes a sector: games start, but their videos, XA sound and CD music are lost, and so are copies made that way. The choice page and the boot log (`disc drive:` lines) say which way the drive is read.

## The library

The library shows your games from four places, each a tab once it has games: Internal (the `games` folder), USB, Network and Disc Drive (a disc in a USB CD or DVD drive). L1 and R1 change tab.

| Button | In the library |
| --- | --- |
| D-pad, left stick | Move |
| Cross | Play (Cross and Circle can be swapped in Settings) |
| Circle | Asks whether to close PSSwanStation |
| Triangle | The game's details |
| Square | Sort and filter, views, "Scan For Games" |
| L2, R2 | The letter before, the letter after |
| Touch pad | Search |
| OPTIONS | The menu: Settings, Search, Memory cards, Scan for games, Start the BIOS; Update, About, Close PSSwanStation |

**Views.** Shelves (the game under the cursor large at the top, with shelves of lately played games, favourites and every game under it; Up and Down change shelf), Covers (a grid, with lately played games on a shelf above), List, and five that stand the covers in space as cases: Flow, Row, Wall, Cascade and Wheel. Aurora layout files (`.cfljson`) in `layouts/` are offered too. Choose a view under Square or in Settings, Library, "Library View".

**Game videos.** When the cursor stays on a game for a moment (2 seconds at first), its video plays: in the cover's place in Shelves, List and the game's details, and behind the covers, as the page's backdrop, in Covers and the views in space. The menu music is lowered meanwhile. Settings, Library, Videos switches them on and off and sets their sound and the wait; Square has the switch too. The videos are the ones ES-DE themes use (below): `videos/<file name>.mp4` in a games folder, `media/videos`, or ES-DE's `downloaded_media/psx/videos`, on the console, a USB drive or a network share.

**Search.** Press the touch pad: type with the D-pad keyboard on the left (Cross types, Square deletes, Triangle is a space). Every game from every place whose name has those letters is listed on the right as you type.

**Sort and filter (Square).** Order by name, last played, most played, year or size; show everything, favourites, games not played yet or hidden games; one region alone; and "Scan For Games", which looks through every place again.

**A game's details (Triangle).** Its description, maker and date from the game database, and how long you have played. Then:

- **Play**, from the disc chosen with Square when it has several;
- **Load state**, from a state saved earlier;
- **Options**, the emulator's settings for this game alone;
- **Cheats**, its cheats and patches;
- **More**: favourite, hidden, another cover (box, title screen or a moment of the game) and its texture pack.

Covers are looked for in `covers/` by the game's file name. With "Download Covers" on, missing ones are fetched from the libretro thumbnails collection, and a game it does not have from xlenore's psx-covers collection by the disc's serial (the serial the game ran under, or the one the game database gives its file name; saved as `<file name>.jpg`): the games on screen first, then every other game of the library in its order, one after another in the background (not while a game runs), so that scrolling finds them there. The library's header counts them ("Downloading covers (game 12 of 340)").

### ES-DE themes

Themes made for [ES-DE](https://es-de.org) (EmulationStation Desktop Edition) can draw the whole library: the theme's gamelist view for the PlayStation, with your games in its list (a text list, a grid or a carousel), their pictures and details where the theme puts them, its fonts, colours and help line. The library's buttons stay the same (Cross plays, Triangle opens the details, Square sorts and filters, L1 and R1 change tab, L2 and R2 jump by letter).

- **Getting themes.** Settings, Library, "ES-DE themes", "Get themes" lists the themes ES-DE's own theme downloader offers (the list ES-DE keeps on GitLab) and fetches the one you choose from its author's repository into `themes/`. Or copy a theme's folder (the one with `capabilities.xml` and `theme.xml` in it) into `themes/`, or into `PSSwanStation/themes` on a USB drive. Themes are large, some hundreds of megabytes; videos and sounds in them are left out when fetched.
- **Choosing one.** Square, Library View (each theme is "ES-DE: " and its name, after the other views), or Settings, Library, "Library View". "ES-DE Themes" (also under Square, "ES-DE Theme Options") chooses the theme's variant, colour scheme, font size, aspect ratio (automatic: the display's) and language, as ES-DE's own menu does. A variant that shows pictures your games do not have gives way to the one the theme names for that, as in ES-DE.
- **Pictures and videos.** A theme shows the kinds of picture ES-DE scrapes, in ES-DE's folders, each file named after the game's file: `covers`, `screenshots`, `titlescreens`, `marquees`, `miximages`, `3dboxes`, `backcovers`, `physicalmedia`, `fanart` (`.png` or `.jpg`) and `videos` (`.mp4`). They are looked for wherever they are kept:
  - on the console: `media/`, `media/psx/` or `downloaded_media/psx/` in the title's folder (ES-DE's `downloaded_media/psx` copies in as it is);
  - on a USB drive: the same under `PSSwanStation/`;
  - in every games folder: the console's `games`, each USB drive's games folder, and every network folder in `network.cfg` (SMB, NFS or FTP): the kinds' folders straight inside it (`smb://nas/roms/psx/videos`, `.../covers`), `media/`, `media/psx/` or `downloaded_media/psx/` inside it, or `downloaded_media/psx/` beside it, as ES-DE lays out its ROMs and media side by side on a NAS; and Batocera's `images/` folder (`<game>-thumb.png`, `-marquee`, `-image`) with its `videos/<game>-video.mp4`.

  The folders are listed once in the background (Settings, Library, ES-DE Themes, "Pictures and videos found" says how many files), again when the library changes and every few minutes, so a share is not asked for each picture. Pictures and videos on a share are read straight from it. A game without a cover there shows its library cover; with "Download Covers" on, title screens and screenshots are fetched from the libretro thumbnails collection too.
- **Videos.** A theme's video plays the game's video (ES-DE's `videos` folder, from any of the places above: the console, a USB drive or a network share), once the cursor has stayed on the game for the theme's delay; its picture shows until then, and after the video when the theme says so. PSSwanStation decodes them itself with FFmpeg (MP4 files with H.264 pictures and AAC or MP3 sound, 8-bit 4:2:0, as scrapers make them), because the console refuses a homebrew title its own video player. The sound plays under the menu music, which is lowered meanwhile. "ES-DE themes" has a switch for the videos and one for their sound. A video that cannot be played leaves the picture in its place, and the boot log's `video:` lines say why.
- **What is shown.** Pictures (PNG, JPEG and SVG), texts, the game's name, developer, publisher, genre, players, description (scrolling as in ES-DE) and release date from the game database, when it was last played, the number of games, the favourite badge, a clock, and the help line with the library's buttons. Animations are not played. Ratings are not shown (the game database has none). The system view (ES-DE's carousel of systems) is not used: the library opens on the PlayStation's games.
- **If a theme cannot be used** (a theme for an older EmulationStation, without `capabilities.xml`, or one that cannot be read), "ES-DE themes" says why, and the library uses its own view. The boot log's `themes:` lines say what was read.

Each theme is its author's work, under its own licence (many are for non-commercial use only). Themes are not part of PSSwanStation and are never in its releases.

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

"Hotkeys" (Settings, Controllers, Hotkeys) switches the held-OPTIONS shortcuts off; the menu then opens the moment OPTIONS is pressed.

**Button Mapping** (Settings, Controllers, Pad) remaps them: which button of the pad presses each of the PlayStation's, OPTIONS included, and which fire by themselves while held (turbo). "The menu opens with" there gives the menu (and the shortcuts' key) to OPTIONS, to either half of the touch pad, or to OPTIONS held for a second. The button that opens the menu is not the game's, so to make OPTIONS the PlayStation's START, first give the menu to a half of the touch pad; with "OPTIONS, held for a second" OPTIONS is both (a tap is the game's, held it opens the menu) and there are no shortcuts. Shortcuts, in the menu, shows the buttons as they are set. When the speedrun timer is on, OPTIONS held with Cross, Square, Triangle and Circle are its controls instead (see Extras).

**The menu (OPTIONS).** The game stays in view. At the left: Resume Game, then three groups. Game: Save State, Load State, Change Disc (a game on several discs), Cheats and Patches, Game Settings, Original Look. Extras: Achievements (when RetroAchievements is on), Netplay, Speedrun Timer, Memory Search, Hotkey List. PSSwanStation: Settings, Reset Game, Close Game. "Settings" changes a setting for every game; "Game Settings" for this game only. Under the game, the pictures of its newest save states. At the right, the quick settings (the ones pinned on the settings board with R3): R1 moves there to change one with Left and Right, L1 goes back to the actions.

**Fast forward and rewind.** Fast forward runs at 2x to 8x, or as fast as the console manages (Settings, Controllers, Hotkeys). Rewind (Settings, Saving, Rewind) is off at first. When on, it keeps the last while of play in memory (128 MB to 1 GB), never on the console's storage; it is gone when the game closes.

**The PlayStation Mouse** (Settings, Controllers, Ports, a player's controller), for the games made for it: slide a finger on the touch pad to move it, or use the right stick; the touch pad pressed down, Cross or R1 is its left button, Circle or L1 its right.

**More players.** Player 1 is whoever started PSSwanStation. A second person presses the PS button on their controller and chooses a user, and becomes player 2 within a couple of seconds. For three or four players, switch the Multitap on in Settings, Controllers, Ports: "In port 1" suits most four-player games. Leave it off otherwise, as some one- or two-player games don't see a controller behind a multitap.

**The Sinden Lightgun** (an experiment; Settings, Controllers, Lightgun, "Sinden Lightgun", off at first) aims player 1's or player 2's GunCon (set that player's controller to the GunCon). PSSwanStation draws a white border round the game's picture ("Sinden Border Size", 1-8% of the screen's height; the picture is made smaller, so none of it is covered), reads the gun's camera over USB and finds the border itself, without Sinden's software. The trigger fires, the second button is the GunCon's A and the third its B; a shot off the screen reloads. Needs PSSwanStation to leave its sandbox; from the next start. Not yet tried on a console: the boot log's `sinden:` lines say what the console and the gun did.

**USB Controllers** (an experiment; Settings, Controllers, USB, "USB Controllers", off at first). PSSwanStation reads some controllers plugged into the console's USB ports itself: a PS3 pad, a DualShock 3 on a cable, an Xbox 360 pad or another USB gamepad. They can be the next player or play together with player 1. Only devices that say what they are are read; Sony's controllers are left to the console. Not yet tried on my console.

**Closing.** Menu, Close ends the game. In the library, Circle asks whether to close PSSwanStation; Cross answers yes. If your files are being brought up to date with a network share or the cloud just then, PSSwanStation first finishes the file it is copying (a bar along the bottom says which; no button cuts that short, so no file is left half copied) and leaves the rest for the next start.

**Screenshots and video** are the console's own: use the Create button.

## Saving

Each game has ten save-state slots and its own memory card; both are kept in `data/` (the states on a USB drive with "Save States On USB Drive").

**Save states.** Menu, Save state and Load state, each slot shown with the picture it was saved at. With "Save State On Exit" on (Settings, Saving, Save States), one more state is written when you close a game, and "Load Resume State On Start" starts from it next time. A game's details (Triangle) can still start it from the beginning.

**Save State On Console Menu** (Settings, Saving, Save States; off at first). While a game runs, its resume state is saved whenever the console's own menu (the PS button) opens over it, quietly and without a pause (not on a timer: the menu is where the console closes a title, and a timer only wore the storage). If the console then closes PSSwanStation (rest mode, or closing it from that menu), the next start offers to continue. It uses the same state as "Save State On Exit". If PSSwanStation stops when a game starts with this on, switch it off: it has not run on a console yet.

**Memory cards** work as on the console, one for each game. Menu, "Memory cards" (with no game running) shows every card and the saves on it, with their pictures. There you can:

- copy a save to another card, or delete it;
- put a save out as a file (`memcards/export/`, `.mcs`);
- bring in whole cards (`.mcd`, `.mcr`, `.mc`, `.gme`, `.vmp` and other raw images) and single saves (`.mcs`, `.psx`, `.psv` and others) from `memcards/import/`.

**Card backups.** When a game closes and its card has changed, a copy is kept: the last ten of each card (Settings, Console, "Memory Card Backups"). The manager can go back to one.

**Cloud Saves** (Settings, Saving, Storage). The memory cards, and the save states if you switch "Save states too" on, kept in your Google Drive or Dropbox as well: brought here when PSSwanStation starts, sent there when a game closes, so another console (or this one after a reinstall) has them. A card played in two places keeps the one played last and the other beside it (`.conflict-<date>`). Signing in takes a phone, and no password is typed on the console:

- **Google Drive**: the console shows a code and a QR code; open google.com/device, sign in and type the code. PSSwanStation gets a folder of its own in your Drive, hidden from your files, and nothing else.
- **Dropbox**: scan the QR code, sign in and allow PSSwanStation; Dropbox shows a code, which you type on the console or paste in Remote Control (Settings tab, "Dropbox code"). PSSwanStation gets `Apps/PSSwanStation` and nothing else.

The console keeps only the sign-in (in `data/cloud.cfg`, never given out by the web panel); Sign out forgets it, and for Google withdraws it there too.

## The picture

At first a game looks and runs as on the PlayStation (the Original preset): drawn by the software renderer at the console's own resolution, with its colours, dithering and interlacing, its timing and speed (the CPU's instruction cache included), the edges a television hid cropped, and nothing added. Enhancements are yours to choose: a preset below, or each setting under Settings, Graphics and Display. "Differences From Original" (Display, Preset, and "Original Look" in the menu over a game) lists every setting that is not as the console had it, to put back one by one or all at once. An install from before build 25 keeps the settings it had. "Show Frame Rate" (Display, Output) tells you whether a game holds its speed. Every Display setting except Display Mode and HDR Output can be a game's own (its details, Game Settings; or Game Settings in the menu while it runs).

| Setting (Settings, Display) | Choices |
| --- | --- |
| Picture Preset | Original (as the PlayStation drew it: the software renderer, its resolution, colours, dithering, interlacing, timing and speed, a TV's cropped edges; at first), Original on a TV (the same through a composite signal and a curved picture tube), Fast (2x, PGXP geometry), Normal (4x, full colour, PGXP with straight textures, gaps closed), Enhanced (6x), Beautiful (8x, JINC2 textures, CAS sharpening), Speedrun (as the PlayStation drew it, square pixels, nothing that changes its timing). The enhanced presets smooth edges by resolution, not MSAA, which would turn accurate blending off |
| Differences From Original | Every setting in force that is not as the PlayStation had it, each put back by itself or all at once (also "Original Look" in the menu over a game) |
| Display Scaling | Fit the screen, whole multiples, or stretch |
| Scaling Filter | Smooth, square pixels, sharp bilinear, FSR 1, NIS, CAS, MMPX; "Sharpening Strength" for FSR 1, NIS and CAS |
| CRT Shader | Scanlines (soft or full), a shadow mask, or crt-guest-advanced in five kinds; and presets brought in from USB |
| Video Signal Simulation | As it is, dither smoothed, S-Video or composite |
| Brightness, Contrast, Saturation, Gamma | 50% to 150% |
| Film Grain | Off, Light, Medium or Strong: a fine grain over the picture that moves every frame, strongest in the middle tones |
| Border Fill | Black, the picture's own light (the picture blurred over the whole screen), a gradient, a picture from `borders/`, or the picture carried on past its edges, blurred |
| Frame Pacing | By the display, the game's own speed, or the game's own speed by the clock (for variable refresh rate) |
| Display Mode | 60 Hz, 120 Hz, or 120 Hz with variable refresh rate; from the next start |
| Black Frame Insertion | At 120 Hz, a black refresh between a 60 fps game's frames |
| HDR Output | Disabled (as it always was), or the picture sent to the TV as HDR10; from the next start. With it: HDR Paper White, HDR Peak Brightness and Calibrate HDR |
| Frame Generation | Pictures made between the game's, with a generation method, quality, mode and rate settings |
| Generation method (under Frame Generation) | The title's own (the default), AMD FSR3's optical flow, from AMD's FidelityFX SDK, or MihawkGen, Mihawk's small trained network |

**Scaling Filters.** Smooth blends neighbouring pixels; square pixels shows them as squares; sharp bilinear keeps pixels even and crisp at any size. FSR 1 and NIS follow edges, then sharpen; CAS sharpens fine detail without halos. FSR and NIS pay off most at a lower internal resolution (3x or 4x). MMPX is for pixel art at the PlayStation's own resolution (Internal Resolution Scale 1x, or the software renderer): it doubles each pixel the way an artist draws pixel art larger, keeping lines one colour and corners sharp, then grows it with sharp bilinear. A picture larger than the screen (8x on a 1080p screen) is made smaller by averaging, whichever filter is chosen.

**Gaps between polygons.** Above 1x, "Close Gaps Between Polygons" (Settings, Graphics, Geometry; off at first, on in the Normal, Enhanced and Beautiful presets) covers the hairline gaps where a game's polygons meet unevenly: the sparkles along their edges. 3D objects' outlines become a quarter of a pixel fuller; 2D pictures and see-through polygons are left as they are. The Speedrun preset switches it off.

**Shader presets from USB.** Settings, Display, "Import Shader Presets" brings in a libretro slang preset (`.slangp`) from `PSSwanStation/shaders/` on a USB drive. It is compiled on the console and becomes one of the CRT Shader's choices. Not every preset runs fast enough at 4K.

**120 Hz.** Display Mode is read when PSSwanStation starts. If the screen stays dark after choosing 120 Hz, start PSSwanStation holding L1 and R1, wait ten seconds, close it and start it again.

**HDR Output** (Settings, Display, Output; disabled at first, as OGBox has it) sends the picture to an HDR TV as HDR10 (BT.2020 colours, the PQ curve). It needs the console's own HDR on (Settings, Screen and Video, Video Output) and a TV that takes HDR; otherwise the picture stays SDR, and the setting says why. Turned on, it begins at the next start: the console asks the TV for HDR as a title starts, and the TV stays in HDR while PSSwanStation runs, even if HDR is turned off meanwhile. White is shown at the HDR Paper White (203 nits, HDR's reference, at first), the menus too; HDR Peak Brightness raises the game's brightest parts toward the TV's peak (1000 nits at first; Off keeps the picture as an SDR screen shows it). Calibrate HDR finds both with two test pictures: an X in a bright square that disappears where its level reaches the TV's peak, then grey steps and white at the white level. A start after one that did not finish turns HDR off again. Screenshots while HDR is on are of the game's picture alone.

**Frame Generation** (Off or On) makes pictures between the game's own: a 30 fps game moves at 60 on a 60 Hz screen, or up to 120 at 120 Hz. It never changes the game's speed or sound. Its settings:

- **Generation method:** the title's own, which is the default, AMD FSR3's optical flow, or MihawkGen (a trained network of 0.43 million parameters that reads the game's two pictures alone; its quality, ahead and movement-view settings do not apply, and it makes none ahead). AMD FSR3's optical flow: on my test pictures its frames are about half as far from the true ones as the title's own. Its frames take the picture's look (film grain, colours, scaling filter, sharpening and picture tube) like the title's own;
- **Quality:** Performance, Balanced, or Quality (checks each movement from both pictures' side);
- **Mode:** between the game's last two pictures, or ahead of the latest (no waiting, more mistakes where movement turns);
- **Up to the screen's rate**, or up to 60 a second at 120 Hz;
- **Videos:** included, or left as they are;
- **Take back the delay:** one frame of run-ahead.

It makes nothing across a cut to another scene, starts again after a state is loaded, and is off while fast forwarding or rewinding. Expect a thin fringe where something moves across a background moving the other way.

## Extras

**Cheats and patches.** After the cheat database is fetched (Settings, Network, "Update Cheat Database"), a game's cheats and patches are in its details and in the menu under Cheats and patches, found by the disc's serial number. Cross switches one on or off; Left and Right choose a value where a cheat has one. What is on is kept for that game. A game the database has no codes for gets the libretro database's cheats instead; they may not fit your version. Your own GameShark codes can be typed in: "Add a GameShark code" at the end of the list asks for a name, then the code on a keypad (8 digits, a space and 4 more, a line for each code, a new line begun by itself after twelve digits). It is switched on at once and kept in `cheats/<serial>.cht`; Square on one of your codes edits it, and emptied it is removed. The file's form, for codes written on a computer:

```
[Infinite Health]
Type = Gameshark
Activation = EndFrame
800B7526 03E7
```

**Mods.** A mod changes a game a feature at a time, without changing its disc image. It is a text file: a folder with a `mod.txt` in it, or a `<name>.mod` file, in `mods/` (or `PSSwanStation/mods/` on a USB drive). A game's mods are under Cheats in its details, above the patches, each feature switched on and off by itself; what is on is kept for that game. Settings, Saving, "Mods" says how many were found and whether any could not be read (the boot log's `mods:` lines say which line). The format:

```
name = Faster start
version = 1.0
author = Someone
description = What the mod does.
games = SLUS-00594, SCES-00344

[feature]
name = Skip the logos
description = What this part does.
on = yes
patch = 80041234: 2a 00 02 24 > 00 00 00 00
code = 80041238 0000
setting = swanstation_GPU_WidescreenHack = false
```

A `patch` writes the new bytes only where the old ones are found, so a part of the game loaded later from the disc is patched when it arrives, and another version of the game is left alone; addresses are in the console's 2 MB of memory (80000000 to 801FFFFF), and the bytes are as many on each side. `code` lines are GameShark codes as they are, and `setting` lines are emulator options the feature needs while it is on. Mods are text: they carry no game data and can be shared. Like cheats, they are off in netplay and in RetroAchievements' hardcore mode.

**Use Widescreen Patches** (Settings, Graphics, Geometry). With the aspect ratio set to 16:9 or wider, or the widescreen hack on, a game that has a widescreen patch in the patch database uses it by itself: the game's own camera made wide, without the general hack's pop-in at the edges. Its Cheats page shows it as "On, by itself".

**Adaptive Resolution Scale** (Settings, Graphics, Renderer, off as it comes). While a game cannot keep its speed at the resolution scale chosen, the scale is lowered a step at a time, down to half of it, and raised again once the game has kept its speed for twenty seconds. A step costs a moment's hitch; a raise that does not hold is not tried again until the game starts anew. The boot log's `resolution:` lines say when.

**Memory Search** (menu, while a game runs). The classic cheat search: look for a value (a byte, two or four), or one that went up, down, changed or stayed the same, and narrow it down while you play. What is left can be watched over the game, set, frozen at a value, or made a cheat in the game's own cheat file. Not available in RetroAchievements' hardcore mode.

**Speedrun Timer** (menu, "Speedrun Timer"). A timer at the top right, in game time (pauses and loading don't count) or real time. OPTIONS held with Cross splits (and starts), with Square takes the last split back, with Triangle passes a split by, with Circle stops and resets. Splits can trigger by themselves when a value in the game's memory becomes a number (found with Find in memory), and the timer can start the same way. The best run and best splits are kept for each game; a run with fast forward, rewind or a state load counts as practice.

**Recordings** (Settings, System, Input Recording). "Record Input" keeps every player's buttons, frame by frame, from a state taken when it starts. "Play back a recording" starts the game from that state and runs it the same way again: to show a run, or to bring back the moment something went wrong. Playback needs the same settings, cheats and build.

**Texture packs.** A game's pack goes in `textures/<serial>/`, or in `PSSwanStation/textures/<serial>/` on a USB drive (pictures may be in folders inside it). Two kinds are read:

- **Texture blocks** (Vulkan renderer; "HD Texture Packs (Texture Blocks)", Graphics, Textures; off at first): the PlayStation's GPU reads textures through a cache that holds a texture page in blocks, 64x64 texels of a 4-bit texture, 32x64 of an 8-bit one, 32x32 of a 15-bit one. A picture named `block-<4|8|15>-<16 hex digits>.png` replaces the block whose hash those digits are (XXH3-64 of the block's 16-bit words, row by row, then its palette's 16 or 256), in 2D and 3D alike. Pictures may be larger than the block, up to 256 x 256, and are drawn smoothly; transparent pixels (alpha below one half) stay transparent. A primitive that reads more than 2 x 2 blocks, uses a texture window, or a texture the game drew itself keeps the original. To make a pack, switch on "Write Out Texture Blocks": each block a game shows is written once to `textures/<serial>/dump/`, already named; draw over it and move it up a folder.
- **Whole uploads** (`vram-write-<number>.png`): replacement art for a picture the game sends to video memory in one piece. Switch on "Enable VRAM Write Texture Replacement" (Graphics, Textures).

**RetroAchievements.** Settings, Network, RetroAchievements: switch it on and sign in with your retroachievements.org account. The password is typed once; only the token the server returns is kept. Hardcore mode earns more, and switches off state loading, cheats and rewind. The message at a game's start says which mode it is in ("Hardcore Mode" or "Softcore"). In hardcore mode a recording cannot be played back, and playback stops if hardcore comes on. An unlock made while the server cannot be reached is sent when it comes back; if the title is closed first, the unlock is kept in `data/achievements-unsent.txt` and sent after the next sign-in. What is sent, and what is kept, is in [PRIVACY.md](PRIVACY.md) (also in the title folder).

**Starting a game from a forwarder.** A PS5 forwarder (an icon on the home screen for one game), or anything else that starts the title, can name the game with launch arguments:

```
--rom "games/<folder>/<file>.chd" --exit-after-game
```

`--rom` takes a full path (`/data/homebrew/PPSA99248/games/...`, `/mnt/usb0/...`), a path from the title folder (or from `/data/psswanstation` when your files live there), or the game's name as the library shows it. The game must be in the library: the title waits for the library's first scan of a USB drive or share (a minute at most). With `--exit-after-game` the title closes when that game is closed, back to the home screen. A game that is not found says so (and, with `--exit-after-game`, the title closes once that message is closed). The boot log's `main:` and `forwarder:` lines say what was asked and found.

**Netplay** (an experiment). Two consoles, one game, players 1 and 2: start the game, then menu, Netplay. One console hosts and shows its address; the other chooses "Join a game" and types it (port 28800). Both need the same disc, BIOS and build. Memory cards, cheats, states, fast forward and rewind are out while it lasts. It has only run between two PCs.

**Remote Control** (Settings, Network, Remote). Switch it on and scan the code with a phone, or type the address on a computer on the same network (port 3311). The page starts and closes games, resets, saves and loads states, runs the speedrun timer, changes some settings, and lets you put discs and covers in or take saves out. The address carries a key; "Make a new key" shuts out every device that had the old one.

## Settings reference

Settings (first in the menu) opens on a board of ten areas: Console, Graphics, Display, Audio, Controllers, Library, Saving, Interface, Network and System. Each area is a tile with a light for each of its settings, lit when the setting is not at its default, so what you have changed is seen at a glance. Cross opens an area; inside it the sections run in the order the picture, the sound or a press travels (Graphics: Renderer, Geometry, Textures, Colour; Display: Preset, Frame, Scaling, Filters, Adjustments, Frame Generation, Output).

| Button | On the board, and in an area |
| --- | --- |
| D-pad, Cross | Choose an area and open it; in an area, choose a setting, Left and Right change it |
| L1, R1 | The area before or after |
| L2, R2 | The section before or after |
| Square | Back to the default (for a game's own value: back to the value for every game) |
| R3 | Pin the setting to the quick settings, or unpin it |
| L3 | Undo the last change made here (the changes are listed along the bottom) |
| Triangle | Search every setting by its name and description; R1 in the results shows one in its area |
| Circle | Back to the board, then out |

Beside the list, each setting says what it does, its current and default value, whether it applies at once or after a restart ("(Restart)" in its name), and whether it is SwanStation's or PSSwanStation's. SwanStation's options keep SwanStation's own names and descriptions; what PSSwanStation adds about one is a note under it. Over a running game, Graphics and Display leave a window on the game: the change shows there as it is made.

The quick settings are the settings you pin (R3). At first: Picture Preset, Internal Resolution Scale, CRT Shader, Fast Forward Speed and Vibration Strength. The menu over a game shows them beside its actions (R1), to change without leaving the game; they are kept in `quick-settings.txt` in the title folder.

Game Settings (the menu over a game, or a game's details) is the same board with only what can be a game's own: Console, Graphics, Display, Audio and Controllers' SwanStation options and the Display settings but Display Mode and HDR, which the console reads as PSSwanStation starts. A dot after a value there marks the game's own value.

Every setting, by area and section:

| Area | Section | Settings |
| --- | --- | --- |
| Console | Disc | CD-ROM Region Check, CD-ROM Read Thread, CD-ROM Async Readahead, Preload CD-ROM Image To RAM, Pre-cache CHD Images To RAM, Preload Network Images To RAM, CD-ROM Read Speedup, CD-ROM Seek Speedup, Apply Image Patches (Restart), USB Disc Drive (Restart), USB Disc Drive Speed |
|  | BIOS | Console Region, NTSC-J BIOS (Restart), NTSC-U BIOS (Restart), PAL BIOS (Restart), Fast Boot |
|  | CPU | CPU Execution Mode, CPU Overclocking, CPU Recompiler ICache, CPU Recompiler Block Linking, CPU Recompiler Fast Memory Access, CPU Recompiler Fast Memory Access Rewrite, Enable 8MB RAM (Dev Console), Internal Run-Ahead, Apply Compatibility Settings, Use Old MDEC Routines |
|  | Memory Cards | Memory Card 1 Type (Restart), Memory Card 2 Type, Use Single Card For Playlist, Memory Card Backups |
| Graphics | Renderer | GPU Renderer, Internal Resolution Scale, Adaptive Resolution Scale, Threaded Rendering (Software), Use Software Renderer For Readbacks (Restart), Multisample Antialiasing, Downsampling, Shader Precompilation, GPU On Separate Core (Restart), Threaded Command Recording (Restart), GPU Wait Spin Time (Restart), GPU Wait Sleep Time (Restart) |
|  | Geometry | PGXP Geometry Correction, PGXP Culling Correction, PGXP Perspective Correct Textures, PGXP Perspective Correct Colors, PGXP Depth Buffer, PGXP Vertex Cache, PGXP CPU Mode, PGXP Preserve Projection Precision, PGXP Geometry Tolerance, PGXP Depth Clear Threshold, Widescreen Hack, Use Widescreen Patches, Close Gaps Between Polygons, Lines Drawn as the Console Draws Them |
|  | Textures | Texture Filtering, Enable VRAM Write Texture Replacement, HD Texture Packs (Texture Blocks), Write Out Texture Blocks, Preload Texture Replacements |
|  | Colour | True Color Rendering, Scaled Dithering, Accurate Blending, Chroma Smoothing For 24-Bit Display |
| Display | Preset | Picture Preset, Differences From Original |
|  | Frame | Core Provided Aspect Ratio, Custom Aspect Ratio Numerator, Custom Aspect Ratio Denominator, Crop Mode, Force 4:3 For 24-Bit Display, Disable Interlacing, Force NTSC Timings, Display Active Start Offset, Display Active End Offset, Display Line Start Offset, Display Line End Offset |
|  | Scaling | Display Scaling, Scaling Filter, Sharpening Strength, Border Fill |
|  | Filters | CRT Shader, Import Shader Presets, Video Signal Simulation, Film Grain |
|  | Adjustments | Brightness, Contrast, Saturation, Gamma |
|  | Frame Generation | Frame Generation, Frame Generation Method, Frame Generation Quality, Frame Generation Mode, Frame Generation Limit, Frame Generation For Videos, Frame Generation Run-Ahead, Show Motion Vectors |
|  | Output | Frame Pacing, Display Mode (Restart), Black Frame Insertion, HDR Output (Restart), HDR Paper White, HDR Peak Brightness, Calibrate HDR, Display OSD Messages, Show Frame Rate |
| Audio | Mix | Game Volume, Menu Sound Volume, Menu Music Volume, Video Preview Volume |
|  | Output | Audio Latency, Mute CD Audio, Use Per-Frame Audio Batching (Restart) |
|  | Interface Sounds | Menu Music, Sound Theme, Startup Sound |
| Controllers | Ports | Connected Controllers, Multitap Mode, Controller 1 Type, Controller 2 Type, Controller 3 Type, Controller 4 Type, Analog Mode Combo, Enable Rumble, GunCon Show Crosshair |
|  | Controller 1 | Controller 1 Force Analog Mode, Controller 1 Use Analog Sticks for D-Pad in Digital Mode, Controller 1 Analog Axis Scale, Controller 1 Vibration Bias, Controller 1 Lightgun X Scale, Controller 1 Lightgun Y Scale, Controller 1 NeGcon Steering Axis Deadzone, Controller 1 NeGcon Twist Response |
|  | Controller 2 | Controller 2 Force Analog Mode, Controller 2 Use Analog Sticks for D-Pad in Digital Mode, Controller 2 Analog Axis Scale, Controller 2 Vibration Bias, Controller 2 Lightgun X Scale, Controller 2 Lightgun Y Scale, Controller 2 NeGcon Steering Axis Deadzone, Controller 2 NeGcon Twist Response |
|  | Controller 3 | Controller 3 Force Analog Mode, Controller 3 Use Analog Sticks for D-Pad in Digital Mode, Controller 3 Analog Axis Scale, Controller 3 Vibration Bias, Controller 3 NeGcon Steering Axis Deadzone, Controller 3 NeGcon Twist Response |
|  | Controller 4 | Controller 4 Force Analog Mode, Controller 4 Use Analog Sticks for D-Pad in Digital Mode, Controller 4 Analog Axis Scale, Controller 4 Vibration Bias, Controller 4 NeGcon Steering Axis Deadzone, Controller 4 NeGcon Twist Response |
|  | Controller 5 | Controller 5 Force Analog Mode, Controller 5 Use Analog Sticks for D-Pad in Digital Mode, Controller 5 Analog Axis Scale, Controller 5 Vibration Bias |
|  | Controller 6 | Controller 6 Force Analog Mode, Controller 6 Use Analog Sticks for D-Pad in Digital Mode, Controller 6 Analog Axis Scale, Controller 6 Vibration Bias |
|  | Controller 7 | Controller 7 Force Analog Mode, Controller 7 Use Analog Sticks for D-Pad in Digital Mode, Controller 7 Analog Axis Scale, Controller 7 Vibration Bias |
|  | Controller 8 | Controller 8 Force Analog Mode, Controller 8 Use Analog Sticks for D-Pad in Digital Mode, Controller 8 Analog Axis Scale, Controller 8 Vibration Bias |
|  | Pad | Confirm Button, Button Mapping, Analog Stick Deadzone, Controller Vibration, Vibration Strength, Player Indicators, Tilt Steering, Tilt Steering Range, Invert Tilt Steering |
|  | Hotkeys | Hotkeys, Hotkey List, Fast Forward Speed |
|  | Lightgun | Sinden Lightgun (Restart), Sinden Border Size |
|  | USB | USB Controllers (Restart), USB Controller Assignment |
| Library | Sources | Data Folders, Scan USB Drives (Restart), Network Shares, Wake-on-LAN, Mods |
|  | Folders | Game Folders, BIOS Folder (Restart), Memory Cards Folder (Restart), Save States Folder, Cheats Folder, Covers Folder, Texture Packs Folder (Restart) |
|  | View | Library View, ES-DE Themes, Group Multi-Disc Games |
|  | Media | Download Covers, Video Previews, Video Preview Delay |
| Saving | Save States | Save State On Exit, Load Resume State On Start, Save State On Console Menu, Save States On USB Drive |
|  | Rewind | Rewind, Rewind Granularity, Rewind Buffer Size |
|  | Storage | Data Location, Sync Memory Cards, Sync Covers, Sync Now, Cloud Saves |
| Interface | Theme | Theme, Accent Colour, Interface Scale, Animations, Show Clock |
|  | Accessibility | High Contrast, Colour-Blind Safe Colours |
|  | Mascot | Swan Animation, Swan Follows Theme, Seasonal Decorations |
|  | Startup | Startup Animation, Screen Saver, Console Notifications |
| Network | RetroAchievements | RetroAchievements, RetroAchievements Account, Hardcore Mode, Unofficial Achievements |
|  | Remote | Remote Control, Update Cheat Database |
|  | Updates | Check For Updates |
| System | Logging | Log Mode, Log Location (Restart), Log Level, Write Log Now |
|  | Performance | Prioritize Audio And Input (Restart), Log Performance Measurements |
|  | Input Recording | Record Input, Stop Recording, Stop Playback |

## Troubleshooting

Two logs say what happened: `psswanstation-boot.log` (written from the first moment of every start, with the two starts before kept as `.1.log` and `.2.log`) and `logs/psswanstation.log`, the emulator's and the graphics driver's own messages. Both are in the title's folder, or in `PSSwanStation` on a USB drive.

Settings, System, Logging decides how (from the next start):

| Setting | Choices |
| --- | --- |
| Logs | **Written as they come** (the usual: everything for finding a problem). **Only when something goes wrong**: the lines are kept in memory (the session's first 512 KB, with its start-up, and its last 8 MB) and written only on a crash, a notice of a problem, a game that stands still for 15 seconds, or "Write Log Now"; nothing is written otherwise, and the logs of the runs before stay. **Off**: nothing, not even a crash report |
| Log Location | The title's folder, or a USB drive (off the console's storage; the title's folder when no drive can be written to) |
| Log Level | **The boot log** alone (start-up, problems, each game's summary). **Boot log and emulator's log** (the usual). **Everything**: all the emulator says, and a line every ten seconds about a game's speed (what "Verbose logging" was) |

To send a log of a problem that did not crash with "Only when something goes wrong", choose "Write Log Now" right after it. A game that gives no new frame for 15 seconds (PSSwanStation's menus and the console's own menu not counted) writes the log by itself, with a line saying so ("watchdog: no frame for 15 s"), and the rest of the session's lines are written as they come: the log is there even when the console then closes PSSwanStation.

**Sparing the console's storage.** Settings are kept in memory as they change and written when PSSwanStation closes and when the console's menu opens (the console can close the title from there); a file is not written again when nothing in it changed. The graphics driver keeps the one shader cache (`radv-shader-cache`, written once for each new shader); the emulator's own pipeline cache file is gone. An update unpacks and replaces only the files that changed. Save states can go to a USB drive (Settings, Saving, "Save States On USB Drive"), and so can the logs.

| Problem | What to do |
| --- | --- |
| Black screen after choosing the icon | Look for `psswanstation-boot.log`. None means the title did not start at all: check kstuff, and the folder's place and name |
| Dark screen after choosing 120 Hz | Start holding L1 and R1, wait ten seconds, close it and start it again |
| PSSwanStation stops when a game starts | Switch off sleep-safe saving; then try Safe start (hold L1 and R1) |
| A game crashes at once | Try CPU Execution Mode "Cached Interpreter" (Settings, Console, CPU) and, if it is set to Vulkan, GPU Renderer "Software" (Graphics) |
| A game is too slow | Lower the Internal Resolution Scale (Graphics); FSR 1 or NIS keeps it sharp |
| No USB tab | Switch on "Scan USB Drives" and restart; games go in `psx`, `ps1` or `playstation` at the top of the drive; the ELF loader must be running (port 9021) |
| No Network tab | Check the folders under Settings, Library, "Network Shares" (or the `path` lines in `network.cfg`, server by IP address); a server the page does not find may be asleep or have file sharing off |
| The console will not turn off | It was started with a disc in a USB drive. Hold the power button to turn it off, and next time start the console with the drive empty and put the disc in afterwards |
| No Disc Drive tab | Switch on "Scan USB Drives" or "USB Disc Drive" and restart; the ELF loader must be running (port 9021). The boot log's `disc drive:` lines say which drives `/dev` has, whether each opens, and why a disc was not read |
| A disc's videos or music are missing | The drive is read 2048 bytes a sector (the choice page says so): the console did not let the SCSI commands through |
| Sparkles along polygon edges | Check "Close Gaps Between Polygons" is on (Graphics); it only works above 1x |
| The picture stood still for a moment | The boot log has a line "the screen stood still for ..." after the lines saying what was being done |

**Reporting a problem.** Say what you did, what you saw, which build (top right of the library, or Settings, System, About) and which firmware. Copy the logs before starting the title again. Never attach game files, BIOS files, firmware or keys, and please leave game titles out: "a 30 fps racing game" is enough.
