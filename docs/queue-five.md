# Queue Five

**Feature ID:** `RA2-QOL-001`

Hold **Shift** and click a production cameo in the Yuri's Revenge sidebar to
queue five of that item. A plain click keeps the game's normal one-item
behavior. This shortcut applies to production clicks only; Shift-right-click
queue removal is not included.

The implementation adds four copies of the game's normal production event to
the outgoing event ring before the original event is added. This keeps the
existing production and event-processing path in use. The extra copies stop
when the ring reaches its existing 128-event capacity.

This is a separate shortcut from the Modern mouse controls setting. The current
hook sites and event-ring layout are for Yuri's Revenge (`gamemd.exe`).
