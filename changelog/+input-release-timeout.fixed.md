A stuck encoder button can no longer leave the receiver unresponsive:

- Entering and leaving display sleep waits for the button to be released, which never happened when a dirty or jammed contact held the pin low: the panel was already dark, the radio kept playing and every input was ignored until the battery was pulled. The wait is now bounded at three seconds.
- The backlight pin is handed back to the LEDC after a standby wake. It stayed routed to the RTC peripheral, which setup() cannot drive, so the panel could stay dark for the whole session after a long-press standby.
- A wake press that is released too early now parks the supply rails low again before going back to sleep, instead of leaving the receiver supply floating for the whole sleep.
