Fixed and polished the standby and timer paths:

- The encoder now wakes a timer-triggered Bluetooth standby even when "lock" is the configured sleep mode. The main loop dropped the pending counts based on the configured mode instead of the running one, so the unit could only be woken by the button.
- The same check in the Bluetooth HID gesture path was aligned, so push-and-rotate works after that wake-up too.
- USB serial: any incoming byte now wakes a powered-down receiver before the command is parsed, matching the app link. Previously every command was ignored while in standby, so a USB-only setup had to press a physical key.
- The off and standby timers show their live remaining minutes instead of the original duration, and turning the encoder adjusts from the value on screen.
- The standby timer clears its stored duration once it fires, so the settings entry reads "off" instead of keeping a number that can never fire again.
- Restoring Wi-Fi/Bluetooth after the final-minutes quiet window moved to the timer tick, so it no longer blocks the encoder while a timer is being adjusted.
- Waking from standby starts the chip directly in the current band's mode (FM or AM) instead of always in AM, removing one mode switch from the wake path.
- Two-state settings ("scroll direction", "encoder") now follow the turn direction instead of toggling. A fast turn reports two counts at once, and toggling twice left the setting unchanged, so the switch felt dead.
- The standby timer holds off while the off timer is in its final-minutes quiet window: Bluetooth is already down at that point, so standby would leave the unit unreachable until the button press, and it is about to power off anyway.
- Pressing the button in the standby timer sidebar only closes it. Clearing the countdown on a normal exit was too easy to trigger by accident; turning it down to zero is the explicit off switch.
