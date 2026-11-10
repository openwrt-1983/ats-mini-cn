Moved the driver hardening out of the sketch and into the library it belongs to:

- The bundled SI4732 fork (`ats-mini/libraries/SI4732`, a hardened 2.1.8 driver) now carries the bus timeout, the bounded status/RDS reads and the single-shot seek with a cancel on timeout. `build.sh` selects it explicitly, because arduino-cli would otherwise pick the official PU2CLR library of the same name.
- `SI4735-fixed.h` no longer reimplements `seekStationProgress()`: the library version issues one seek, gives up on a dead bus and cancels the seek when the knob is released, which the local copy never did.
- RDS polling keeps the local override (it still zeroes the unused command bytes) but gained the same guard, so an unplugged or powered-down tuner can no longer spin the main loop forever and freeze the UI.
