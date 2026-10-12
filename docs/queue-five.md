# Queue Five

**Feature ID:** `RA2-QOL-001`

Hold **Shift** and click a production cameo in either game's sidebar to queue
five of that item. A plain click keeps the game's normal one-item
behavior. This shortcut applies to production clicks only; Shift-right-click
queue removal is not included.

The implementation adds four copies of the game's normal production event to
the outgoing event ring before the original event is added. This keeps the
existing production and event-processing path in use. The extra copies stop
when the ring reaches its existing 128-event capacity.

This is a separate shortcut from the Modern mouse controls setting. Both game
targets use the same 128-entry ring and 111-byte event records, at different
addresses:

| Target | Shift-click hooks | Event count | Ring index | Event ring | Timestamps |
| --- | --- | --- | --- | --- | --- |
| Yuri's Revenge (`gamemd.exe`) | `0x006AB511`, `0x006AB76E` | `0x00A802C8` | `0x00A802D0` | `0x00A802D4` | `0x00A83A54` |
| Red Alert 2 (`game.exe`) | `0x0068119E`, `0x006813EF` | `0x00A32338` | `0x00A32340` | `0x00A32344` | `0x00A35AC4` |

These addresses are for the supported shipping executables. The base-game
target has been built from its own lift; its in-game behavior still needs
manual verification.
