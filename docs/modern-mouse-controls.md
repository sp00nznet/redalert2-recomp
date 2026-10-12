# Modern mouse controls

## Purpose and scope

The F10 menu offers **Classic** and **Modern** mouse controls in both Red Alert
2 and Yuri's Revenge. Classic remains the default. The presenter saves the
choice in `ra2.ini` as `mousecontrols=classic` or `mousecontrols=modern`.

Modern mode changes battlefield input; it does not change `rules.ini` or
`rulesmd.ini`. Each game uses its own lifted hook addresses and engine globals.
The native Linux host has no F10 menu: its hooks pass the game's input through
unchanged, so it always plays with Classic controls.

## Behavior

| Input | Modern behavior |
| --- | --- |
| Left-click a unit or building | Select it. |
| Left-drag | Box-select. |
| Left-click open battlefield ground | Clear selection without issuing an order. |
| Right-click a battlefield cell or object | Issue the context-sensitive order. |
| Right-click a unit | Issue its contextual order if the game offers one; never select the clicked unit or change the current selection. |
| Right-drag | Pan the camera using the original game behavior. |
| Left-click the minimap | Center the camera on that location. |
| Right-click the minimap | Issue the selected units' order at that location. |
| Right-click an already selected deployable unit | Deploy it. |
| Right-click an already selected building | Make it the primary building. |
| Double-left-click a unit | Select all visible units of the same type, like one press of `T`. |
| Shift + left-click an unselected unit | Add it to the current selection. |
| Shift + double-left-click a unit | Add all visible units of the same type to the current selection. |
| Shift + left-drag a selection rectangle | Add the units in the rectangle to the current selection. |
| Left-click a unit already in a group selection | Replace the group with that unit. |

Active placement and order modes retain their original right-click behavior.
The F10 shortcut on the presenter bars and menu/interface clicks are not
remapped.

**Known tradeoff:** because a right-click never selects, double-right-clicking
an *unselected* deployable unit or building no longer deploys it or makes it
primary. Select it first, then right-click it. No replacement shortcut is
planned.

**Known limitation:** double-left-click posts the `T` key to the game. If
"select same type" is rebound in `Keyboard.ini`, Modern double-click follows
whatever command `T` is bound to instead.

## Implementation notes

### Presenter and patch ownership

`src/runtime/present.c` builds and saves the F10 choice for both targets. It
coordinates the physical button state with the mouse messages delivered to the
game; `src/runtime/input.c` provides the matching `GetKeyState` and
`GetAsyncKeyState` behavior. Keep lifted-game changes in `run_lift.py`
(`MODERN_MOUSE_TARGETS` and `modern_mouse_patches`). `src/recomp/gen/` and
`src/recomp/gen_game/` are generated and ignored by Git. The Linux host
defines the same hook functions as pass-throughs in `src/linux/main.c`.

### Battlefield event path

`run_lift.py` patches the battlefield mouse dispatcher (`0x00693107` in Yuri's
Revenge) to route a short Modern right-click through the game's left-button
bookkeeping, except when a placement or order mode needs its native right-up
behavior. If the right-button gesture crosses the Windows drag threshold, the
synthetic left press is released without a click and the original right-drag
camera pan takes over.

The mouse-up hook at `0x00693271` runs after `DecideAction` and rewrites the
resolved action:

- **Routed right-click.** If the action is `None` (0), `Select` (7) or
  `ToggleSelect` (8), the hook passes `None` and also clears the target slot
  that `LeftMouseButtonUp` receives. Replacing the action alone is not enough:
  `LeftMouseButtonUp` still selects a selectable, unselected target when the
  action is `None` (branch at `0x004ABDE1`). With no action and no target, it
  takes its plain exit at `0x004AC294`. Every other action, such as Move (1),
  passes through unchanged. The synthetic left release that hands a right-drag
  to the camera pan is ignored the same way, so a pan that starts over a unit
  does not select it. The mouse-down side needs no guard: `ConvertAction` and
  slot 45 only update the cursor and arm drag-selection.
- **Left-click on open ground.** With no target, any action other than
  `Select`/`ToggleSelect` becomes a call to the selection-clear helper
  (`sub_0048DC90`) followed by `None`, so no order is issued.
- **Left-click on an object.** `Move` becomes `Select`; other non-selection
  actions become `None`, which selects a selectable target through the branch
  above and never issues an order.

The right-up patch at `0x004AAE6E` stops the game's ordinary right-up cleanup
from clearing the selection after a pan.

The Yuri's Revenge lifted `DisplayClass` path is:

| Address | YRpp name | Role |
| --- | --- | --- |
| `0x00692300` | `ProcessClickCoords` | Resolve screen coordinates to map cell and target. |
| `0x00692610` | `DecideAction` | Classify the cell or object into an `Action`. |
| `0x004AAE90` | `ConvertAction` | Update cursor/action presentation. |
| `0x004AC310` | — (vtable slot 45) | On press, record the press point and arm drag-selection. |
| `0x004AC380` | `LeftMouseButtonDown` (vtable slot 47) | Update press and drag-selection state. |
| `0x004AB9B0` | `LeftMouseButtonUp` (slot 48) | Execute the resolved action. |
| `0x004AAD30` | `RightMouseButtonUp` (slot 49) | Cancel active modes; its ordinary fallback clears selection. |
| `0x0048DC90` | — | Clear the current selection by calling each object's `Deselect()`. |

The base-game lift has the same behavior on corresponding paths, with its own
addresses and selected-object globals:

| Role | Yuri's Revenge | Red Alert 2 |
| --- | --- | --- |
| Mouse message dispatch / right-click remap | `0x00693107` | `0x0066A467` |
| Minimap button flags | `0x006539D3` (stack `+0x48`) | `0x0062FE33` (stack `+0x44`) |
| Mouse-up resolved action | `0x00693271` | `0x0066A5D1` |
| Right-up selection cleanup | `0x004AAE6E` | `0x0049B0CE` |
| Box-selection cleanup | `0x004ABCD4` | `0x0049BE44` |
| Target-object cleanup | `0x004ABE4A` | `0x0049BFB4` |
| Selection-clear helper | `0x0048DC90` | `0x00482BC0` |

### Same-type and additive selection

Modern double-left-click on a unit posts the game's `T` key, reusing the
game's same-type selection rules. The action hook checks the target object's
vtable to limit this to unit, infantry, and aircraft objects; it does not
depend on the resolved action, which can vary with the current selection.

Shift-click and Shift-double-click record a timestamp; the next call to the
shared selection-clear helper within one second skips its deselection loop
once, so the clicked unit or same-type group is added to the existing
selection.

When a plain left-click targets a unit that is already in a multi-unit
selection, the hook checks membership in the game's selected-object vector and
returns `Select`, whose single-object path clears the group and selects the
clicked unit.

For Shift box selection, the presenter's window procedure latches `MK_SHIFT`
from the mouse messages, because the selection hooks run later on the game
thread. When Shift was held at the start of the drag or became held during it,
the box-selection and target-object clear calls are skipped
(`ra2_modern_preserve_shift_selection`), as is the shared helper's deselection
loop (`ra2_modern_preserve_global_selection_clear`); the helper's other cleanup
still runs.

### Minimap event path

The minimap uses `RTacticalClass@RadarClass::virtual_31` at `0x006539D0`.
Its first byte argument carries left-button flags `0x01`/`0x04` and
right-button flags `0x10`/`0x40`. The Modern patch at `0x006539D3` swaps the
low and high nibbles, then writes the mapped byte back to the stack argument.
Both the register and stack value matter: later branches in the same handler
read the original argument directly. The helper also keeps the live button
state consistent with the mapped radar event.

The lifted function names and callback roles were matched against the
community-maintained [YRpp DisplayClass.h](https://github.com/Ares-Developers/YRpp/blob/master/DisplayClass.h),
[MapClass.h](https://github.com/Ares-Developers/YRpp/blob/master/MapClass.h), and
[GeneralDefinitions.h](https://github.com/Ares-Developers/YRpp/blob/master/GeneralDefinitions.h).

## Manual verification

Checked by hand in Yuri's Revenge. The Red Alert 2 port builds from its own
lift but has not been checked in game. The scripted playtest harness does not
yet assert Modern-mode selection, minimap, or right-click behavior.

| Case | Status |
| --- | --- |
| Select a unit with left-click; box-select with left-drag | Verified. |
| Clear selection with left-click on open ground | Verified, including mixed land/naval selection. |
| Left-click a friendly unit while another is selected | Verified: selects the target without issuing Move. |
| Left-click a naval yard with a selected ship | Verified: does not trigger repair. |
| Left-click an enemy turreted building with an attacking unit selected | Verified: does not issue an attack order. |
| Issue an order with right-click | Move verified; other target types need more coverage. |
| Right-click a unit; start a right-drag over a unit | Appears to work: selection unchanged. Not covered exhaustively. |
| Left-click or right-click the minimap | Verified: left-click recenters; right-click issues an order. |
| Right-click drag, including repeated drags and pan direction | Verified: camera pans correctly and keeps the selection. |
| Right-click an already selected deployable unit/building | Not yet checked. |
| Double-left-click a unit to select visible units of its type | Appears to work; needs more coverage. |
| Shift-click a unit; Shift-double-click a different unit type with another group selected | Verified. |
| Shift-click a unit that is already in a group selection | Not yet checked. |
| Shift + left-drag a selection rectangle | Verified: keeps the existing selection and adds the boxed units, with Shift held before or after the drag starts. |
| Left-click a unit already in a group selection | Verified: replaces the group with the clicked unit. |
| Special placement/order modes and interface clicks | Verified for the cases tried; not exhaustive. |
