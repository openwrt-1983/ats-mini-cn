Added plain-number RSSI and SNR readouts in the free corner above the scale:

- The area at the right edge, above the frequency scale and to the right of the station name, was empty; it now shows `SNR n` and `RSSI n` in two lines, updated whenever the values change.
- The readouts are drawn last and clear their own corner first, so a long station name cannot run into them.
- Only the default layout gets the readout: the alternate large S-meter layout already shows both values as gauges.
