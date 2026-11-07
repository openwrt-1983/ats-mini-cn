#ifndef PARSER_ADHOC_H
#define PARSER_ADHOC_H

#include "ParserTypes.h"
#include "Themes.h"

// Memory names are UTF-8. Must match MEMORY_NAME_LEN in Common.h, which cannot
// be included here without pulling the display stack into the parser tests.
#ifndef MEMORY_NAME_LEN
#define MEMORY_NAME_LEN 25
#endif

struct AdhocCommand {
  enum class Id : uint8_t {
    Unknown = 0,
    RotateCw,
    RotateCcw,
    Click,
    ShortPress,
    BandUp,
    BandDown,
    ModeUp,
    ModeDown,
    StepUp,
    StepDown,
    BandwidthUp,
    BandwidthDown,
    AgcUp,
    AgcDown,
    VolumeUp,
    VolumeDown,
    BacklightUp,
    BacklightDown,
    SleepOn,
    SleepOff,
    Standby,
    WakeLight,
    CalUp,
    CalDown,
    CaptureScreen,
    CaptureScreenBinary,
    ToggleLog,
    MonitorOn,
    GetMemories,
    SetMemory,
    ToggleThemeEditor,
    SetTheme,
    GetTheme,
    SetFrequency,
  };

  static constexpr size_t BAND_LEN = 8;
  static constexpr size_t MODE_LEN = 4;
  static constexpr size_t THEME_WORDS =
      (sizeof(ColorTheme) - offsetof(ColorTheme, bg)) / sizeof(uint16_t);

  Id id;
  uint32_t memory_slot;
  char band[BAND_LEN];
  uint32_t frequency;
  char mode[MODE_LEN];
  bool has_name;
  char name[MEMORY_NAME_LEN]; // Omitted preserves the name; present but empty clears it. UTF-8.
  uint16_t theme_words[THEME_WORDS];
};


// Pure complete-frame parser. Text frames include their line ending.
// On errors the output may be partially filled and must not be executed.
ParseResult parseAdhoc(ByteSpan frame, AdhocCommand& out);

namespace command_parser {
extern const FramingRules adhocFraming;
}

#endif
