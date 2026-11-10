#include "Common.h"
#include "Themes.h"
#include "Utils.h"

#define VBAT_MON  4                 // GPIO04 -- Battery Monitor PIN (ADC1 channel 3)

//
// Hardware, see docs/source/hardware.md and the schematics:
//
//   VBAT --- R8 (100k) ---+--- BAT_ADC (GPIO4)
//                         |
//                      R10 (100k)
//                         |
//                        GND          C12 (10nF) from BAT_ADC to GND
//
// So the pin sees VBAT / 2 and the firmware has to multiply back by (R8+R10)/R10.
//
#define BATT_DIVIDER         2.0f   // (R8 + R10) / R10
#define BATT_ADC_READS         12   // Samples per measurement (must be <= 32)
#define BATT_ADC_WARMUP         2   // First samples after the channel switch are junk
#define BATT_ADC_TRIMMED        2   // Drop this many lowest and highest samples
// Sampling blocks the main loop, which also polls the rotary encoder, so the
// whole burst is kept below ~1.5 ms. The divider is only 50k into 10nF
// (tau = 0.5 ms) and the cell voltage drifts far slower than that, so a long
// settle time buys nothing here.
#define BATT_ADC_SPACING_US   120
#define BATT_FILTER_ALPHA    0.30f  // IIR weight of a new measurement (0..1)
#define BATT_FILTER_ALPHA_SLOW 0.02f  // IIR weight of a new measurement in the slow SOC path (0..1)
#define BATT_SAMPLE_INTERVAL  500   // ms between real measurements
#define BATT_DISPLAY_HYST    0.01f  // Volts; keeps the last digit from flickering

// Level thresholds for the four SOC bars, on a realistic Li-ion open circuit
// voltage curve. The previous values (3.68/3.78/3.88) put a cell at about
// half charge into the top bucket.
#define BATT_SOC_LEVEL1      3.740  // Battery SOC voltage for 25%
#define BATT_SOC_LEVEL2      3.870  // Battery SOC voltage for 50%
#define BATT_SOC_LEVEL3      4.000  // Battery SOC voltage for 75%
#define BATT_SOC_HYST_2      0.030  // Battery SOC hyteresis voltage divided by 2

// A single 18650 never reads outside this window. Anything else means the ADC
// is not connected/not settled yet, so the reading is dropped instead of
// being displayed as a plausible but wrong voltage.
#define BATT_VALID_MIN       2.000f
#define BATT_VALID_MAX       5.200f

// USB present: VBUS minus the diode drop in the load sharing circuit.
#define BATT_USB_THRESHOLD   4.300f

// State machine used for battery state of charge (SOC) detection with
// hysteresis (Default = Illegal state)
static uint8_t batteryState = 255;

// Current battery voltage (filtered)
static float batteryVolts = 4.0;

// Slowly tracked cell voltage that drives the SOC state machine. A transient
// load (Wi-Fi burst, backlight step) moves the terminal voltage for a second
// or two, but the state of charge does not change at that pace, so the bars
// follow this average instead of the display voltage.
static float batterySlow = 4.0;

// Whether batterySlow holds a real measurement (not the 4.0 V default)
static bool slowReady = false;

// Last value handed to the display, with hysteresis applied
static float batteryShown = 0.0;

// Measurement state
static bool     batteryReady = false;
static uint32_t batteryLastMs = 0;

//
// Map a voltage to a SOC bucket. Used to seed the state machine so that the
// very first screen after power on already shows the right number of bars
// instead of starting in the "0 to 25%" bucket.
//
static uint8_t batteryBucket(float volts)
{
  if(volts > BATT_SOC_LEVEL3) return 3;
  if(volts > BATT_SOC_LEVEL2) return 2;
  if(volts > BATT_SOC_LEVEL1) return 1;
  return 0;
}

//
// One calibrated measurement: average of the middle samples, outliers dropped.
//
static float batteryReadOnce()
{
  uint32_t mv[BATT_ADC_READS];
  uint8_t  i, j, n = 0;

  // analogReadMilliVolts() runs the value through the per-chip ADC
  // calibration burnt into eFuse, which is what makes the absolute voltage
  // trustworthy. analogRead() returns raw counts that drift with chip and
  // temperature, which is why the old code needed a magic 1.702 factor.
  for(i = 0; i < BATT_ADC_READS; i++)
  {
    uint32_t sample = analogReadMilliVolts(VBAT_MON);

    // Burn the first few conversions: the sample-and-hold capacitor and C12
    // still carry charge from the previous channel when the pin is switched.
    if(i >= BATT_ADC_WARMUP) mv[n++] = sample;

    delayMicroseconds(BATT_ADC_SPACING_US);
  }

  if(n < (2 * BATT_ADC_TRIMMED + 1)) return batteryVolts;

  // Insertion sort, the array holds 14 entries at most
  for(i = 1; i < n; i++)
  {
    uint32_t key = mv[i];
    for(j = i; j > 0 && mv[j - 1] > key; j--) mv[j] = mv[j - 1];
    mv[j] = key;
  }

  // Mean of the middle samples: kills the switching regulator spikes from
  // the LCD backlight and the audio amplifier without slowing the response.
  uint32_t sum = 0;
  uint8_t  lo = BATT_ADC_TRIMMED, hi = n - BATT_ADC_TRIMMED;
  for(i = lo; i < hi; i++) sum += mv[i];

  return ((float)sum / (float)(hi - lo)) * BATT_DIVIDER / 1000.0f;
}

//
// Measure and return battery voltage
//
float batteryMonitor()
{
  static bool adcConfigured = false;

  if(!adcConfigured)
  {
    // 12 bits is the default, but ask explicitly: the calibration table used
    // by analogReadMilliVolts() depends on resolution and attenuation.
    analogReadResolution(12);
    // 11 dB attenuation is required here: the divider halves the cell, but
    // 4.2 V still arrives as 2.1 V, above the 0 dB / 2.5 dB ranges.
    analogSetPinAttenuation(VBAT_MON, ADC_11db);
    adcConfigured = true;
  }

  uint32_t now = millis();

  // Both the status stream (every 200 ms) and the layout (every redraw) ask
  // for the voltage. Sample once per interval and hand out the cached value
  // in between, so the display does not jitter with every frame.
  if(batteryReady && (uint32_t)(now - batteryLastMs) < BATT_SAMPLE_INTERVAL)
    return batteryVolts;

  batteryLastMs = now;

  float raw = batteryReadOnce();

  // Reject impossible readings, keep the previous value
  if(raw < BATT_VALID_MIN || raw > BATT_VALID_MAX) return batteryVolts;

  // With USB connected the pin sits on the load sharing rail at VBUS minus
  // one diode drop, which says nothing about the cell. Track it in the fast
  // filter only (the charging indicator keys off it) and freeze the slow SOC
  // path, so unplugging resumes from the last real cell voltage.
  bool charging = raw > BATT_USB_THRESHOLD;

  if(!batteryReady)
  {
    batteryVolts = raw;
    batteryReady = true;
  }
  else
  {
    // Exponential filter: responds in about three measurements (~1.5 s)
    // but a single noisy conversion cannot move the bars.
    batteryVolts += (raw - batteryVolts) * BATT_FILTER_ALPHA;
  }

  if(charging) return batteryVolts;

  if(!slowReady)
  {
    batterySlow = batteryVolts;
    slowReady = true;
  }
  else
  {
    // The bars trail the display voltage by design: at this rate the average
    // needs minutes to cross a level, so neither a load transient nor a
    // single good/bad measurement can step them.
    batterySlow += (batteryVolts - batterySlow) * BATT_FILTER_ALPHA_SLOW;
  }

  // State machine
  // SOC (%)      batteryState
  //  0 to 25           0
  // 25 to 50           1
  // 50 to 75           2
  // 75 to 100          3
  switch(batteryState)
  {
    case 0:
      if      (batterySlow > (BATT_SOC_LEVEL1 + BATT_SOC_HYST_2)) batteryState = 1;   // State 0 > 1
      break;
    case 1:
      if      (batterySlow > (BATT_SOC_LEVEL2 + BATT_SOC_HYST_2)) batteryState = 2;   // State 1 > 2
      else if (batterySlow < (BATT_SOC_LEVEL1 - BATT_SOC_HYST_2)) batteryState = 0;   // State 1 > 0
      break;
    case 2:
      if      (batterySlow > (BATT_SOC_LEVEL3 + BATT_SOC_HYST_2)) batteryState = 3;   // State 2 > 3
      else if (batterySlow < (BATT_SOC_LEVEL2 - BATT_SOC_HYST_2)) batteryState = 1;   // State 2 > 1
      break;
    case 3:
      if      (batterySlow < (BATT_SOC_LEVEL3 - BATT_SOC_HYST_2)) batteryState = 2;   // State 3 > 2
      break;
    default:
      // First measurement after power on: start where the voltage says we are
      batteryState = batteryBucket(batterySlow);
      break;
  }

  // Return current voltage
  return(batteryVolts);
}

//
// Show last measured battery voltage and status at given screen
// coordinates. Return true if voltage was drawn.
//
bool drawBattery(int x, int y)
{
  if(sleepOn()) return false;

  // Measure battery voltage and status
  batteryMonitor();

  // Set display information
  spr.drawRoundRect(x, y + 1, 28, 14, 3, TH.batt_border);
  spr.drawLine(x + 29, y + 5, x + 29, y + 10, TH.batt_border);
  spr.drawLine(x + 30, y + 6, x + 30, y + 9, TH.batt_border);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(TH.batt_voltage);

  if(switchThemeEditor())
  {
    // Alternate between five battery states every 10 seconds
    batteryState = (millis() % 50000u) / 10000u;
    batteryVolts = batteryState >= 4 ? 4.5 : 4.0;
  }

  // The hardware has a load sharing circuit to allow simultaneous charge and power
  // With USB(5V) connected the voltage reading will be approx. VBUS - Diode Drop = 4.65V
  // If the average voltage is greater than 4.3V, show ligtning on the display
  if(batteryVolts > BATT_USB_THRESHOLD)
  {
    // Charging: the cell is being topped up, so it is by definition full enough
    batteryState = 3;

    spr.fillRoundRect(x + 2, y + 3, 24, 10, 2, TH.batt_charge);
    spr.drawLine(x + 9 + 8, y + 1, x + 9 + 6, y + 1 + 5, TH.bg);
    spr.drawLine(x + 9 + 6, y + 1 + 5, x + 9 + 10, y + 1 + 5, TH.bg);
    spr.drawLine(x + 9 + 11, y + 1 + 6, x + 9 + 4, y + 1 + 13, TH.bg);
    spr.drawLine(x + 9 + 2, y + 1 + 13, x + 9 + 4, y + 1 + 8, TH.bg);
    spr.drawLine(x + 9 + 4, y + 1 + 8, x + 9 + 0, y + 1 + 8, TH.bg);
    spr.drawLine(x + 9 - 1, y + 1 + 7, x + 9 + 6, y + 1 + 0, TH.bg);
    spr.fillTriangle(x + 9 + 7, y + 1, x + 9 + 4, y + 1 + 6, x + 9, y + 1 + 7, TH.batt_icon);
    spr.fillTriangle(x + 9 + 5, y + 1 + 6, x + 9 + 10, y + 1 + 6, x + 9 + 3, y + 1 + 13, TH.batt_icon);
    spr.fillRect(x + 9 + 1, y + 1 + 6, 9, 2, TH.batt_icon);
    spr.drawPixel(x + 9 + 3, y + 1 + 12, TH.batt_icon);
    return false;
  }
  else
  {
    char voltage[8];
    uint16_t color;
    int level;

    // Keep the last digit stable: only move the shown value when the filtered
    // voltage really moved by 10 mV, otherwise "4.06V" flickers to "4.05V"
    // and back on every redraw.
    if(batteryShown <= 0.0f || fabsf(batteryVolts - batteryShown) >= BATT_DISPLAY_HYST)
      batteryShown = batteryVolts;

    // Text representation of the voltage
    sprintf(voltage, "%.02fV", batteryShown);

    // Battery bar color and width
    switch(batteryState)
    {
      case 0:
        color = TH.batt_low;
        level = 6;
        break;
      case 1:
        color = TH.batt_full;
        level = 12;
        break;
      case 2:
        color = TH.batt_full;
        level = 18;
        break;
      case 3:
      default:
        color = TH.batt_full;
        level = 24;
        break;
    }

    spr.fillRoundRect(x + 2, y + 3, level, 10, 2, color);
    spr.drawString(voltage, x - 3, y, FONT_SMALL);
    return true;
  }
}
