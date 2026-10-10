# Roadmap

## Done

- **Both games recompiled**: Yuri's Revenge (`gamemd.exe` 1.001) and Red
  Alert 2 (`game.exe` 1.006) from the same install, each with the host,
  patches and remaster, the playtest suite at 30 of 30 for each.
- **Playing, not just reaching**: every menu screen, a skirmish to its score
  screen, both campaigns into their first mission, and `skirmish-build`
  (deploy, power plant, barracks, a GI, checked in the game's event log).
- **LAN** between two PCs, scripted on both (docs/testing.md).
- **Platforms**: Windows with MSVC (Visual Studio 2022 or 2026) or clang-cl;
  Linux natively on pcrecomp's win32hle, both games 30 of 30; Linux under
  Wine; macOS under CrossOver.
- **Setup**: Setup.cmd on Windows and `setup.sh` on Linux and macOS, from the
  ZIP download to a shortcut.
- **Mods**: a folder in `mods/`, laid over the game's, switched in game
  (`mods/README.md`).
- **Readable lifted C**: functions named from RTTI, vtables and the game's
  own messages, each with a header.
- The toolkit work all of this needed is on pcrecomp `main`.

## Next

In order; each is done when its check is in the suite or the docs.

1. **A mission played to its win.** A fight that throws voxel debris under
   the camera comes with it, and HD voxel animations go on by default once it
   passes (they are opt-in, `RA2_HD_VOXEL_ANIMS=1`).
2. **The rest of the menus** in the suite: the remaining Options screens,
   Load with a save present, WOnline as far as it goes without servers.
3. **Audio in `--record`**, and a native reference run (the shipping exe
   recorded) to compare frames against.
4. **Multiplayer**: a LAN match played to an end, more than two players, and
   the same across a NAT; Windows against Linux.
5. **Release**: v0.1.0.

## Tiberian Sun and Firestorm

[tiberiansun-recomp](https://github.com/sp00nznet/tiberiansun-recomp), its
own repo since each game's catalog, lift, patches and hooks are tied to its
exe's addresses. It started from this repo's host, presenter, scripted input
and test tools, and has the same platforms, mods and remaster. Moving that
shared host into pcrecomp, so both repos use one copy, is still to do.

## The remaster

The reason for the project. Each item sits on code the recompilation owns,
not on patches to a binary:

- **Presentation layer.** Done: the presenter, its settings menu, blurred
  bars beside the 4:3 menus, remembered settings (docs/presenter.md), and
  720p to 4K in game (docs/hires.md).
- **HD voxels.** Done for units, their shadows and aircraft
  (docs/voxels.md). Voxel animations and debris are written and opt-in (see
  Next, 1). Then the units drawn by 0x0073C5F0, and 2x above a game
  resolution of 2048x1080.
- **Higher-resolution art paths**, where a larger source exists or can be
  produced, behind the same asset loaders.
- **Modern input and audio**: raw mouse, rebindable keys, DirectSound replaced
  by a modern backend.
- **Co-op campaign**: two players through the campaign missions together,
  which neither game shipped. The likely path is the game's own lockstep
  multiplayer: a campaign map started as a network game, with the second
  player given a share of the player's house (or a house of its own allied to
  it), and the triggers, briefings, movies and win checks that assume one
  human made to accept two. Still to work out: how the mission's scripts
  name the player, and what a save means with two.

## Deferred

- Online play (Westwood Online / CnCNet): the servers are gone; LAN over IPXEmu
  stays as shipped.

## Out of scope

- Distributing the game, its data, or the lifted C.
- Reimplementing the game: that is what OpenRA does, and does well.
