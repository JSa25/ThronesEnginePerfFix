Total War Saga: THRONES OF BRITANNIA - Engine Performance Fix (dinput8.dll)
==========================================================================

A drop-in DLL that fixes CPU bottlenecks in Thrones of Britannia's engine. It
changes no game files and no saves, so you can remove it at any time.

Created with the help of AI.

Source code: https://github.com/JSa25/ThronesEnginePerfFix

Game version: current Steam build (11631387). Every patch first checks the
exact game bytes it replaces. If they don't match (a different game version or
another engine mod), that patch turns itself off and the game runs unmodified.

Thrones of Britannia runs on the same engine as Attila, so this is a port of
the Attila Engine Performance Fix. Every fix below was checked against
Thrones' own code before it was carried over.


What it fixes
-------------
- Multithreading (the big one): almost every memory allocation in the game goes
  through one shared heap with a single lock. On the campaign map the game's
  16 worker threads spent about half their time waiting on that lock, and the
  main thread a third of its time. The heap is replaced with mimalloc for small
  blocks: game CPU use drops from about 10 busy cores to 3 and the campaign
  frame rate roughly doubles from this fix alone.
- x87 rounding: about 1,270 slow float-to-int conversions across the engine are
  replaced with modern instructions that give identical results. Includes the
  campaign map's fog tests, terrain height lookups and a curve table.
- Small-object pools: freeing a small object searched every pool chunk. This
  gets slower the longer a campaign runs; it now finds the right chunk directly.
- Render-list sorts: two sorts of the draw lists are about 11 times faster,
  with the same result.
- Small memory copies take a fast path.
- Battle render loops: three of the loops that hand objects to the graphics
  card fetch the next items' data ahead of time.
- Loading: the game checks every mod folder for a loose copy of each file it
  loads. The mod remembers which folders don't exist and answers those checks
  itself (it watches the folders and asks Windows again as soon as anything
  changes, so new loose files are still found). The unpacked save is read
  through a buffer instead of many tiny reads, and the pictures for distant
  units are compressed about 14 times faster during battle loading, with
  byte-identical results.
- Menus: closing a panel made the game throw away its pictures and load them
  from disk again on the next open. The mod keeps up to 1,024 recently used
  menu pictures loaded.

Every code patch gives bit-identical results to the original code. They were
tested against the game's own code on millions of inputs. (The menu fix only
keeps pictures in memory longer; what you see is the same.)


Measured results (Ryzen 9 9950X3D, RTX 5090, 4K)
-------------------------------------------------
CapFrameX, same save (early campaign) and same view, vanilla vs version 1.0:

                          Vanilla                 With the mod
  Campaign map            68 fps                  174 fps
     1% / 0.1% low        43 / 38 fps             92 / 67 fps
  Big battle (2 x 20 units, ultra unit size)
                          115 fps                 146 fps
     1% / 0.1% low        70 / 61 fps             88 / 80 fps
  End turn (stopwatch)    10 s                    11 s (no change)

Even at 4K the game was limited by the CPU here: on the campaign map the
graphics card was busy only about 5 ms of each frame. The heap fix makes most
of the difference on the campaign map. The loading and menu fixes are the same
code as in the Attila version; their effect in Thrones wasn't measured
separately.

Your numbers will differ, but the gains come from CPU time saved per frame, so
CPU-bound systems benefit the most. If your graphics card is at 95-100% load,
the card is the limit and the gain will be small.


Install from the Steam Workshop
-------------------------------
Steam downloads Workshop items into a folder the game doesn't load DLLs from,
so one extra step is needed:
1. Subscribe, then open the mod's folder:
   ...\steamapps\workshop\content\712100\3828420829\
2. Close the game and run install.bat. It copies dinput8.dll into the game
   folder (and backs up any dinput8.dll that was already there).
3. That's all. The mod updates itself: when Steam downloads a new version, the
   next game start installs it into the game folder, and it is used from the
   start after that (tobfix.log says so). If the game folder can't be written
   to, the log tells you to run install.bat again instead.

To uninstall, run uninstall.bat BEFORE unsubscribing. Unsubscribing alone
leaves the mod installed.


It doesn't show up in the launcher's mod list
---------------------------------------------
That's normal. This mod is a DLL in the game folder, not a .pack mod, so the
launcher and mod managers don't know about it. It's active as long as
dinput8.dll is in the game folder. To check, start the game and open
tobfix.log in the game folder: the STATUS block lists which fixes are on.


Manual install
--------------
1. Copy dinput8.dll into the game folder, next to thrones.exe, for example:
   ...\steamapps\common\Total War Saga Thrones of Britannia\
2. Start the game. A tobfix.log file appears in the same folder and lists
   which fixes are active, plus your game build, system and settings.

Uninstall: delete dinput8.dll (and tobfix.log).


Turning single fixes off
------------------------
Create an empty file with one of these names in the game folder, then restart
the game:
  heapfix_off.txt    memory allocator (heap)
  pool_off.txt       small-object pools
  sortfix_off.txt    render-list sorts
  x87fix_off.txt     x87 rounding (all of it)
  memfast_off.txt    small memory copies
  loopfetch_off.txt  battle render loop prefetch
  fileio_off.txt     save reading
  attrcache_off.txt  loose-file checks
  texfix_off.txt     battle texture compression
  uikeep_off.txt     keeping menu pictures loaded


Compatibility
-------------
- Thrones of Britannia, current Steam version. Uses SSE4.1 for most of the
  rounding fix (Intel since 2008, AMD since 2011); without it only part of
  that fix is used.
- Not compatible with other dinput8.dll mods.
- Works with normal .pack mods.


Reporting a problem
-------------------
Post your tobfix.log from the game folder. It starts with a header line per
game start and shows whether the mod loaded, which game version you have, a
STATUS block with the fixes that are on, your system and settings, other DLL
mods in the folder, and where a crash happened (and whether it was in the
game's code or in this mod). The log trims itself, so it stays small enough
to post.


License
-------
Source available: you can read the code and build the DLL for your own use.
Please don't redistribute it. mimalloc by Microsoft and Daan Leijen is under
the MIT license (LICENSE-mimalloc.txt).

Built with AI assistance (Claude by Anthropic). Tested in game by JGdaCaesar.
The mod is free and always will be. Donations are optional:
https://ko-fi.com/jgdacaesar60922


DLSS add-on (separate download)
-------------------------------
Since 1.1 this DLL can run the separate add-on "Upscaling and Framegen for
Thrones of Britannia": NVIDIA DLSS Super Resolution and DLSS Frame Generation
on NVIDIA RTX graphics cards. It does nothing unless that add-on's FrameBridge
folder is in the game folder (its install.bat puts it there). The add-on's own
README has the details; the engine fix keeps it up to date from the add-on's
Workshop item.


Changelog
---------
1.1 - runs the separate DLSS add-on "Upscaling and Framegen for Thrones of
      Britannia" (does nothing without it) and copies the add-on's Workshop
      updates in at game start. A very long game folder path can no longer
      overflow the names of the switch files.
      SHA-256 of dinput8.dll:
      183b259926ef9b9df89342c033ba1ee93514f5cc53b2342ec497d4037afdd3e0
1.0 - first release.
