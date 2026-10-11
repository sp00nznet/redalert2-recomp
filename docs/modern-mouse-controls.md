# Modern mouse controls

**Status:** the optional Modern mode is implemented for the Yuri's Revenge
presenter build. The user reports that the latest minimap routing appears to
work; further in-game checks are ongoing. This document records the target,
implementation, investigation, and manual test cases.

## Goal

Add an optional modern mouse-control mode to the presenter's F10 menu. Keep
classic Red Alert 2 controls as the default and preserve the selected mode in
the presenter's `ra2.ini` settings.

The requested target is:

- Left-click a unit or building to select it; left-drag to box-select.
- Left-click open battlefield ground to deselect.
- Right-click to issue a context-sensitive order, such as move or attack.
- Preserve keyboard modifiers and the game's special orders where practical.

This is a runtime input feature, not a `rules.ini`/`rulesmd.ini` content mod.

## Reference behavior: confirmed in game

The user verified the target behavior in C&C 3 in game: left-click selects,
left-clicking empty ground deselects, and right-click issues the move/order.
Treat these as the acceptance criteria for the optional mode. The *Kane's
Wrath* manual corroborates the left-click selection and right-click move or
attack split; it also documents `A + right-click` for attack-move and
right-click-drag for rapid camera scrolling. Its controls table does not list
empty-ground deselection, and lists Escape as opening the pause menu; the
user's direct in-game confirmation fills that gap.

Reference: [Kane's Wrath PC manual, Complete Controls (page 2)](https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/24810/manuals/CC3KWpcMANfinal.pdf?t=1741704747)

## How this differs from Red Alert 2

The Red Alert 2 manual describes the legacy C&C scheme: left-click selects
units and gives orders, while right-click deselects and cancels orders. That
means the requested C&C3-style behavior is a change to game command semantics,
not a left/right swap. A blind swap would make left-clicking a unit deselect it.

Reference: [Red Alert 2 PC manual, The Game Mouse](https://static.wikia.nocookie.net/cnc_gamepedia_en/images/3/3b/CCUC-RA2-pcMANOLeUS.pdf/revision/latest)

## What the checkout establishes

- The Windows presenter receives left, right, and middle mouse events and
  forwards them to the game's window: `src/runtime/present.c`, in
  `wndproc()` and `forward_mouse()`.
- Input is currently forwarded with its original mouse message. The game also
  reads button state through `GetKeyState`/`GetAsyncKeyState`, which the host
  answers in `src/runtime/input.c` and `src/runtime/host.c`. Any remapping must
  keep messages and polled button state consistent.
- A right-click on the presenter bars opens the settings menu. A right-click
  inside the game picture is forwarded to the game. Modern mode must retain
  this distinction.
- F10 settings are assembled and persisted by `settings_menu()` and
  `settings_save()` in `src/runtime/present.c`; this is a natural place for an
  opt-in mode selector. The presenter and lifted game run in the same host
  process (`src/runtime/input.c`), so the menu can expose a small runtime
  getter for a source-controlled `run_lift.py` patch to read. This avoids
  encoding the control mode in the game's unrelated `RA2MD.INI`.
- `work/rtti.json` identifies `MouseClass` and `WWMouseClass` in the lifted
  game. Their generated functions in `src/recomp/gen/recomp_*.c` still have
  address-based `sub_...` names. The exact `DisplayClass` click callbacks and
  action classifier are identified by matching the lifted vtable to YRpp.
  The game message dispatcher also connects left-button-up and right-button-up
  messages to these paths. The numeric `Action` values used in the classifier
  are mapped to YRpp names, including move, attack, select, and special actions.
- The lifted game has a real Windows message path: `WWMouseClass` methods call
  `GetMessage`, and a lifted message-handling block at `0x0061D383` compares
  against `WM_LBUTTONDOWN` (`0x201`) in `src/recomp/gen/recomp_0034.c`. The
  presenter forwards these messages into the game window. The battlefield
  dispatch is separately traced through `sub_006930A0` below.
- `src/recomp/gen` is generated and not committed. `run_lift.py` has explicit
  source-controlled patch hooks for behavior changes, so a lifted-game change
  has a plausible maintainable home once the responsible routine is found.
- The current scripted playtest harness exercises left-click, drag, and
  modifier-click input. It does not yet express right-click orders or assert
  selection/deselection behavior (`tools/playtest.py`, `docs/testing.md`).

## Feasibility assessment

**The prototype path was feasible and has been implemented.** The presenter
stores the Classic/Modern setting, and the lifted game reads it through a
source-controlled `run_lift.py` patch hook. The battlefield resolver provides
the clicked object/cell and named action; the existing left-button handler
executes actions, while a separate routine clears selection.

## Implementation and runtime verification

The implementation is in the source tree for Yuri's Revenge:

- F10 has a persistent Classic/Modern mouse-control choice in `ra2.ini`; the
  default remains Classic.
- The lifted battlefield dispatcher routes right-button down/up through the
  existing left-button coordinate and action path only when no active
  placement/order mode is present. A short click issues a contextual order;
  moving past the Windows drag threshold switches to the native right-button
  path so the original camera pan remains available. Active modes keep their
  original right-up cancel path.
- Before the existing left-up callback runs, an unmodified left-click whose
  action is `Move` clears the current selection and suppresses that move.
  Right-click orders retain the computed action.
- During a routed short right-button click, `VK_LBUTTON` is synthesized while
  `VK_RBUTTON` remains visible to the game. When a drag crosses the system
  threshold, the synthetic left press is released before native right-button
  pan begins. This paired handoff fixed the initial Modern-only camera drift.
- The patch lives in `run_lift.py`; regenerated lifted C remains disposable.
  This prototype covers the Yuri's Revenge presenter build. The separate Red
  Alert 2 and native Linux build paths are not wired to it.

The user verified in game that Modern left-click selects, left-clicking open
ground deselects, and right-click moves units. Right-click drag still pans the
camera, and quick consecutive drags work after the handoff fix. The user also
reported that the listed classic-mode and contextual-order checks worked.
These are manual checks; the scripted playtest harness has not been extended.

## Manual left/right-click case ledger

This ledger collects the click and drag cases reported during the manual
Modern-mode playtests. “Verified” means the user reported trying the case in
game; “pending” means the latest code change still needs an in-game check.

| Case | Intended Modern behavior | Reported behavior and current status |
| --- | --- | --- |
| Left-click a unit | Select it. | **Verified working.** Basic left-click selection worked. |
| Left-click a friendly unit while another unit is selected | Select the friendly target; do not issue a move order. | It initially issued Move and sent the selected unit around the target. **Fixed and verified** by the user. |
| Left-click open battlefield ground | Clear selection; do not issue Move. | Basic deselection is **verified working**. With a mixed land/naval selection, it initially issued Move; the user later reported mixed selection was good. **Fixed and verified.** |
| Right-click a battlefield cell/object | Issue the context-sensitive order, including Move or attack. | Right-click movement was **verified working**. Other target classes, including turreted buildings, are not fully verified. |
| Left-click a naval yard with a selected ship | Do not issue the naval-yard repair order; keep left-click semantics for selection/deselection. | It initially accepted a left-click repair order. The user later reported “Naval yard ok.” **Fixed and verified.** |
| Double-left-click a deployable unit | Run its deploy action, preserving the original double-click behavior. | The current left-click filter suppresses non-selection actions on objects, so it may block deploy. **Requested for the next iteration; not yet verified.** |
| Double-left-click a building | Make that building primary, preserving the original double-click behavior. | **Requested for the next iteration; not yet verified** with Modern controls. |
| Click or hover the minimap with one selected land or naval unit | Left-click recenters the camera; right-click issues the unit order. Hovering alone must not issue an order. | The initial event-register swap had no effect because later code read the original stack argument. Rewriting that argument fixed the routing. The user reports the latest build appears to work; further playtesting is ongoing. |
| Left-click a turreted building | Follow the C&C3-style left-click selection/deselection behavior; reserve contextual attacks for right-click. | The user reported a left-click was accepted on a turreted building and right-click also worked. They later could not test attacking an enemy building, so the target ownership and resulting action remain unclear. **Pending clarification by in-game retest.** |
| Short right-click | Issue the contextual order. | **Verified working.** |
| Right-click drag | Pan the camera and preserve the current selection. | The initial remap treated the drag like a left drag. The handoff was changed to restore native camera pan; pan then worked, including repeated quick drags. A later test found that ending a pan cleared selection. The latest build suppresses that release deselection. **Pan verified; selection preservation pending in-game verification.** |
| Right-drag pan direction and start | Pan in the direction dragged without an initial jump. | Modern mode initially drifted toward a screen corner; Classic mode did not. The handoff/coordinate changes improved it, and the user later said “That did it.” **Reported resolved.** The choppy feel was compared with Classic and disregarded as normal game behavior. |
| Two quick right-drag pans | Start each pan reliably. | A second quick drag sometimes failed to register during the drift investigation. The later handoff fix was reported to support quick consecutive drags. **Reported resolved.** |
| Special placement/order modes and interface clicks | Keep their original cancel and interface behavior. | The user reported the listed checks worked. **Verified for the cases tested**, not every mode or interface target. |

## Minimap input: investigation and fix

The minimap has a separate radar event handler at `0x006539D0`, identified by
the local RTTI map as `RTacticalClass@RadarClass`. Tracing showed left-button
event flags `0x01` and `0x04`, right-button flags `0x10` and `0x40`, and the
hover flag `0x88`. Swapping the low and high nibbles leaves hover unchanged.

The first patch changed the event byte in a register, but later branches in
the same lifted function read the original byte again from the stack argument.
So the logged swap ran while the handler still chose actions from the
unmodified flags. The source-controlled patch in `run_lift.py` now writes the
mapped byte back to that argument as well. This keeps the radar's later
dispatch decisions consistent with the remap and leaves the hover flag intact.

The user tested the resulting build and reported that the minimap behavior
appears to work. Further playtesting should confirm that hover never issues an
order, left-click recenters the camera, right-click issues the unit order, and
right-drag still pans without clearing selection. Earlier attempts caused a
hover crash or repeated Move orders; those failures led to the current
callback-argument fix. Bounded `[modern-radar]` diagnostics remain available
while these checks continue.

## Lifted battlefield path found

The setup output contains enough information to identify the main game-side
seam. YRpp, a community reverse-engineered header project, names the following
`DisplayClass` methods and addresses:

- `ProcessClickCoords` at `0x00692300` resolves screen coordinates into map
  coordinates and an object target, with additional visibility/validity flags.
- `DecideAction` at `0x00692610` classifies a cell/object into an `Action`.
- `ConvertAction` is a virtual that updates the cursor/action presentation.
- `LeftMouseButtonDown`, `LeftMouseButtonUp`, and `RightMouseButtonUp` are
  virtual callbacks on the same class.

These declarations line up with the repo's YR executable and lifted output.
`work/rtti.json` gives `DisplayClass` vtable `0x007E6114`; reading its entries
from `game/gamemd.exe` maps the relevant tail entries to:

| Vtable slot | Lifted address | YRpp declaration |
| ---: | ---: | --- |
| 46 | `0x004AAE90` | `ConvertAction` |
| 47 | `0x004AC380` | `LeftMouseButtonDown` |
| 48 | `0x004AB9B0` | `LeftMouseButtonUp` |
| 49 | `0x004AAD30` | `RightMouseButtonUp` |

The lifted `0x004AB9B0` body is substantial and takes the five stack arguments
expected for `LeftMouseButtonUp`: coordinates, cell, object, action, and an
additional flag. Its code branches on the object/action inputs and invokes
game-object callbacks. This is where current left-button click behavior is
implemented, after the screen point has been resolved. `0x00692300` and
`0x00692610` are present as separate lifted functions with the same addresses
and are called together by the surrounding display logic. The `0x004AC380`
down callback reads and updates the `DisplayClass` drag-selection state.

This identifies the seam used by the implementation: branch on the mode in
battlefield click handling, reuse the point resolver and action classifier,
and call the selection-clear routine for open-ground left clicks. Named
action values clarify the order paths. Manual checks cover the requested
behavior; unusual target classes and untested combinations may still need
follow-up verification.

## Mouse message dispatch traced

The presenter forwards mouse messages to the game window, and the lifted
window/input path eventually calls `sub_006930A0` in
`src/recomp/gen/recomp_0039.c`. That routine maps standard Windows button
messages through a dispatch table:

| Message | Lifted route | What the route does |
| --- | --- | --- |
| `WM_LBUTTONDOWN` (`0x0201`) | coordinate/action resolution, `ConvertAction` (`0x004AAE90`), then `0x004AC310` | Updates click/drag-related state. This is not the vtable slot YRpp names `LeftMouseButtonDown` (`0x004AC380`), so the down-event mapping needs care. |
| `WM_LBUTTONUP` (`0x0202`) | coordinate/action resolution, then `LeftMouseButtonUp` (`0x004AB9B0`) | Calls the substantial five-argument handler with resolved cell/object/action data. This is the confirmed left-click behavior seam. |
| `WM_RBUTTONDOWN` (`0x0204`) | right-button coordinate checks, `0x004AAD20`, state updates | `0x004AAD20` is an empty return stub; this route does not issue the main contextual order. |
| `WM_RBUTTONUP` (`0x0205`) | right-button state checks, then `RightMouseButtonUp` (`0x004AAD30`) in the matching-state path | The callback cancels active placement/repair/sell/power-style modes. Its ordinary fallback calls `sub_0048DC90` to deselect the current selection. If the stored right-button state differs, the dispatcher invokes `MouseClass::UpdateCursor` through vtable offset `+0x48`. |

The left-up route calls `ProcessClickCoords` (`0x00692300`) and
`DecideAction` (`0x00692610`) before `LeftMouseButtonUp`. YRpp's
`GeneralDefinitions.h` maps the numeric action values to names. The key values
visible in the handler are `Move = 1`, `Attack = 5`, `Select = 7`,
`ToggleSelect = 8`, `Sell = 12`, `NoSell = 14`, `NoRepair = 15`,
`TogglePower = 33`, `NoTogglePower = 34`, `PlaceWaypoint = 42`,
`PlaceBeacon = 60`, and `AttackMoveTar = 63`. The action classifier calls the
target object's `MouseOverCell` or `MouseOverObject` virtual where a target is
present; both return the context-sensitive `Action` type. The order handler
has explicit `Move` and `NoMove` branches and sends other named actions through
object callbacks.

The clear-selection route is now identified rather than inferred from the
right-click callback name. `RightMouseButtonUp` (`0x004AAD30`) cancels active
placement/repair/sell/power-style modes. Its ordinary fallback calls
`sub_0048DC90`, which walks the current selection collection and calls each
object's `Deselect()` virtual (`0x005F44A0` in the local YR executable); the
object method removes itself from that collection and clears its selected
flag. The separate right-button `+0x48` virtual route resolves to
`MouseClass::UpdateCursor`, so it is a cursor-state path, not a hidden order
handler. Right-button-up itself does not issue ordinary move/attack orders.

The implementation sends modern right-click through the existing resolver
and left-up order handler, while open-ground left-click uses the existing
deselection routine. In-game checks confirmed the requested interaction and
the tested contextual orders; this does not establish every possible target
and modifier combination.

Reverse-engineered declarations used for this mapping: [YRpp DisplayClass.h](https://github.com/Ares-Developers/YRpp/blob/master/DisplayClass.h), [YRpp MapClass.h](https://github.com/Ares-Developers/YRpp/blob/master/MapClass.h), [YRpp GScreenClass.h](https://github.com/Ares-Developers/YRpp/blob/master/GScreenClass.h), [YRpp ObjectClass.h](https://github.com/Ares-Developers/YRpp/blob/master/ObjectClass.h), and [YRpp GeneralDefinitions.h](https://github.com/Ares-Developers/YRpp/blob/master/GeneralDefinitions.h). These are community-maintained declarations, not official EA source.

## Investigation outcome

1. **Dispatch trace: complete for the selected seam.** Left-up reaches the
   resolver and action handler; right-up cancels modes or deselects. The
   state-dependent alternate right route updates the cursor. The left-down
   helper still differs from the `LeftMouseButtonDown` vtable callback, but
   left-down already preserves click/drag selection state and did not need to
   change. Right-drag was verified in game.
2. **Action trace: sufficient for the implementation.** Numeric values are mapped to
   YRpp's `Action` enum, the order handler has explicit move/no-move branches
   and callbacks for other actions, and the source-level selection clear path
   is confirmed. Per-target runtime outcomes and interactions with active
   modes/modifiers outside the reported checks may need additional testing.
3. **Prototype implemented and built.** The optional F10 setting routes only
   battlefield events in Modern mode and preserves the classic route when
   disabled. Active-mode cancellation and presenter/menu behavior were
   included in the runtime checks.

## Scope and limitations

- The F10 setting is labeled **Classic** and **Modern**, defaults to Classic,
  and persists in the presenter's settings.
- The setting lives in the presenter; the semantic change runs in the traced
  lifted-game handler. Menu/dialog clicks and the right-click-on-bars settings
  shortcut are not remapped.
- `WM_*BUTTON` messages and polled `VK_*BUTTON` state are kept in agreement.
- The implementation covers the Yuri's Revenge presenter build. `--classic`,
  Red Alert 2, and the native Linux host are outside this change.

## Verification

The build completed successfully. The user manually verified in game:

- Classic mode retains its original mouse behavior.
- Modern mode selects with left-click, deselects on open ground, and sends
  contextual right-click orders.
- Right-click drag pans the camera, including after repeated quick drags.
- The tested special modes and interface interactions remained functional.
- The option persists across restart in Yuri's Revenge. Red Alert 2 support is
  outside this implementation.

No automated tests were added or run. The separate Red Alert 2 and native
Linux build paths are outside this implementation.

## Modern minimap input iteration

The minimap uses a separate radar event handler at `0x006539D0`; the
battlefield mouse remapping does not cover it. Runtime tracing showed the
handler receives left-button event bits `0x01` and `0x04`, right-button bits
`0x10` and `0x40`, and a hover event `0x88`. The button bits are arranged in
matching low/high nibbles, so Modern mode now swaps those nibbles on the
handler's first event byte. This reverses radar left/right behavior while
leaving hover `0x88` unchanged. Classic mode passes the event byte through
unchanged.

The previous attempted radar patch changed a later action byte; hover then
looked like an order and caused the continuous move-command bug. That patch was
removed. The handler-entry flag swap is confirmed by runtime logs, but the user
reported that it did not change the click behavior. The radar handler also
consults live button state, so Modern mode now swaps both the event flags and
the live left/right button state while a radar button is held. The live state
swap clears when neither button is held. Classic mode passes both through
unchanged.

The current `build/ra2.exe` completed an incremental runtime build and link.
The radar behavior still needs in-game verification. Check that hovering over
the radar issues no orders, left-clicking recenters the camera, and right-
clicking issues a unit order.
