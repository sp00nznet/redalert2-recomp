# Recon: what the binaries are

## The install

The Steam build of *Command & Conquer: Red Alert 2 and Yuri's Revenge* (from
*The Ultimate Collection*), 1.9 GB:

| File | Size | Built | Role |
|---|---:|---|---|
| `gamemd.exe` | 5,286,208 | 2001-10-31 | **Yuri's Revenge 1.001: the target** |
| `game.exe` | 5,077,312 | 2001-06-06 | Red Alert 2 1.006, same engine |
| `RA2MD.exe`, `Ra2.exe` | 742,152 / 893,704 | | launchers |
| `binkw32.dll` | 286,208 | | RAD Bink video (imported directly) |
| `Blowfish.dll` | 225,331 | | Westwood Online cipher, an in-process COM server |
| `ddraw.dll` | 2,835,968 | | DDrawCompat, added by the Steam release |
| `wsock32.dll` | 24,064 | | IPXEmu (LAN games over UDP), added by the Steam release |

`gamemd.exe` is the target because it is the newer build and the one the
modding community has mapped for twenty years (YRpp, Ares, Phobos all address
1.001). `game.exe` is the same engine six months earlier; once `gamemd.exe`
runs, it is the same pipeline again.

## gamemd.exe

```
  Machine:       i386
  Image Base:    0x00400000
  Entry Point:   0x007CD80F (RVA 0x003CD80F)
  Linker:        6.00
  Timestamp:     2001-10-31 01:30:54 UTC (0x3BDF544E)
  Code Range:    0x00401000 - 0x007E1000 (4,063,232 bytes)
    .text     VA 0x00001000  VSize 0x003E0000
    .rdata    VA 0x003E1000  VSize 0x00031000
    .data     VA 0x00412000  VSize 0x00367BE4  (raw 0x6C000: 3.1 MB of it is BSS)
    .rsrc     VA 0x0077A000  VSize 0x0008A000
  Imports: 371 functions from 15 DLLs
  No known protection/packer indicators found.
```

MSVC 6.0, statically linked CRT, no `.reloc`, no DRM. The 10,560-byte
overlay is the Authenticode signature. Raw offsets equal RVAs for `.text`,
`.rdata` and `.data`, which makes byte-level work on it easy.

The **EA App (Origin) build** is the same: every section of its `game.exe`
and `gamemd.exe` is byte-identical to Steam's, and so is the PE timestamp.
Only the header checksum and the signature differ (EA re-signed them, 296
bytes longer), so it lifts to the same C and the same patches apply. The
rest of the folder matches too, plus EA's `Core\Activation.dll`, which the
exes do not import.

The **launcher check is already gone** in this build. `0x0049F5C0` ("Checking
if launcher is running") and `0x0049F620` ("Notify launcher") are both
`mov al, 1; ret`, with the original bodies left behind them as dead code. So
`gamemd.exe` runs without `RA2MD.exe`, and the host needs no launcher mutex.

Imports: `KERNEL32` 162, `USER32` 96, `GDI32` 22, `WSOCK32` 19, `ole32` 15,
`binkw32` 13, `OLEAUT32` 11, `ADVAPI32` 9, `COMCTL32` 7, `IMM32` 6, `WINMM` 5,
`VERSION` 3, and one each from `DDRAW` (`DirectDrawCreate`), `DSOUND`
(ordinal 1, `DirectSoundCreate`) and `SHELL32`. Unlike The Movies, DirectX is
in the import table, so headless DirectDraw is a plain import shim.

## Classes: RTTI was left on

```
[*] type descriptors : 988
[*] complete object locators : 1,214
[*] classes          : 954
[*] virtual methods  : 6,665  (6,664 attributable to one class)
```

The names are the ones the community uses: `TechnoClass`, `BuildingClass`,
`UnitClass`, `HouseClass`, `CellClass`, `DisplayClass`, `CCINIClass`,
`MixFileClass`... 639 of the 954 are template instances
(`DynamicVectorClass<T>`, `TClassFactory<T>`, ...), 47 are keyboard commands
(`*CommandClass`), 25 are rules types (`*TypeClass`).

**Locomotion is COM.** The 11 locomotors (`DriveLocomotionClass`,
`HoverLocomotionClass`, `JumpjetLocomotionClass`, ...) each have a
`TClassFactory<T>`, and WinMain registers 78 class objects with
`CoRegisterClassObject` at startup. A unit's movement is created through
`CoCreateInstance` by CLSID. native32 passes those calls to real ole32, and
the factories are lifted code, so this works without anything special.

## The function catalog

```
$ py -3 ..\tools\tools\disasm\disasm32.py game\gamemd.exe -o work\functions.json --seed-functions work\rtti_seeds.json
[*] Functions: 22682  (thunks=51, leaves=8906)
[*] Instructions: 2,823,399
[*] Byte coverage: 3,615,767 / 4,063,232 (89.0% of code range)
real	14m54.640s
```

That is with pcrecomp's padding fix (bringup.md, 4). Before it, 23,248
functions and 516 entries starting inside alignment padding, 66 of them hiding
the real function behind.

## Tiberian Sun, for later

The same Steam library has *Tiberian Sun* + *Firestorm*. A first look:

| | Tiberian Sun `Game.exe` | Yuri's Revenge `gamemd.exe` |
|---|---|---|
| Built | 2000-06-05 | 2001-10-31 |
| Code | 2,920,448 bytes | 4,063,232 bytes |
| Protection | none | none |
| RTTI classes | 807 | 954 |
| Class names in common | 744 | 744 |

92% of Tiberian Sun's classes have a namesake in Yuri's Revenge. `SUN.EXE` is
the launcher (with an encrypted `.bind` loader section); `Game.exe` is the game.
