// PSSwanStation - where each setting goes on the settings board (ui_board.cpp),
// and the title's own settings' names and descriptions, in SwanStation's
// manner. SwanStation's options keep their own (name and desc are null).
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// match: an emulator option's key or a prefix of keys (ending in '*'), a
// picture setting ("picture:<name>"), or the row's label as its builder makes
// it. area -1: not shown (the same setting is shown elsewhere). def: the
// default's choice (-1: the emulator option's own default; -2: not a value).
// {root} is the title's folder as the player sees it.

// ------------------------------------------------------------------ Console
{ "swanstation_CDROM_RegionCheck", Console, "Disc", nullptr, nullptr, -1 },
{ "swanstation_CDROM_ReadThread", Console, "Disc", nullptr, nullptr, -1 },
{ "swanstation_CDROM_ReadaheadSectors", Console, "Disc", nullptr, nullptr, -1 },
{ "swanstation_CDROM_LoadImageToRAM", Console, "Disc", nullptr, nullptr, -1 },
{ "swanstation_CDROM_PreCacheCHD", Console, "Disc", nullptr, nullptr, -1 },
{ "Load network games into memory", Console, "Disc", "Preload Network Images To RAM",
	"Reads a disc image on a network share into RAM before the game starts. The game never waits for the network "
	"while it runs. Disabling this starts games sooner.", 1 },
{ "swanstation_CDROM_ReadSpeedup", Console, "Disc", nullptr, nullptr, -1 },
{ "swanstation_CDROM_SeekSpeedup", Console, "Disc", nullptr, nullptr, -1 },
{ "swanstation_CDROM_LoadImagePatches", Console, "Disc", nullptr, nullptr, -1 },
{ "USB disc drive", Console, "Disc", "USB Disc Drive (Restart)",
	"Plays discs from a USB CD or DVD drive connected to the console. Requires PSSwanStation to leave its sandbox "
	"at startup (Lapy, or the ELF loader on port 9021).", 0 },
{ "Drive speed", Console, "Disc", "USB Disc Drive Speed",
	"Sets the speed requested from the USB disc drive. Quiet is 8x, Normal is 24x, Max is the fastest the drive "
	"supports. A drive that fails a read twice is slowed down for the rest of the session.", 2 },
{ "swanstation_Console_Region", Console, "BIOS", nullptr, nullptr, -1 },
{ "swanstation_BIOS_PathNTSCJ", Console, "BIOS", nullptr, nullptr, -1 },
{ "swanstation_BIOS_PathNTSCU", Console, "BIOS", nullptr, nullptr, -1 },
{ "swanstation_BIOS_PathPAL", Console, "BIOS", nullptr, nullptr, -1 },
{ "swanstation_BIOS_PatchFastBoot", Console, "BIOS", nullptr, nullptr, -1 },
{ "swanstation_CPU_ExecutionMode", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_CPU_Overclock", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_CPU_RecompilerICache", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_CPU_RecompilerBlockLinking", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_CPU_FastmemMode", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_CPU_FastmemRewrite", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_Console_Enable8MBRAM", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_Main_RunaheadFrameCount", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_Main_ApplyGameSettings", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_Hacks_OldMDECRoutines", Console, "CPU", nullptr, nullptr, -1 },
{ "swanstation_MemoryCards_Card1Type", Console, "Memory Cards", nullptr, nullptr, -1 },
{ "swanstation_MemoryCards_Card2Type", Console, "Memory Cards", nullptr, nullptr, -1 },
{ "swanstation_MemoryCards_UsePlaylistTitle", Console, "Memory Cards", nullptr, nullptr, -1 },
{ "Card backups", Console, "Memory Cards", "Memory Card Backups",
	"Keeps a copy of a memory card when a game closes and the card has changed, in {root}data/saves/backups. "
	"Backups can be restored from the memory card manager.", 2 },

// ------------------------------------------------------------------ Graphics
{ "swanstation_GPU_Renderer", Graphics, "Renderer", nullptr, nullptr, -1 },
{ "swanstation_GPU_ResolutionScale", Graphics, "Renderer", nullptr, nullptr, -1 },
{ "picture:dynamic_resolution", Graphics, "Renderer", "Adaptive Resolution Scale",
	"Lowers the internal resolution scale one step at a time while the game cannot run at full speed, and raises "
	"it again once it can. Never goes above the scale chosen, or below half of it.", 0 },
{ "swanstation_GPU_UseThread", Graphics, "Renderer", nullptr, nullptr, -1 },
{ "swanstation_GPU_UseSoftwareRendererForReadbacks", Graphics, "Renderer", nullptr, nullptr, -1 },
{ "swanstation_GPU_MSAA", Graphics, "Renderer", nullptr, nullptr, -1 },
{ "swanstation_GPU_DownsampleMode", Graphics, "Renderer", nullptr, nullptr, -1 },
{ "swanstation_GPU_ShaderPrecompile", Graphics, "Renderer", nullptr, nullptr, -1 },
{ "GPU on a separate core", Graphics, "Renderer", "GPU On Separate Core (Restart)",
	"Runs the graphics driver's command recording thread and the software renderer's thread on dedicated CPU "
	"cores, and keeps every other thread off those cores.", 1 },
{ "Graphics driver on its own thread", Graphics, "Renderer", "Threaded Command Recording (Restart)",
	"Records the graphics driver's commands on a thread of its own, without dedicated cores. Only offered with "
	"GPU On Separate Core disabled.", 0 },
{ "Wait for the GPU: checks", Graphics, "Renderer", "GPU Wait Spin Time (Restart)",
	"Sets how long the graphics driver polls for the GPU to finish before it sleeps. Longer values react sooner "
	"and keep a CPU core busier.", 0 },
{ "Wait for the GPU: sleeps", Graphics, "Renderer", "GPU Wait Sleep Time (Restart)",
	"Sets how long the graphics driver sleeps between polls once it has stopped spinning. Shorter values react "
	"sooner after the GPU finishes.", 0 },
{ "swanstation_GPU_PGXP*", Graphics, "Geometry", nullptr, nullptr, -1 },
{ "swanstation_GPU_WidescreenHack", Graphics, "Geometry", nullptr, nullptr, -1 },
{ "picture:widescreen_patch", Graphics, "Geometry", "Use Widescreen Patches",
	"Uses a game's own widescreen patch from the patch database instead of the widescreen hack, when the aspect "
	"ratio is wide or the widescreen hack is enabled. Avoids objects appearing at the edges of the screen.", 1 },
{ "swanstation_GPU_CloseGaps", Graphics, "Geometry", nullptr, nullptr, -1 },
{ "swanstation_GPU_ExactLines", Graphics, "Geometry", nullptr, nullptr, -1 },
{ "swanstation_GPU_TextureFilter", Graphics, "Textures", nullptr, nullptr, -1 },
{ "swanstation_TextureReplacements_*", Graphics, "Textures", nullptr, nullptr, -1 },
{ "swanstation_GPU_TrueColor", Graphics, "Colour", nullptr, nullptr, -1 },
{ "swanstation_GPU_ScaledDithering", Graphics, "Colour", nullptr, nullptr, -1 },
{ "swanstation_GPU_AccurateBlending", Graphics, "Colour", nullptr, nullptr, -1 },
{ "swanstation_GPU_ChromaSmoothing24Bit", Graphics, "Colour", nullptr, nullptr, -1 },

// ------------------------------------------------------------------ Display
{ "picture:preset", Display, "Preset", "Picture Preset",
	"Applies a group of graphics and display settings to every game at once. Original matches the output of the "
	"console: software renderer, native resolution, dithering, interlacing and overscan cropping.", 1 },
{ "Differs from the original", Display, "Preset", "Differences From Original",
	"Lists every setting in effect that differs from the console's original output, to revert one by one or all "
	"at once.", -2 },
{ "swanstation_Display_AspectRatio", Display, "Frame", nullptr, nullptr, -1 },
{ "swanstation_Display_CustomAspectRatio*", Display, "Frame", nullptr, nullptr, -1 },
{ "swanstation_Display_CropMode", Display, "Frame", nullptr, nullptr, -1 },
{ "swanstation_Display_Force4_3For24Bit", Display, "Frame", nullptr, nullptr, -1 },
{ "swanstation_GPU_DisableInterlacing", Display, "Frame", nullptr, nullptr, -1 },
{ "swanstation_GPU_ForceNTSCTimings", Display, "Frame", nullptr, nullptr, -1 },
{ "swanstation_Display_Active*", Display, "Frame", nullptr, nullptr, -1 },
{ "swanstation_Display_Line*", Display, "Frame", nullptr, nullptr, -1 },
{ "picture:scaling", Display, "Scaling", "Display Scaling",
	"Determines how the frame is scaled to the screen. Fit keeps the aspect ratio. Integer only scales by whole "
	"multiples, keeping pixels even. Stretch fills the screen.", 0 },
{ "picture:scaler", Display, "Scaling", "Scaling Filter",
	"Selects the filter used to enlarge the frame to the screen. FSR 1, NIS and CAS sharpen after scaling. MMPX "
	"enlarges pixel art at the native resolution.", 2 },
{ "picture:fsr_sharpness", Display, "Scaling", "Sharpening Strength",
	"Sets how strongly FSR 1, NIS or CAS sharpens the enlarged frame.", 1 },
{ "picture:border", Display, "Scaling", "Border Fill",
	"Determines what fills the screen beside a 4:3 frame: black, a blurred copy of the frame, the frame's edges "
	"extended, or a picture file.", 0 },
{ "picture:crt", Display, "Filters", "CRT Shader",
	"Simulates the picture of a CRT display. Scanlines and Shadow Mask are drawn over the frame; the "
	"crt-guest-advanced presets by guest(r) simulate the beam, glow, mask and curvature.", 0 },
{ "Shader presets from USB", Display, "Filters", "Import Shader Presets",
	"Imports libretro slang presets (.slangp) from PSSwanStation/shaders on a USB drive. Imported presets are "
	"compiled on the console and listed under CRT Shader.", -2 },
{ "picture:signal", Display, "Filters", "Video Signal Simulation",
	"Simulates the video signal between the console and the TV. Dither Smoothed removes the dither pattern; "
	"S-Video and Composite soften colour as those cables did.", 0 },
{ "picture:grain", Display, "Filters", "Film Grain",
	"Applies animated film grain to the frame. Reduces visible banding in flat colours.", 0 },
{ "picture:brightness", Display, "Adjustments", "Brightness", "Sets the brightness of the frame.", 10 },
{ "picture:contrast", Display, "Adjustments", "Contrast", "Sets the contrast of the frame.", 10 },
{ "picture:saturation", Display, "Adjustments", "Saturation", "Sets the colour saturation of the frame.", 10 },
{ "picture:gamma", Display, "Adjustments", "Gamma",
	"Sets the gamma of the frame. Values above 1.00 lighten mid-tones, values below darken them.", 10 },
{ "picture:frame_generation", Display, "Frame Generation", "Frame Generation",
	"Generates frames between the game's own frames from the motion of the picture, for smoother movement in "
	"games that run below the display's refresh rate. Increases input latency.", 0 },
{ "picture:fg_method", Display, "Frame Generation", "Frame Generation Method",
	"Selects how intermediate frames are generated: PSSwanStation's own, AMD FSR3 optical flow, or MihawkGen.", 0 },
{ "picture:fg_quality", Display, "Frame Generation", "Frame Generation Quality",
	"Sets the precision of the motion estimation. Higher quality costs more GPU time.", 1 },
{ "picture:fg_mode", Display, "Frame Generation", "Frame Generation Mode",
	"Determines whether generated frames are placed between the game's frames, or predicted ahead of them for "
	"lower latency.", 0 },
{ "picture:fg_cap", Display, "Frame Generation", "Frame Generation Limit",
	"Sets the highest frame rate generated: the display's refresh rate, or 60 frames per second.", 0 },
{ "picture:fg_videos", Display, "Frame Generation", "Frame Generation For Videos",
	"Determines whether frames are generated for full-motion video.", 0 },
{ "picture:fg_runahead", Display, "Frame Generation", "Frame Generation Run-Ahead",
	"Uses run-ahead to compensate for the latency frame generation adds.", 0 },
{ "picture:fg_debug", Display, "Frame Generation", "Show Motion Vectors",
	"Draws the estimated motion over the frame, for debugging frame generation.", 0 },
{ "picture:pacing", Display, "Output", "Frame Pacing",
	"Determines how frames are timed to the display. Display Sync matches the game to the refresh rate and "
	"stretches audio to fit. The other modes keep the game's exact rate.", 0 },
{ "Display output", Display, "Output", "Display Mode (Restart)",
	"Sets the video mode requested from the console: 60 Hz, 120 Hz, or 120 Hz with variable refresh rate.", 0 },
{ "picture:black_frames", Display, "Output", "Black Frame Insertion",
	"Inserts a black frame between frames at 120 Hz, for clearer motion and a darker picture. Requires Display "
	"Sync pacing and a game at half the refresh rate.", 0 },
{ "HDR", Display, "Output", "HDR Output (Restart)",
	"Outputs the picture as HDR10. Requires HDR enabled in the console's video output settings and an HDR display.", 0 },
{ "HDR white level", Display, "Output", "HDR Paper White",
	"Sets the brightness of white in HDR output. 203 nits is the reference level.", 13 },
{ "HDR highlights", Display, "Output", "HDR Peak Brightness",
	"Sets the brightness the brightest parts of the picture are expanded towards. Set it to the display's peak "
	"brightness.", 7 },
{ "Adjust HDR brightness", Display, "Output", "Calibrate HDR",
	"Shows test patterns to find the display's peak brightness and the paper white level.", -2 },
{ "swanstation_Display_ShowOSDMessages", Display, "Output", nullptr, nullptr, -1 },
{ "Show frame rate", Display, "Output", "Show Frame Rate",
	"Shows the frame rate and the emulation speed in the corner of the screen.", 0 },

// ------------------------------------------------------------------ Audio
{ "Game", Audio, "Mix", "Game Volume", "Sets the volume of the game. Every other volume follows it.", 20 },
{ "Menu sounds", Audio, "Mix", "Menu Sound Volume", "Sets the volume of the interface's sounds.", 8 },
{ "Music", Audio, "Mix", "Menu Music Volume", "Sets the volume of the interface's music.", 6 },
{ "Videos", Audio, "Mix", "Video Preview Volume",
	"Sets the volume of video previews in the library. Menu music is lowered while one plays.", 4 },
{ "Video sound", -1, "", nullptr, nullptr, -2 },
{ "Sound delay", Audio, "Output", "Audio Latency",
	"Sets how much audio is buffered ahead of the output. Lower values reduce latency; increase it if the sound "
	"crackles.", 0 },
{ "swanstation_CDROM_MuteCDAudio", Audio, "Output", nullptr, nullptr, -1 },
{ "swanstation_Audio_FastHook", Audio, "Output", nullptr, nullptr, -1 },
{ "Menu music", Audio, "Interface Sounds", "Menu Music",
	"Selects the music played in the interface: PSSwanStation's own, the files in {root}music, or the interface "
	"kit's songs.", 0 },
{ "Sound set", Audio, "Interface Sounds", "Sound Theme", "Selects the set of sounds the interface plays.", 0 },
{ "Start-up sound", Audio, "Interface Sounds", "Startup Sound", "Plays a sound with the startup animation.", 1 },

// ------------------------------------------------------------------ Controllers
{ "Controllers", Controllers, "Ports", "Connected Controllers",
	"Lists the players with a controller. Player 1 is the user who started PSSwanStation; other players are "
	"users signed in on the console with a controller of their own.", -2 },
{ "Multitap", -1, "", nullptr, nullptr, -2 },
{ "swanstation_ControllerPorts_MultitapMode", Controllers, "Ports", nullptr, nullptr, -1 },
{ "Player 1's controller", Controllers, "Ports", "Controller 1 Type",
	"Selects the type of controller connected to port 1.", 1 },
{ "Player 2's controller", Controllers, "Ports", "Controller 2 Type",
	"Selects the type of controller connected to port 2.", 1 },
{ "Player 3's controller", Controllers, "Ports", "Controller 3 Type",
	"Selects the type of controller connected to port 3. Requires the multitap.", 1 },
{ "Player 4's controller", Controllers, "Ports", "Controller 4 Type",
	"Selects the type of controller connected to port 4. Requires the multitap.", 1 },
{ "swanstation_Controller_AnalogCombo", Controllers, "Ports", nullptr, nullptr, -1 },
{ "swanstation_Controller_EnableRumble", Controllers, "Ports", nullptr, nullptr, -1 },
{ "swanstation_Controller_ShowCrosshair", Controllers, "Ports", nullptr, nullptr, -1 },
{ "swanstation_Controller1_*", Controllers, "Controller 1", nullptr, nullptr, -1 },
{ "swanstation_Controller2_*", Controllers, "Controller 2", nullptr, nullptr, -1 },
{ "swanstation_Controller3_*", Controllers, "Controller 3", nullptr, nullptr, -1 },
{ "swanstation_Controller4_*", Controllers, "Controller 4", nullptr, nullptr, -1 },
{ "swanstation_Controller5_*", Controllers, "Controller 5", nullptr, nullptr, -1 },
{ "swanstation_Controller6_*", Controllers, "Controller 6", nullptr, nullptr, -1 },
{ "swanstation_Controller7_*", Controllers, "Controller 7", nullptr, nullptr, -1 },
{ "swanstation_Controller8_*", Controllers, "Controller 8", nullptr, nullptr, -1 },
{ "Confirm button", Controllers, "Pad", "Confirm Button",
	"Selects the button that confirms in the interface. The other button goes back. Games are not affected.", 0 },
{ "Buttons", Controllers, "Pad", "Button Mapping",
	"Maps the controller's buttons to the PlayStation's, selects the button that opens the menu, and sets turbo "
	"buttons.", -2 },
{ "Stick dead zone", Controllers, "Pad", "Analog Stick Deadzone",
	"Sets how far an analog stick moves from the centre, in any direction, before the game sees it.", 2 },
{ "Vibration", Controllers, "Pad", "Controller Vibration", "Passes the game's vibration to the controller.", 1 },
{ "Vibration strength", Controllers, "Pad", "Vibration Strength",
	"Scales the vibration strength. 100% is the game's own.", 9 },
{ "Player lights", Controllers, "Pad", "Player Indicators",
	"Shows each player's colour on the controller's light bar.", 1 },
{ "Tilt steering", Controllers, "Pad", "Tilt Steering",
	"Maps player 1's controller tilt to the left analog stick, for steering. Experimental.", 0 },
{ "Tilt for the whole way", Controllers, "Pad", "Tilt Steering Range",
	"Sets how far the controller is tilted for full stick deflection.", 2 },
{ "Tilt the other way round", Controllers, "Pad", "Invert Tilt Steering", "Inverts the tilt steering direction.", 0 },
{ "Shortcuts", Controllers, "Hotkeys", "Hotkeys",
	"Enables hotkeys while a game runs: OPTIONS held with R2 fast forwards, with L2 rewinds, with R1 and L1 saves "
	"and loads a state. The menu opens when OPTIONS is released.", 1 },
{ "What the shortcuts are", Controllers, "Hotkeys", "Hotkey List", "Lists every hotkey.", -2 },
{ "Fast forward speed", Controllers, "Hotkeys", "Fast Forward Speed",
	"Sets the emulation speed while the fast forward hotkey is held.", 1 },
{ "Sinden Lightgun", Controllers, "Lightgun", "Sinden Lightgun (Restart)",
	"Aims a GunCon with a Sinden Lightgun on USB. PSSwanStation draws a white border around the frame and tracks "
	"it with the gun's camera. Experimental.", 0 },
{ "Border", Controllers, "Lightgun", "Sinden Border Size",
	"Sets the thickness of the white border around the frame, as a percentage of the screen's height. The frame "
	"is reduced so that none of it is covered.", 2 },
{ "USB controllers", Controllers, "USB", "USB Controllers (Restart)",
	"Reads USB controllers the console does not support itself: PS3 controllers, DualShock 3, Xbox 360 "
	"controllers and other USB gamepads. Experimental.", 0 },
{ "A USB controller is", Controllers, "USB", "USB Controller Assignment",
	"Determines which player a USB controller is: the next player without a controller, or player 1 together with "
	"the DualSense.", 0 },

// ------------------------------------------------------------------ Library
{ "Folders", Library, "Sources", "Data Folders",
	"Games go in {root}games, BIOS files in bios, covers in covers, cheats in cheats, texture packs in "
	"textures/<serial>. Memory cards, save states and settings are kept in data.", -2 },
{ "USB drives", Library, "Sources", "Scan USB Drives (Restart)",
	"Scans USB drives for games in a psx, ps1 or playstation folder. Requires PSSwanStation to leave its sandbox "
	"at startup (Lapy, or the ELF loader on port 9021).", 0 },
{ "Network shares", Library, "Sources", "Network Shares",
	"Adds folders of games on SMB or NFS shares or FTP servers, and the folder for your files. Servers on the "
	"network are found automatically.", -2 },
{ "Wake the server", Library, "Sources", "Wake-on-LAN",
	"Wakes a sleeping server at startup and whenever its share is scanned, by the hardware address given in "
	"{root}network.cfg.", -2 },
{ "Mods", Library, "Sources", "Mods",
	"Applies mods to games: patches, GameShark codes and options, without changing the disc image. Mods are read "
	"from {root}mods.", -2 },
{ "Game Folders", Library, "Folders", "Game Folders",
	"Adds folders of games anywhere on the console's storage or a USB drive. Each folder is searched with the "
	"folders inside it; the games stay where they are.", -4 },
{ "folder:bios", Library, "Folders", "BIOS Folder (Restart)",
	"Selects the folder the BIOS files are read from. Default: {root}bios.", -4 },
{ "folder:memory_cards", Library, "Folders", "Memory Cards Folder (Restart)",
	"Selects the folder memory cards and their backups are kept in. Default: {root}data/saves. Existing cards are "
	"not moved.", -4 },
{ "folder:save_states", Library, "Folders", "Save States Folder",
	"Selects the folder save states are kept in. Overrides Save States On USB Drive. Default: {root}data/states. "
	"Existing states are not moved.", -4 },
{ "folder:cheats", Library, "Folders", "Cheats Folder",
	"Selects the folder your own cheat files (<serial>.cht) are read from and written to. Default: {root}cheats.", -4 },
{ "folder:covers", Library, "Folders", "Covers Folder",
	"Selects the folder covers are read from and downloaded to. Default: {root}covers.", -4 },
{ "folder:textures", Library, "Folders", "Texture Packs Folder (Restart)",
	"Selects a folder searched first for texture packs (<serial>/). The usual places are still searched. Default: "
	"{root}textures.", -4 },
{ "Library view", Library, "View", "Library View",
	"Selects the layout of the library: Shelves, Covers, List, the 3D views, Aurora layouts from {root}layouts, "
	"and ES-DE themes from {root}themes.", 0 },
{ "ES-DE themes", Library, "View", "ES-DE Themes",
	"Selects the ES-DE theme, its variant, colour scheme and font size, and downloads themes.", -2 },
{ "Multi-disc games as one", Library, "View", "Group Multi-Disc Games",
	"Shows the discs of a game as one entry, started from the first disc, with one memory card for all discs.", 1 },
{ "Download covers", Library, "Media", "Download Covers",
	"Downloads cover art from the libretro thumbnails for games without one. Covers in {root}covers are never "
	"replaced.", 1 },
{ "Game videos", Library, "Media", "Video Previews",
	"Plays a game's video (videos/<file name>.mp4, ES-DE's media folders) when it is selected in the library.", 1 },
{ "Video starts after", Library, "Media", "Video Preview Delay",
	"Sets how long a game is selected before its video starts. ES-DE themes set their own.", 1 },

// ------------------------------------------------------------------ Saving
{ "Save when a game is closed", Saving, "Save States", "Save State On Exit",
	"Saves a resume state when a game is closed, separate from the ten save state slots.", 0 },
{ "Continue where I left off", Saving, "Save States", "Load Resume State On Start",
	"Loads the resume state when a game is started. The game's details page can still start it from the "
	"beginning.", 0 },
{ "Sleep-safe saving", Saving, "Save States", "Save State On Console Menu",
	"Saves the resume state whenever the console's menu opens over the game, so that rest mode or closing from "
	"the console's menu loses nothing.", 0 },
{ "Save states on a USB drive", Saving, "Save States", "Save States On USB Drive",
	"Stores save states and the resume state on a USB drive instead of the console's storage.", 0 },
{ "Rewind", Saving, "Rewind", "Rewind",
	"Keeps states in memory so that the rewind hotkey can go back through the last moments of play. Reduces "
	"performance slightly. Disabled in netplay and hardcore mode.", 0 },
{ "Rewind steps", Saving, "Rewind", "Rewind Granularity",
	"Sets how often a rewind state is kept. Finer steps rewind more smoothly and use more memory.", 1 },
{ "Rewind memory", Saving, "Rewind", "Rewind Buffer Size", "Sets the amount of memory rewind states may use.", 1 },
{ "Where my files are kept", Saving, "Storage", "Data Location",
	"Determines where BIOS files, covers, cheats, memory cards, states, texture packs and settings are kept: the "
	"title's folder, /data/psswanstation, a USB drive, or the network share.", 0 },
{ "Memory cards on the network share", Saving, "Storage", "Sync Memory Cards",
	"Keeps memory cards in the network share's files folder, so that every console uses the same cards.", 0 },
{ "Covers on the network share", Saving, "Storage", "Sync Covers",
	"Keeps covers in the network share's files folder, so that every console has the same covers.", 0 },
{ "Bring my files up to date now", Saving, "Storage", "Sync Now",
	"Sends changed files to the network share and fetches files changed there now.", -2 },
{ "Cloud saves", Saving, "Storage", "Cloud Saves",
	"Keeps memory cards, and optionally save states, in Google Drive or Dropbox. Signing in uses a phone.", -2 },

// ------------------------------------------------------------------ Interface
{ "Theme", Interface, "Theme", "Theme",
	"Selects the interface's colours, panels, fonts, motion and sounds: PSSwanStation's own, or one of the "
	"interface kit's thirty.", 0 },
{ "Accent colour", Interface, "Theme", "Accent Colour", "Sets the colour of the cursor and highlights.", 0 },
{ "Size", Interface, "Theme", "Interface Scale", "Sets the size of text and every other element.", 2 },
{ "Animations", Interface, "Theme", "Animations",
	"Determines how much the interface animates. Off also disables video previews.", 0 },
{ "Clock", Interface, "Theme", "Show Clock", "Shows the console's time in the library's header.", 1 },
{ "High contrast", Interface, "Accessibility", "High Contrast",
	"Uses solid panels, the strongest text and a stronger cursor in every theme.", 0 },
{ "Colour-blind safe colours", Interface, "Accessibility", "Colour-Blind Safe Colours",
	"Uses blue and orange instead of green and red.", 0 },
{ "Swan", Interface, "Mascot", "Swan Animation",
	"Determines how the swan moves: Lively, Calm, or Still.", 0 },
{ "Swan dresses for the theme", Interface, "Mascot", "Swan Follows Theme",
	"Draws the swan in the style of the theme.", 1 },
{ "Seasonal touches", Interface, "Mascot", "Seasonal Decorations",
	"Dresses the swan for the season: by the date, a season chosen, or none.", 0 },
{ "Start-up animation", Interface, "Startup", "Startup Animation",
	"Plays the startup animation. Any button skips it.", 1 },
{ "Screen saver", Interface, "Startup", "Screen Saver",
	"Darkens the screen after the menus have had no input for this long. Never while a game is on screen.", 2 },
{ "Console notices", Interface, "Startup", "Console Notifications",
	"Uses the console's notifications for problems outside PSSwanStation's screen, such as a startup error.", 0 },

// ------------------------------------------------------------------ Network
{ "RetroAchievements", Network, "RetroAchievements", "RetroAchievements",
	"Enables achievements from retroachievements.org. Requires an account and an internet connection.", 0 },
{ "Account", Network, "RetroAchievements", "RetroAchievements Account", "Signs in to or out of retroachievements.org.", -2 },
{ "Hardcore mode", Network, "RetroAchievements", "Hardcore Mode",
	"Disables save state loading, cheats and rewind, for full achievement points. Enabling it resets the game.", 0 },
{ "Unofficial achievements", Network, "RetroAchievements", "Unofficial Achievements",
	"Shows achievements still in development. They award no points. Applies from the next game.", 0 },
{ "Phone and web control", Network, "Remote", "Remote Control",
	"Controls PSSwanStation from a browser on a phone or computer on the same network.", -2 },
{ "Fetch the newest cheat database", Network, "Remote", "Update Cheat Database",
	"Downloads the newest release of the chtdb cheat database.", -2 },
{ "Check for updates", Network, "Updates", "Check For Updates",
	"Checks for a newer build at startup and shows it in the header. Nothing is downloaded until Update is "
	"chosen.", 1 },

// ------------------------------------------------------------------ System
{ "Logs", System, "Logging", "Log Mode",
	"Determines when the boot log is written: immediately, only when something goes wrong (kept in memory until "
	"then), or never.", 0 },
{ "Where the logs are kept", System, "Logging", "Log Location (Restart)",
	"Determines where log files are written: the title's folder, or a USB drive.", 0 },
{ "What the logs keep", System, "Logging", "Log Level",
	"Selects what is logged: the boot log, also the emulator's and the graphics driver's log, or everything at the "
	"developer level.", 1 },
{ "Save the log now", System, "Logging", "Write Log Now",
	"Writes the log kept in memory to psswanstation-boot.log now.", -2 },
{ "Sound and controls before the emulator", System, "Performance", "Prioritize Audio And Input (Restart)",
	"Runs the audio and USB controller threads at a higher priority than the emulator, and background work at a "
	"lower one. Experimental.", 0 },
{ "Measure the game in the boot log", System, "Performance", "Log Performance Measurements",
	"Writes the speed, the slowest frame and the time of each part of a frame to the boot log every five "
	"seconds.", 0 },
{ "Record what I press", System, "Input Recording", "Record Input",
	"Records the inputs of a game frame by frame from its start. Playing a recording back repeats the game "
	"exactly.", -2 },
{ "Stop recording", System, "Input Recording", "Stop Recording", "Stops the input recording.", -2 },
{ "Stop playback", System, "Input Recording", "Stop Playback", "Stops the input playback.", -2 },
