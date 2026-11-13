"CPU off" light sleep reprogrammed the encoder pads' interrupt type to the
level the wake-up hardware watches, and `gpio_wakeup_disable()` leaves that
level trigger behind while the encoder ISR is still attached. Holding the knob
to wake the unit then re-entered the ISR in a tight loop until the interrupt
watchdog reset the chip -- on screen the boot banner with the "Hold to reset"
countdown while the finger was still down, and one crash loop was enough to
wipe the stored settings through the factory-reset prompt. The encoder
interrupts are now detached for the sleep and re-attached with CHANGE on the
way out, and the factory-reset gesture is only accepted after a genuine
power-on, never after a watchdog or panic reboot.
