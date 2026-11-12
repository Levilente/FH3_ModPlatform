FH3 Mod Platform
================

The shared base for Forza Horizon 3 (Azimuth) mods. It's one DLL, DINPUT8.dll, which every
Azimuth loads from its own folder. It loads every mod in the fh3-mods folder next to azimuth.exe and
gives them game-text edits, game file and database changes, title-screen
notices, update checks, logging, a way to talk to each other, access to
the game, and shared hooks into its code.

Players: copy DINPUT8.dll next to azimuth.exe (the folder of your Forza Horizon 3 install), then
start the game: the MODS tile on the Home tab opens the mods' pages. Needs Azimuth (tested with
1.0.4) and the Xbox One exe of Forza Horizon 3 (5 Jun 2018). Updating from 1.2.x or older: delete
DWrite.dll and d3dcompiler_47.dll first. It can sit next to the FH2 platform's DWrite.dll: each
runs only for its own game. Mods built on the platform usually ship it in their own zips too.

Built in: speed and power units of their own (Settings > HUD and
Gameplay, under Units), SELL CAR in the garage's car menu, the upgrade
bridge mods use for garage upgrades, and a proper saving screen when
you Exit to Desktop.

Mod makers: this zip has
  DINPUT8.dll               the platform, to test with
  include\fh3mod.h          the API (C/C++)
  include\fh3game.h         names for the game addresses mods use
  examples\HelloMod.dll     an example mod; put it in fh3-mods to try it
  tools\fh3mod.exe          runs mods' commands in the running game:
                            fh3mod list, fh3mod <mod> <command> [args]
Docs, the example's source and build setup:
  https://github.com/Levilente/FH3_ModPlatform

Everything the platform does is logged to fh3-mods\platform.log. If the
game crashes, a report naming the mod involved goes to fh3-mods\crashes.

MIT License. The platform includes MinHook (BSD 2-Clause), so ship
LICENSE-MinHook.txt along with the platform's DLLs. Unofficial fan-made
software; not affiliated with Playground Games, Turn 10, Microsoft or the
Azimuth developers.
