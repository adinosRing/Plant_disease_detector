// Hardware and behaviour settings. Secrets live in secrets.h (not committed).
#pragma once

#define FIRMWARE_VERSION "1.0.0"

// ---------------------------------------------------------------- Pins (ESP32)
// MQ2 AO must go to an ADC1 pin (ADC2 is unusable while Wi-Fi is on).
// The MQ2 module runs from 5 V, so its AO can reach ~5 V: put it through a
// divider (e.g. 10k top / 20k bottom) before the ESP32 pin (max 3.3 V).
#define PIN_MQ2_AO        34
#define PIN_DHT           4
#define PIN_BUZZER        25   // drive through an NPN transistor (see README)
#define PIN_SILENCE_BTN   27   // momentary to GND, internal pull-up. -1 = none
#define PIN_STATUS_LED    2    // on-board LED on most dev boards. -1 = none

#define DHT_SENSOR_TYPE   DHT22  // AM2302: -40..80 C, 0.1 C resolution

// true  = passive piezo (driven with a tone)
// false = active buzzer module (just switched on/off)
#define BUZZER_PASSIVE    false
#define BUZZER_TONE_HZ    3000

// ----------------------------------------------------------------- MQ2 circuit
#define MQ2_VCC_VOLTS        5.0f   // supply on the module's VCC pin
#define MQ2_RLOAD_KOHM       1.0f   // load resistor on the module (often 1k; check yours)
#define MQ2_DIVIDER_FACTOR   1.5f   // Vsensor = Vpin * factor; (10k+20k)/20k = 1.5. 1.0 = no divider
#define MQ2_ADC_OVERSAMPLE   16
#define MQ2_WARMUP_MS        (60UL * 1000UL)  // heater warm-up after power on
#define MQ2_CAL_SAMPLES      30               // clean-air samples for R0
// Fixed R0 (kOhm) if you calibrated it yourself; 0 = auto-calibrate on first
// boot and store in flash. Hold the silence button during boot to redo it.
#define MQ2_R0_KOHM          0.0f

// ------------------------------------------------------------------ Thresholds
// See hazard::Thresholds in hazard_logic.h for the defaults and meanings.
#define GAS_WARN_PPM          300.0f
#define GAS_ALARM_PPM         1000.0f
#define TEMP_WARN_C           45.0f
#define TEMP_ALARM_C          57.0f   // common fixed-temperature heat-detector rating
#define RISE_WARN_C_PER_MIN   4.0f
#define RISE_ALARM_C_PER_MIN  8.0f

// ---------------------------------------------------------------------- Timing
#define SAMPLE_PERIOD_MS      2000UL            // DHT22 needs >= 2 s between reads
#define DHT_FAULT_AFTER       5                 // consecutive failed reads
#define ALARM_REMINDER_MS     (5UL * 60UL * 1000UL)  // re-notify while ALARM persists
#define SILENCE_MS            (5UL * 60UL * 1000UL)  // buzzer hush duration

// ------------------------------------------------------------------- Notifier
#define NOTIFY_QUEUE_LEN      12
#define NOTIFY_HTTP_TIMEOUT_MS 10000
#define NOTIFY_RETRY_MIN_MS   2000UL
#define NOTIFY_RETRY_MAX_MS   60000UL
#define NTP_SERVER_1          "pool.ntp.org"
#define NTP_SERVER_2          "time.google.com"
