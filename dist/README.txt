mfg-unlock -- DLSS Multi Frame Generation at 3x/4x on RTX 40 series (Ada)
=========================================================================

READ THIS FIRST
---------------
ANTI-CHEAT: this injects a DLL into the game process. GTA V ships BattlEye,
other games ship EAC or their own. Using this in a game with anti-cheat
active -- GTA Online especially -- can get your account banned. Single-player
only, and check what your game runs first.

TESTED ON ONE MACHINE: an RTX 4070 Ti, driver 616.56, DOOM The Dark Ages and
GTA V Enhanced. Other Ada cards should work but have not been tried.

Nothing is written to disk. Every patch is applied in memory. Delete
version.dll and your install is exactly as it was.


INSTALL
-------
1. Copy version.dll into the game folder, next to the game's .exe.
   The game must import version.dll. DOOM The Dark Ages and GTA V
   Enhanced both do.

2. Create empty files in that same folder for what you want:

      mfg-unpin.txt      frame pacing fix   -- recommended, this is the one
                                               that stops 4x looking stuttery
      mfg-nometer.txt    keeps pacing on the CPU pacer -- recommended
      mfg-cubins.txt     three frame-gen kernels rebuilt for Ada
      mfg-indicator.txt  NVIDIA's own on-screen overlay (diagnostic)

   On Windows:  type nul > mfg-unpin.txt

3. Turn frame generation on in the game and pick 3x or 4x.

   DOOM also has a console variable, in DOOMTheDarkAgesConfig.local:
      r_streamlineDLSSGMode "3"      (0:Off, 1-3:number of frames to generate)


DID IT WORK
-----------
mfg-unlock.log appears next to version.dll:

   gates rewritten: 2
   CPU pacer enabled, sites: 1
   driver flip metering switched off, sites: 1
   kernels rebuilt from the Blackwell PTX: 3

Anything reporting "sites: 0" matched nothing and did nothing -- most likely
a Streamline version this build has not seen.

sl.log, same folder, should contain:

   Multi-frame supported, max generated frames 5


IF SOMETHING GOES WRONG
-----------------------
Delete the flag file for whichever patch you suspect, or delete version.dll
to remove everything. Nothing else is modified.

Start with mfg-unpin.txt alone. Add the rest one at a time.


WHY 4x LOOKS WRONG WITHOUT mfg-unpin.txt
----------------------------------------
The plugin flips a pacing mode about three quarters of a second after frame
generation starts, and the flip costs a full queue flush and a command
context switch -- in the middle of gameplay. That is the stutter, and it is
why changing the multiplier and changing it back appears to fix it. The
patch removes the transition.


Source, and what all of this actually patches:
https://github.com/<user>/mfg-unlock
