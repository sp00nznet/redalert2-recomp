# Modern mouse controls

## Purpose and scope

The F10 menu offers **Classic** and **Modern** mouse controls for Yuri's
Revenge. Classic remains the default. The presenter saves the choice in
`ra2.ini` as `mousecontrols=classic` or `mousecontrols=modern`.

Modern mode changes battlefield input; it does not change `rules.ini` or
`rulesmd.ini`. The implementation covers the Yuri's Revenge presenter build.
Red Alert 2 and the native Linux host are not wired to this option.

## Behavior

| Input | Modern behavior |
| --- | --- |
| Left-click a unit or building | Select it. |
| Left-drag | Box-select. |
| Left-click open battlefield ground | Clear selection without issuing Move. |
| Right-click a battlefield cell or object | Issue the context-sensitive order. |
| Right-drag | Pan the camera using the original game behavior. |
| Left-click the minimap | Center the camera on that location. |
| Right-click the minimap | Issue the selected units' order at that location. |
| Hover the minimap | Do not issue an order. |

Active placement and order modes retain their original right-click behavior.
The F10 shortcut on the presenter bars and menu/interface clicks are not
remapped.

## Implementation notes

### Presenter and patch ownership

`src/runtime/present.c` builds and saves the F10 choice. It coordinates the
physical button state with the mouse messages delivered to the game;
`src/runtime/input.c` provides the matching `GetKeyState` and
`GetAsyncKeyState` behavior. Keep lifted-game changes in `run_lift.py`.
`src/recomp/gen/` is generated and ignored by Git.

### Battlefield event path

`run_lift.py` patches the Yuri's Revenge dispatcher at `0x00693107` to route a
short Modern right-click through the game's left-button bookkeeping, except
when a placement or order mode needs its native right-up behavior. If the
right-button gesture crosses the Windows drag threshold, the original
right-drag camera pan continues.

The left-click patch at `0x00693271` uses the game's resolved action and target.
When an unmodified left-click on open ground resolves to `Move`, it calls
`sub_0048DC90` to clear selection instead of issuing that move. Unit/object
clicks and contextual right-click orders continue through the game's action
handler. The right-drag release patch at `0x004AAE6E` prevents the game's
ordinary right-up cleanup from clearing selection after a pan.

The relevant lifted `DisplayClass` path is:

| Address | YRpp name | Role |
| --- | --- | --- |
| `0x00692300` | `ProcessClickCoords` | Resolve screen coordinates to map cell and target. |
| `0x00692610` | `DecideAction` | Classify the cell or object into an `Action`. |
| `0x004AAE90` | `ConvertAction` | Update cursor/action presentation. |
| `0x004AC380` | `LeftMouseButtonDown` (vtable slot 47) | Update press and drag-selection state. |
| `0x004AB9B0` | `LeftMouseButtonUp` (slot 48) | Execute the resolved action. |
| `0x004AAD30` | `RightMouseButtonUp` (slot 49) | Cancel active modes; its ordinary fallback clears selection. |
| `0x0048DC90` | — | Clear the current selection by calling each object's `Deselect()`. |

### Minimap event path

The minimap uses `RTacticalClass@RadarClass::virtual_31` at `0x006539D0`.
Its first byte argument carries left-button flags `0x01`/`0x04`, right-button
flags `0x10`/`0x40`, and hover flag `0x88`. The Modern patch at `0x006539D3`
swaps the low and high nibbles, then writes the mapped byte back to the stack
argument. Both the register and stack value matter: later branches in the same
handler read the original argument directly. The symmetric hover flag remains
unchanged. The helper also keeps the live button state consistent with the
mapped radar event.

The lifted function names and callback roles were matched against the
community-maintained [YRpp DisplayClass.h](https://github.com/Ares-Developers/YRpp/blob/master/DisplayClass.h),
[MapClass.h](https://github.com/Ares-Developers/YRpp/blob/master/MapClass.h), and
[GeneralDefinitions.h](https://github.com/Ares-Developers/YRpp/blob/master/GeneralDefinitions.h).

## Manual verification

Manual in-game verification:

| Case | Status |
| --- | --- |
| Select a unit with left-click; box-select with left-drag | Verified. |
| Clear selection with left-click on open ground | Verified, including mixed land/naval selection. |
| Left-click a friendly unit while another is selected | Selects the target without issuing Move. |
| Left-click a naval yard with a selected ship | Does not trigger repair. |
| Issue an order with right-click | Move verified; other target types, including turreted enemy buildings, need more coverage. |
| Left-click or hover the minimap; right-click the minimap | Recenters, issues an order, and does not order on hover. |
| Right-click drag, including repeated drags and pan direction | Camera pans correctly and preserves selection. Verified. |
| Double-left-click a deployable unit or building | Requested behavior; not yet verified in Modern mode. |
| Special placement/order modes and interface clicks | Verified for the reported cases; not exhaustive. |

The in-game checks are manual. The scripted playtest harness does not yet
assert Modern-mode selection, minimap, or right-click behavior.
