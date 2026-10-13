# Shift-queued moves

**Feature ID:** `RA2-QOL-002`
**Status:** Proposed; implementation reverted pending a safer input path.

## Goal

With Modern controls, hold **Shift** and right-click the battlefield to
issue a move immediately and append later destinations to that unit's route.
Shift alone should not enable the game's waypoint cursor or play waypoint
sounds. Releasing Shift should finish the queued route. Showing waypoint lines
is optional. The first implementation may wait until Shift is released before
units start moving, but immediate movement is preferred.

## Native planner findings

- `DisplayClass` stores planner mode at offset `0x11B3`.
- `sub_004AC700` changes planner mode. It takes two stack arguments and returns
  with `ret 8`; calling it with the wrong stack layout froze the game.
- Entering planner mode normally calls `sub_0048DC90` at `0x004AC7BC`, which
  clears the active selection. Any reuse of this mode must preserve the units
  being routed.
- The native planner records orders while open and commits them when it closes.
  That does not provide the preferred immediate first move by itself.

## Why the first attempt was reverted

The first Shift hook opened native planner mode from the right-click message.
In-game, Shift-right-click moved a unit immediately instead of appending a
route, and subsequent waypoint behavior did not work. A separate right-click
selection guard also blocked normal move orders because it suppressed action
`1`, which is used by the game's move path. The waypoint hook and its planner
selection override have been removed; do not treat the prior implementation as
a working base.

## Next investigation

Trace the planner's order-recording and commit path independently from the
mouse action resolver. Find a way to add a destination without changing the
normal right-click action into a planner click, and preserve the selected-unit
group while recording it. If native planner mode cannot start the first move
immediately, consider a direct append/commit path or split immediate movement
from later queued destinations. Keep the waypoint sound and cursor effects out
of Shift handling.

This is a Modern controls idea. The planner addresses above are for
Yuri's Revenge (`gamemd.exe`); a Red Alert 2 version would need the matching
`game.exe` addresses. No waypoint implementation is currently active.
