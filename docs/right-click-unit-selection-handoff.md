# Modern right-click selection fix

## Result

The user reports that the fix appears to work in game. The fresh YR lift had
24,944 functions and zero lift errors, and the executable built successfully.
The full in-game case matrix has not been confirmed.

## Root cause

Modern right-click maps a short physical right-click to synthetic left-button
down/up messages. The game's `LeftMouseButtonUp` handler at `0x004AB9B0`
selects an eligible, unselected target even when the resolved action is
`None` (`0`). This is why changing `Select` (`7`) to `None` alone did not
prevent selection. The diagnostic run showed the unit target entering the
mouse-up handler with action `7`; selection count increased while that handler
ran.

## Implementation

- `src/runtime/present.c` defines `MODERN_CLICK_IGNORE` (`0xFFFFFFFE`) to
  distinguish an ignored click from normal game actions and the existing
  clear-selection sentinel.
- For a routed right-click whose action is `None` (`0`), `Select` (`7`), or
  `ToggleSelect` (`8`), `ra2_modern_left_click_action` returns that sentinel.
  The same sentinel is used for the synthetic left release when a right-drag
  becomes a camera pan.
- The patch at `0x00693271` in `run_lift.py` handles the sentinel by clearing
  the target argument and passing action `None` to the mouse-up handler. With
  no target, the handler exits without selecting anything. Other action types
  pass through unchanged.
- Temporary runtime logging and diagnostic hooks have been removed.

## Accepted tradeoff

An unselected deployable unit or building no longer gets selected by the first
right-click of a double-right-click, so that old gesture no longer deploys the
unit or makes the building primary. The user accepts this tradeoff for now.
When the target is already selected, its ordinary right-click action remains
available. No replacement shortcut is planned.

## Follow-up coverage

The user says the fix appears to work. If continuing verification, cover
right-click with no selection and with a group selected (including a clicked
unit already in that group), ground movement, enemy attack, right-drag pan
starting over a unit, left-click selection, Classic mode, and special
placement/order modes. The double-right-click tradeoff above is accepted, not
a pending implementation request.
