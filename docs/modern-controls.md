# Modern controls

The F10 menu's **Controls** entry offers **Classic** and **Modern** in both
Red Alert 2 and Yuri's Revenge. Classic, the default, keeps the original games'
controls. Modern brings two changes:

- **Mouse**: left-click selects, right-click orders, with modern selection
  shortcuts.
- **Queue Five** (`RA2-QOL-001`): Shift-click a build icon in the sidebar to
  queue five of that item.

The presenter saves the choice in `ra2.ini` as `controls=classic` or
`controls=modern`. Neither mode changes `rules.ini` or `rulesmd.ini`. Each
game uses its own lifted hook addresses and engine globals. The native Linux
host has no F10 menu: its hooks pass the game's input through unchanged, so it
always plays with Classic controls.

## Mouse

### Behavior

| Input | Modern behavior |
| --- | --- |
| Left-click a unit or building | Select it. |
| Left-drag | Box-select. |
| Left-click open battlefield ground | Clear selection without issuing an order. |
| Shift + left-click open battlefield ground | Keep the selection; Shift only ever adds. |
| Right-click a battlefield cell or object | Issue the context-sensitive order. |
| Right-click a unit | Issue its contextual order if the game offers one; never select the clicked unit or change the current selection. |
| Right-drag | Pan the camera using the original game behavior. |
| Left-click the minimap | Center the camera on that location. |
| Right-click the minimap | Issue the selected units' order at that location. |
| Right-click an already selected deployable unit | Deploy it. |
| Right-click an already selected building | Make it the primary building. |
| Double-left-click a unit | Select all visible units of the same type, like one press of `T`. |
| Shift + left-click an unselected unit | Add it to the current selection. |
| Shift + left-click a selected unit or building | Remove it from the current selection. |
| Shift + double-left-click a unit | Add all visible units of the same type to the current selection. |
| Shift + left-drag a selection rectangle | Add the units in the rectangle to the current selection; if every one of them is already selected, remove them instead. |
| Left-click a unit already in a group selection | Replace the group with that unit. |

Active placement and order modes retain their original right-click behavior.
The F10 shortcut on the presenter bars and menu/interface clicks are not
remapped.

**Known tradeoff:** because a right-click never selects, double-right-clicking
an *unselected* deployable unit or building no longer deploys it or makes it
primary. Select it first, then right-click it. No replacement shortcut is
planned.

### Implementation notes

#### Presenter and patch ownership

`src/runtime/present.c` builds and saves the F10 choice for both targets. It
coordinates the physical button state with the mouse messages delivered to the
game; `src/runtime/input.c` provides the matching `GetKeyState` and
`GetAsyncKeyState` behavior. Keep lifted-game changes in `run_lift.py`
(`MODERN_CONTROLS_TARGETS` and `modern_controls_patches`). `src/recomp/gen/` and
`src/recomp/gen_game/` are generated and ignored by Git. The Linux host
defines the same hook functions as pass-throughs in `src/linux/main.c`.

#### Battlefield event path

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
  (`sub_0048DC90`) followed by `None`, so no order is issued. With Shift held
  it becomes a plain `None` instead and the selection is kept.
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
| After `LeftMouseButtonUp` returns | `0x00693290` | `0x0066A5F0` |
| Same-type selection key down / key up | `0x00732CA0` / `0x00732CC0` | `0x006F7650` / `0x006F7670` |
| Right-up selection cleanup | `0x004AAE6E` | `0x0049B0CE` |
| Box-selection cleanup | `0x004ABCD4` | `0x0049BE44` |
| Target-object cleanup | `0x004ABE4A` | `0x0049BFB4` |
| Selection-clear helper | `0x0048DC90` | `0x00482BC0` |

#### Same-type and additive selection

Modern double-left-click on a unit runs the game's own same-type selection
(`TypeSelectCommandClass`, the `T` command), so its on-screen and ownership
rules apply. The action hook only flags the request; a second hook just after
`LeftMouseButtonUp` returns calls the command's key-down and key-up halves,
as one tap of its key would. It does not depend on the key binding in
`Keyboard.ini`. The action hook checks the target object's vtable to limit
this to unit, infantry, and aircraft objects; it does not depend on the
resolved action, which can vary with the current selection.

Shift-click and Shift-double-click set a one-shot flag: the shared
selection-clear helper skips its deselection loop once, so the clicked unit or
same-type group is added to the existing selection. The after-mouse-up hook
drops the flag when the click is done, so it never carries over to a later
click.

A Shift-click on an object that is already selected returns `ToggleSelect`
(8). `LeftMouseButtonUp` handles it at `0x004ABD24` by calling only that
object's `Deselect()` (vtable `+0x150`).

When a plain left-click targets a unit that is already in a multi-unit
selection, the hook checks membership in the game's selected-object vector and
returns `Select`, whose single-object path clears the group and selects the
clicked unit.

For Shift box selection, the presenter's window procedure latches `MK_SHIFT`
from the mouse messages, because the selection hooks run later on the game
thread. A press only counts as a drag once it moves past the Windows drag
distance (`SM_CXDRAG`/`SM_CYDRAG`), so a click that wobbles a pixel is still a
click. When Shift was held at the start of the drag or became held during it,
the box-selection and target-object clear calls are skipped
(`ra2_modern_preserve_shift_selection`), as is the shared helper's deselection
loop (`ra2_modern_preserve_global_selection_clear`); the helper's other cleanup
still runs.

A Shift box whose units are all selected already removes them. The box walk
(`0x006D9FF0` in Yuri's Revenge, `0x006AA610` in Red Alert 2) calls a callback
for each object in the rectangle, and the callback calls the object's
`Select()`. With Shift held, the hook on the walk call (`0x004ABCEB` /
`0x0049BE5B`) runs the walk twice: first a dry run in which the hook on the
callback's `Select()` (`0x004AC2F1` / `0x0049C470`) only counts the objects and
the unselected ones, then the real walk, in which that hook calls `Deselect()`
when the dry run found every object selected, and `Select()` otherwise. The
walk clears the rectangle it reads (`TacticalClass` `+0xD90`/`+0xD94`), so the
hook puts it back between the two walks.

#### Minimap event path

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

## Queue Five

**Feature ID:** `RA2-QOL-001`

Hold **Shift** and click a build icon in the sidebar to queue five of that
item. A plain click keeps the game's normal one-item behavior, and Classic
controls keep it for Shift-click too. It applies to production clicks only;
Shift-right-click to remove from the queue is not included.

The hook adds four copies of the game's own production event to the outgoing
event ring before the original event is added, so the game's normal
production and event-processing path does the rest (`queue_five_patches` in
`run_lift.py`, `src/runtime/queuefive.c`). The hook asks
`ra2_modern_controls()` first and does nothing in Classic. The extra copies stop when the ring
reaches its 128-event capacity.

Both games use the same 128-entry ring and 111-byte event records, at
different addresses:

| Target | Shift-click hooks | Event count | Ring index | Event ring | Timestamps |
| --- | --- | --- | --- | --- | --- |
| Yuri's Revenge (`gamemd.exe`) | `0x006AB511`, `0x006AB76E` | `0x00A802C8` | `0x00A802D0` | `0x00A802D4` | `0x00A83A54` |
| Red Alert 2 (`game.exe`) | `0x0068119E`, `0x006813EF` | `0x00A32338` | `0x00A32340` | `0x00A32344` | `0x00A35AC4` |

These addresses are for the supported shipping executables.

## Manual verification

Checked by hand in both games unless noted. The scripted playtest harness does
not yet assert Modern-mode selection, minimap, or right-click behavior.

| Area | Verified |
| --- | --- |
| Left-click | Selects a unit or building; on open ground clears the selection (including mixed land/naval selections) without issuing Move; on a friendly unit selects it without Move; on a unit in a group replaces the group. With a ship selected, a naval yard does not trigger repair; with an attacker selected, an enemy turreted building does not trigger an attack. |
| Box selection | Left-drag box-selects. Shift-box adds, with Shift pressed before or after the drag starts; over units that are all selected already it removes them; a mixed box only adds; over open ground it keeps the selection. |
| Shift-click | Adds an unselected unit; removes a selected one; on open ground keeps the selection, even when clicked quickly or with a slight wobble. |
| Double-click | Selects the visible units of the clicked type; Shift-double-click adds that type to the current group. |
| Right-click | Issues Move; deploys an already selected deployable unit and makes an already selected building primary; never selects the clicked unit. |
| Right-drag | Pans the camera, including repeated drags and every direction, and keeps the selection; starting over a unit does not select it. |
| Minimap | Left-click recenters; right-click issues the order. |
| Right-click orders | Attack, force-fire and force-move, entering transports and garrisons, engineer capture, harvesting, and depot repair. |
| Special modes | Placement, sell, repair, and superweapon targeting modes keep their right-click cancel; interface clicks are unchanged. |
| Classic mode | Unchanged, and switching between Classic and Modern during a game works. |
| Queue Five | Shift-click on a build icon queues five; a plain click queues one; with Classic controls, Shift-click queues one. |
