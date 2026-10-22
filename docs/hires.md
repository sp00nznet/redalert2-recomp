# High resolution and widescreen

Yuri's Revenge takes its in-game resolution from `RA2MD.INI`:

```ini
[Video]
ScreenWidth=1920
ScreenHeight=1080
```

The menus stay at 800x600 (the shell resolution); a game, a skirmish or a
mission, opens at the INI's size. On the virtual display any size is accepted
(`SetDisplayMode` is remembered, not applied), and the presenter shows the
picture at its own aspect: a 16:9 game fills a 16:9 window or screen, the 4:3
menus are letterboxed in it.

The menus a battle opens (the pause menu, and its load, save and controls
screens) stay at the game's size, laid out for its height: the art at the
left, as wide as makes 4:3 with the 168-pixel sidebar, and the sidebar at the
right edge, with black between them on a wider screen. While one is open the
presenter leaves that band out, so the menu is 4:3 and letterboxed like the
main menu, and a click right of the cut is moved back across it.

Red Alert 2 starts in the INI's mode, so its main menus are at the game's
size too: an 800-wide dialog centred, with black at the left edge. The band
is cut only from a menu whose art starts at the left edge, as a battle's
menus do in both games. Cut there, a centred menu moved off centre, and where
it reached past the cut (`height * 4 / 3 - 168`: 1272 at 1920x1080, 792 at
1280x720) it was sliced.

## What works

The playtest suite checks each size in a skirmish, and 1080p in a campaign
(`py -3 tools/playtest.py 'res-*'`):

| Resolution | Skirmish | Campaign | Notes |
|---|---|---|---|
| 1280x720 | pass | | |
| 1720x720 | pass | | 21:9, half of 3440x1440 (a whole 2x there, and small enough for HD vehicles); view 1552x688 |
| 1920x1080 | pass | pass (Allied, first mission) | sidebar of 16 cameo slots; view 1752x1048 |
| 2560x1080 | pass | | 21:9; view 2392x1048 |
| 2560x1440 | pass | | |
| 2560x1600 | pass | | 16:10, most Macs' shape (with 1680x1050 in F10's list) |
| 3440x1440 | pass | | 21:9; view 3272x1408 |
| 3840x1600 | pass | | 21:9; view 3672x1568 |
| 3840x2160 | pass | | sidebar rows capped at 30 (below) |

The four 21:9 sizes and 2560x1600 pass in Red Alert 2 as well
(`RA2_TARGET=game`). Nothing wider than the presenter's 4096x2160 frame is
offered, so no 5120x2160 or 32:9.

The game's own Options screen lists only 640x480, 800x600 and 1024x768 unless
`[Video] AllowHiResModes=yes` is in its INI; then it lists every mode the
display offers. On Linux and macOS the host adds that key when it isn't there
(a `no` you set stays), and win32hle offers the sizes above up to 4K and the
display's own size (its logical size on a Retina Mac). On Windows the modes
are your display's, which can be larger than the presenter shows, so the key
is left to you and F10 is the way past 1024x768.

Yuri's Revenge only saves the size picked there, for the next battle. Red
Alert 2 changes to it at once: a box asks to confirm, the menus come back at
the new size, and the same box asks whether to keep it (else the old mode
returns after a few seconds). Off Windows that box was painted under the
Options screen and the game seemed frozen (#24); it needs pcrecomp with
window z-order (`SetWindowPos`). The `options-resolution` playtest walks it.

A larger resolution shows more of the battlefield, not a bigger picture: the
art stays at its native size, and the presenter's sharp scaling is what makes
it large on a 4K screen.

## The 4K limit is RA2's

At 3840x2160 the game faults just after allocating its view buffers:

```
[game] SidebarSurface (168x2160) SYSTEM MEMORY
[game] Set_View_Dimensions(0,0,3672,2128)
[game] Allocating ZBuffer (3672x2128)
=== fault 0xC0000005 at ... read of 0x00000064 in lifted sub_006ABD30
```

The shipping code does the same under `--original`
(`[original] exception 0xC0000005 at 006ABF96 (read 0x00000064)`), so it is not
the lift. `0x006ABF6C` lays out the sidebar's cameo buttons: rows =
`(height - 26 - top) / 50`, two buttons per row, written into a static array
at `0x00B07E80` with 0x38-byte entries. The next object starts at
`0x00B0B328`, which leaves about 240 entries, and at 2160 lines the loop runs
past the end into whatever follows (a null vtable).

## The fix: cap the rows

The lift caps the row count at 30 (`run_lift.py`, `sidebar_rows_patches`):
60 buttons, a tab's share of the array. Every place that computes the rows
divides by 50 the same way (`imul 0x51EB851F`, `sar 4`, `add r, r >> 31`), so
the patch finds each by that shape in the sidebar code that reads its height
(`0x00886F9C`) or top (`0x00B0B4F8`) and adds one line of C after the `add`:

```c
if ((int32_t)eax > 30) eax = 30; /* remaster: sidebar rows, run_lift.py sidebar_rows_patches */
```

It finds 26 sites. Up to 1440 lines the rows are 27 or fewer and nothing
changes; at 2160 the strip is 30 rows and the space below it stays empty.
`--original` runs the shipping code unpatched and still crashes at 4K, which
is the point: the oracle is the original game, the remaster is the lift.
