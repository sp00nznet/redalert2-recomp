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
| Double-right-click a deployable unit | Deploy it (keeps the existing behavior). |
| Double-right-click a building | Make it the primary building (keeps the existing behavior). |
| Double-left-click a unit | Select all visible units of the same type, like one press of `T`. |
| Shift + left-drag a selection rectangle | Add the units in the rectangle to the current selection. |
| Left-click a unit already in a group selection | Replace the group with that unit. |

Active placement and order modes retain their original right-click behavior.
The F10 shortcut on the presenter bars and menu/interface clicks are not
remapped.

These are explicit Modern-mode choices. Deploy and primary-building selection
remain on double-right-click. Double-left-click is reserved for selecting
same-type units. Same-type selection and additive box selection are implemented;
their in-game behavior is under follow-up after inconsistent results. Selecting
one unit from an existing group is a Modern-mode improvement over the base game.

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

Modern double-left-click on a unit posts the game's `T` key after the click is
handled, reusing the game's same-type selection rules. The lifted action hook
checks the target object's vtable to limit this to unit, infantry, and aircraft
objects; it does not depend on the game's resolved action, which can vary with
the current selection. Double-right-click retains the game's deploy and
primary-building actions.

When a plain left-click targets a unit that is already in a multi-unit
selection, the click hook checks membership in the game's selected-object
vector and routes the click through the normal single-object selection action.
That action clears the group and selects the clicked unit. This behavior is
implemented and verified in-game.

For Shift box selection, the mouse-up path reaches the shared clear routine at
`0x0048DC90`. Modern mode skips its deselection loop when Shift was held at the
start of the drag or became held during it, then continues the routine's normal
cleanup. The presenter's window procedure latches `MK_SHIFT` from mouse
messages because the selection hook runs later in the game thread.

### Minimap event path

The minimap uses `RTacticalClass@RadarClass::virtual_31` at `0x006539D0`.
Its first byte argument carries left-button flags `0x01`/`0x04` and right-button
flags `0x10`/`0x40`. The Modern patch at `0x006539D3`
swaps the low and high nibbles, then writes the mapped byte back to the stack
argument. Both the register and stack value matter: later branches in the same
handler read the original argument directly. The helper also keeps the live
button state consistent with the mapped radar event.

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
| Left-click an enemy turreted building with an attacking unit selected | Does not issue an attack order. Verified. |
| Issue an order with right-click | Move verified; other target types still need more coverage. |
| Left-click or right-click the minimap | Left-click recenters; right-click issues an order. |
| Right-click drag, including repeated drags and pan direction | Camera pans correctly and preserves selection. Verified. |
| Double-right-click a deployable unit to deploy; double-right-click a building to make it primary | Existing behavior; preserve. |
| Double-left-click a unit to select visible units of its type | Latest report: seems to work; consistency still needs more coverage. |
| Shift + left-drag a selection rectangle | Verified by the user: the existing selection is preserved and boxed units are added, with Shift held before or after dragging starts. |
| Left-click a unit already in a group selection | Verified: replaces the group with the clicked unit. |
| Special placement/order modes and interface clicks | Verified for the reported cases; not exhaustive. |

The in-game checks are manual. The scripted playtest harness does not yet
assert Modern-mode selection, minimap, or right-click behavior.
