#ifndef STATION_NAMES_H
#define STATION_NAMES_H

//
// Chinese FM station names.
//
// The radio cannot learn these from the air: RDS carries only the 8-character
// ASCII programme service name, and most Chinese FM stations do not transmit
// RDS at all. Names therefore come from the 99 memory slots, which are keyed
// by frequency and may now hold UTF-8 text.
//
// This table is the pick list the web memory editor offers, so a slot can be
// named in Chinese with one click instead of typing. It also defines the
// character set that FontCN16.h / FontCN24.h must cover, so every name in the
// list is guaranteed to render on the receiver's screen.
//

extern const char *const stationNamePresets[];
extern const int         stationNamePresetCount;

#endif // STATION_NAMES_H
