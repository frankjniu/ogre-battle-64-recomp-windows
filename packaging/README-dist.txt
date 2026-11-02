Ogre Battle 64: Recomp
======================

A native Windows port of Ogre Battle 64: Person of Lordly Caliber (USA, Rev A),
built by statically recompiling the original N64 code.

This package contains no game data. You must supply your own ROM dump of the
USA Rev A cartridge.


REQUIREMENTS
------------

    OS     Windows 10 or 11, 64-bit.
    GPU    Direct3D 12.0 (Shader Model 6.0) or Vulkan 1.2. The oldest GPUs
           that covers are roughly GeForce GT 630, Radeon HD 7750 and Intel
           HD 510.
    CPU    x86-64 with SSE4.1.
    RAM    2 GB.
    Disk   about 150 MB, plus the ROM.
    ROM    your own Ogre Battle 64 (USA, Rev A) dump, 40 MB, as .z64, .n64
           or .v64.

A keyboard is enough to play; an XInput gamepad is optional. Without an audio
device the game runs silently. Nothing else has to be installed: the package
carries the Microsoft C++ runtime its shader compiler needs.

If the game crashes as it starts, update your graphics driver first.


HOW TO PLAY
-----------

1. Run ogrebattle64.exe.

2. The start screen appears:

       OGRE BATTLE 64: RECOMP
       [ MAIN ] [ MODS ] [ CONTROLS ] [ SETTINGS ] [ CHEATS ] [ DEBUG ]

   Give it the ROM in one of three ways:
   - on the MAIN tab, select the ROM row and press SPACE to pick the file;
   - drag the ROM onto the window;
   - put the ROM in this folder and start the game again.

   The ROM must be the USA Rev A cartridge. All three byte orders work. A ROM
   of another region or revision, or a modified one, is rejected, and the
   window says why.

3. START GAME (or ENTER) starts the game. The game keeps a copy of the ROM in
   this folder, so later launches go straight into the game. When a mod is
   installed, the start screen appears first so mods can be turned on or off.


SAVES
-----

The battery save is

    saves/ogrebattle64-us-rev1.bin

in this folder. Back it up to keep your progress; delete it to start fresh.

During play, F5 (quick save) copies your save, as of the last time you saved
in the game, to a quick slot. F9 (quick load), pressed twice, puts the quick
slot back as your save and restarts the game; choose Load Game at the title.
The save it replaced is kept as saves/ogrebattle64-us-rev1.bin.bak.

Saves from emulators (mupen64plus .srm, parallel-n64, DexDrive .N64) can be
imported with tools/sramsave.py from the source repository.


START SCREEN
------------

The start screen appears when no ROM is loaded, and whenever a mod is
installed. ESC opens the same panel over the running game and closes it again;
the MODS tab is disabled there, because mods load before the game starts.

    [ MAIN ]      START GAME, the ROM row (opens the file picker), EXIT GAME.
    [ MODS ]      one row per installed mod.
    [ CONTROLS ]  the keyboard key and gamepad button bound to each N64
                  button. Select a row and press the new key or button.
    [ SETTINGS ]  GAME SPEED: 1x, 2x, 4x or 8x. During play, = and - (or R3
                  on a gamepad) change it.
                  WIDESCREEN: 16:9 on the mission field, in cutscene
                  dialogues, on the world map and on the Organize Screen;
                  other screens keep 4:3 with side bars.
                  RESOLUTION: NATIVE (320x240), 2X, or AUTO (the largest
                  whole multiple of 320x240 that fits the window).
                  CRT FILTER: HYLLIAN (sharp, lightest on the GPU), CRTS
                  (slight curve) or LOTTES (shadow mask, bloom and curve).
                  WINDOW: windowed or fullscreen.
                  SOUNDS and VOLUME.
                  MOUSE ENABLED: the pointer drives the world map and
                  mission field cursors.
    [ CHEATS ]    MAX GOTH holds your goth at 9,999,999.
                  MAX ITEM COUNTS holds every owned item at 99.
                  QUICK LEVEL GAIN levels up every unit that reaches the
                  level check after a battle.
                  QUICK SOLDIER PROMOTION turns one soldier in the leader's
                  unit into a Fighter or Amazon after each won battle in
                  which a soldier is still standing.
                  UNLOCK ALL CLASSES fills the class list.
                  DAY ALWAYS 00 keeps the mission day at 00.
                  TIME OF DAY holds the clock at one hour.
                  Saving while a cheat is on keeps the cheated values.
    [ DEBUG ]     the running game's Chaos Frame value.

    TAB / SHIFT+TAB  switch tab
    UP / DOWN        select a row
    LEFT / RIGHT     change the selected option
    SPACE            activate the selected row
    ENTER            start the game
    mouse            click a tab or a row
    gamepad          D-pad moves, A selects, LB/RB switch tab

Bindings are saved in controls.cfg and settings in settings.cfg, both in this
folder.


MODS
----

Every .nrm file in mods/ is loaded at startup and listed on the MODS tab.
Drop a .nrm into mods/ to install it; delete it to uninstall. The package
carries three mods, all off:

    skip-boot-logos.nrm   Skip Boot Logos: boots straight to the title.
    exp-overflow.nrm      EXP Overflow: keeps the EXP left over after a
                          level-up.
    item-randomizer.nrm   Item Randomizer (experimental): an item received
                          after a battle or found on the map becomes a random
                          item. Back up your save before using it.

To turn a mod on, open the MODS tab, select its row and press SPACE. A mod
built for a different version of the game may be refused; the start screen
shows the reason.


IF SOMETHING GOES WRONG
-----------------------

If the game fails to start or crashes, it writes error.log in this folder. A
failure before the game window opens also shows a dialog. The file holds the
reason, the fault address and the last lines of the game's log. Attach the
whole file to a bug report:

    https://github.com/frankjniu/ogre-battle-64-recomp-windows/issues

The next crash overwrites error.log. Closing the window normally writes
nothing.

On an older GPU, set OGRE_GRAPHICS_API=vulkan or OGRE_GRAPHICS_API=d3d12
before launching to choose the renderer backend, and OGRE_CONSOLE=1 to open a
console with the boot log (in cmd: set OGRE_CONSOLE=1).

The source code, build instructions and every environment option are at:

    https://github.com/frankjniu/ogre-battle-64-recomp-windows


LEGAL
-----

Ogre Battle 64 (c) Quest / Nintendo. This is a fan preservation and
interoperability project. It ships no ROM, no extracted assets and no game
data.

This program is free software under the GNU General Public License, version 3.
LICENSE.txt in this folder holds the full text. The license covers the port's
own code. It does not cover Ogre Battle 64 or any of its data.

The executable includes SDL2 (https://libsdl.org), Copyright (C) 1997-2025
Sam Lantinga, under the zlib license.

The CRT FILTER options are ports of Timothy Lottes' public domain CRT shaders
(crt-lottes and crt-lottes-fast) and of Hyllian's CRT shader, which is under
this license:

    Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
