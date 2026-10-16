# PSSwanStation privacy policy

SPDX-FileCopyrightText: 2026 Press5elect

PSSwanStation is a homebrew title I make for a jailbroken PS5. It has no
accounts of its own, no analytics, no telemetry and no advertising. I receive
nothing from it: no crash reports, no usage counts, no identifiers. Everything
it keeps stays on the console, in its title folder (or in
`/data/psswanstation` when that setting is on).

## What leaves the console, and only when

| What | Where it goes | When |
| --- | --- | --- |
| RetroAchievements: the user name, then the token the server hands back; the hash that identifies the game; unlocks, leaderboard entries and the rich presence line | retroachievements.org | only while signed in to RetroAchievements |
| Cover pictures | GitHub (raw.githubusercontent.com, libretro-thumbnails) and thumbnails.libretro.com: the request names the game's title | when a cover is downloaded |
| ES-DE themes | GitLab (ES-DE's list of themes) and the theme's own repository on GitHub or GitLab | only when "Get themes" is used |
| The cheat database | GitHub (DuckStation's chtdb release) | when it is downloaded |
| The update check | the GitHub API, this repository's latest release | when the check runs |
| Netplay | the other console's address that I type in, directly | only during netplay |
| Network game folders | the SMB, NFS or FTP server I name in `network.cfg` | when that folder is used |

Each request says it comes from PSSwanStation and its build number (its
User-Agent, for example `PSSwanStation/23 (PS5) rcheevos/<its version>`). Nothing else
about the console is sent. What those services do with a request is under
their own policies (RetroAchievements, GitHub, libretro).

## What is kept on the console

- `data/retroachievements.cfg`: the RetroAchievements user name and token
  (never the password). Signing out deletes the token.
- `data/achievements-unsent.txt`: unlocks and leaderboard entries made while
  the server could not be reached, kept until they are sent after the next
  sign-in. The requests in it carry the token, so the file is readable by the
  title alone.
- `logs/`: the boot log. Addresses, passwords, tokens and request URLs are not
  written to it.
- Saves, save states, options, covers, the cache and the other folders the
  manual describes.

The web panel, when switched on, serves on the local network only, and asks
for the key its QR code carries; it never serves the RetroAchievements file
or the network share's password.

Deleting the title folder (and `/data/psswanstation`, if used) removes
everything PSSwanStation has kept.

## Changes and contact

This file changes with the title, and its history is this repository's
history. Questions: an issue on
[github.com/Press5elect/PSSwanStation](https://github.com/Press5elect/PSSwanStation/issues).
