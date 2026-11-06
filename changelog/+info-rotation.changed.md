Changed the sidebar info line to rotate through everything that is running:

- The bottom line used to alternate between an active countdown and the clock, so with both timers set the clock disappeared entirely.
- It now rotates through off-timer, standby-timer and clock, in whatever combination is active, one slot per refresh interval.
- A single active item stays on screen without rotating, so nothing blinks when only one of them is set.
