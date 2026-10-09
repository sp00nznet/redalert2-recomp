# Mods

Put a mod in a folder of its own here, by game:

    mods/yr/<name>/    Yuri's Revenge (rulesmd.ini, artmd.ini, ra2md.csf, expandmd*.mix, *.yrm maps...)
    mods/ra2/<name>/   Red Alert 2    (rules.ini, art.ini, ra2.csf, expand*.mix, *.mpr maps...)

The files go in the folder as they would go in the game's folder. The game's
own folder is never changed: the game reads a mod's file where the mod has
one and its own where it does not, and what it writes while a mod is on
(settings, saves) goes into the mod's folder.

To play one:

- **Linux**: `--mod <name>` on the command line, or **F9 at the main menu**,
  which restarts the game with the next mod (and after the last, with none).
  The window's title names the mod; the choice is remembered.
- **Windows**: the settings menu (**F10**) has a **Mod** list; choosing one
  restarts the game with it. `--mod <name>` works too.

`--mod none` plays the game as it shipped.

Mods made for **Ares** or **Phobos** (DLLs that patch `gamemd.exe`'s machine
code, started by Syringe or a launcher) do not work here: the recompiled game
is not that machine code, and what the mod needs from the DLL is missing. A
mod that ships its own `gamemd.exe` is played with this one's, and a MIX file
that its own exe loads by another name needs renaming to an `expandmd##.mix`
name the game looks for (`NPatch.mix` to `expandmd90.mix`, for a mod made on
NPatch). Rules, art, maps, sounds and MIX files are what a mod can bring.
