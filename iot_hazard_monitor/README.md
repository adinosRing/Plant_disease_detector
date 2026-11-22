# IoT Hazard Monitor (MQ2 + DHT11 → buzzer + AWS Lambda alerts)

An ESP32 watches a **MQ2** smoke/combustible-gas sensor and a **DHT11**
temperature/humidity sensor. When it detects a possible hazard it **sounds a
buzzer** on the spot and **sends a signed event to AWS Lambda**. Lambda records
the event and sends an **email and/or SMS through Amazon SNS**.

> ⚠️ This is a hobby early-warning project, **not** a certified smoke or gas
> alarm. Keep certified detectors installed. MQ-series sensors are not
> selective or factory-calibrated, and the DHT11 only measures 0–50 °C.

```
 MQ2 ──AO─(divider)─▶ GPIO34 ┐                       HTTPS + HMAC
 DHT11 ──DATA───────▶ GPIO4  ├─ ESP32 ─── Wi-Fi ──▶ Lambda Function URL
 Silence button ────▶ GPIO27 │      │                   │  ├─▶ DynamoDB (dedupe + history)
                             │      └─▶ GPIO25 ─▶ Buzzer │  └─▶ SNS ─▶ email (all events)
                             └ on-board LED mirrors buzzer          └─▶ SMS (ALARM only)
```

## What counts as hazardous

Each sample (every 2 s) is classed as NORMAL, WARNING or ALARM:

| Signal | WARNING | ALARM |
|---|---|---|
| MQ2 smoke-equivalent index | ≥ 300 ppm | ≥ 1000 ppm |
| Temperature | ≥ 40 °C | ≥ 50 °C (DHT11 maximum) |
| Rate of rise (over 60 s) | ≥ 4 °C/min | ≥ 8 °C/min |
| Smoke/gas ≥ warning **and** heat (temp or rise ≥ warning) | | ALARM (`COMBINED`, likely fire) |

To avoid false alarms and flapping:

* a level is entered only after **2 consecutive** samples above it;
* a level is left only after **5 consecutive** samples below it **minus
  hysteresis** (20 % for gas, 2 °C, 1 °C/min);
* the MQ2 is ignored during its **60 s heater warm-up**;
* rate of rise needs at least 30 s of history.

All thresholds are in `firmware/hazard_monitor/config.h`.

### Buzzer

* **WARNING**: 150 ms chirp every 2 s.
* **ALARM**: ISO 8201 "temporal-3" evacuation pattern (three 0.5 s beeps, 1.5 s pause).
* **Silence button**: hushes the buzzer for 5 min. A higher level cancels the hush.

### Events sent to Lambda

| `type` | When | Notified |
|---|---|---|
| `LEVEL_CHANGED` | any change of level, including back to NORMAL ("cleared") | yes |
| `REMINDER` | every 5 min while ALARM lasts | yes |
| `SENSOR_FAULT` / `SENSOR_RECOVERED` | DHT fails 5 reads in a row / MQ2 reading out of range, and recovery | yes |
| `BOOT` | device started | only if `NotifyOnBoot=true` |

Events are queued on the device and sent from a background task, so a slow or
missing network never delays the buzzer. Failed sends are retried with back-off
(2 s up to 60 s). Each event has a unique `event_id`, so retries are
de-duplicated in DynamoDB and you get one notification per event.

## Hardware

| Part | Notes |
|---|---|
| ESP32 dev board | any board with the Arduino-ESP32 core 2.x or 3.x |
| MQ2 module | runs from **5 V** (heater draws about 150 mA) |
| DHT11 module | 3.3 V. Use the 3-pin module, or add a 10 kΩ pull-up on DATA if you use the bare sensor |
| Buzzer | 5 V active buzzer (or a passive piezo with `BUZZER_PASSIVE true`) |
| NPN transistor (2N2222/BC547) + 1 kΩ | to drive the buzzer |
| 10 kΩ + 20 kΩ resistors | voltage divider for MQ2 AO |
| Push button (optional) | silence/recalibrate |

### Wiring

| From | To |
|---|---|
| MQ2 VCC / GND | 5V (VIN) / GND |
| MQ2 AO | 10 kΩ → **GPIO34**, and GPIO34 → 20 kΩ → GND |
| DHT11 VCC / GND / DATA | 3V3 / GND / **GPIO4** |
| GPIO25 | 1 kΩ → transistor base. Emitter → GND. Collector → buzzer −. Buzzer + → 5V |
| Button | **GPIO27** ↔ GND (internal pull-up is used) |

Notes:

* **The divider is required**: the MQ2 output can reach 5 V and ESP32 pins are 3.3 V max.
  If you change it, update `MQ2_DIVIDER_FACTOR` (= (Rtop + Rbottom) / Rbottom).
* Use an **ADC1** pin (GPIO32–39) for the MQ2. ADC2 cannot be used while Wi-Fi is on.
* Check the load resistor on your MQ2 board (often `102` = 1 kΩ, sometimes 4.7 kΩ or 10 kΩ)
  and set `MQ2_RLOAD_KOHM`.
* The MQ2 "DO" pin (on-board comparator) is not used.

## Firmware setup

1. Install the **Arduino IDE** (or `arduino-cli`), the **esp32** board package,
   and the libraries **DHT sensor library** and **Adafruit Unified Sensor**.
2. `cp firmware/hazard_monitor/secrets.example.h firmware/hazard_monitor/secrets.h`
   and fill it in **after** deploying the backend (you need the URL and key).
3. Open `firmware/hazard_monitor/hazard_monitor.ino`, select your board, and upload.
4. Open the Serial Monitor at 115200 baud.

### MQ2 calibration

The MQ2 reports a ratio Rs/R0, where R0 is its resistance in clean air. On the
**first boot** (after the 60 s warm-up) the firmware averages 30 samples
(about 1 min) **in clean air**, computes R0 and saves it to flash.

* New MQ2 sensors should first be **burned in** (powered for 24–48 h).
  Recalibrate afterwards.
* To recalibrate: **hold the silence button while powering on** for 3 s.
* Or set a fixed `MQ2_R0_KOHM` in `config.h`.
* Gas is not monitored while calibrating. Never calibrate in smoky or gassy air.

The ppm value comes from the datasheet's smoke curve. Treat it as an
**index for thresholds**, not as a measured concentration. Tune
`GAS_WARN_PPM` / `GAS_ALARM_PPM` by watching the serial output in your room
(e.g. near a blown-out match, or a lighter released without a flame).

### Arduino command line (optional)

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "DHT sensor library" "Adafruit Unified Sensor"
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/hazard_monitor
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 firmware/hazard_monitor
```

## AWS backend

`lambda/template.yaml` (AWS SAM) creates:

* **Lambda** (Python 3.12) with a public **Function URL**. Every request is
  authenticated in code with HMAC-SHA256.
* **Secrets Manager** secret holding the HMAC key (generated for you).
* **DynamoDB** table: event history (30-day TTL) and retry de-duplication.
* **SNS topic**: an email subscription, plus an optional SMS subscription
  filtered to `level = ALARM`.
* **CloudWatch alarm** that emails you if the Lambda itself starts failing.

### Deploy

```bash
cd lambda
sam build
sam deploy --guided \
  --parameter-overrides AlertEmail=you@example.com AlertPhoneNumber=+15551234567
```

Then:

1. **Confirm the SNS subscription email** (no alerts arrive until you do).
2. Copy the `FunctionUrl` output into `LAMBDA_URL` in `secrets.h`.
3. Get the device key and put it in `SIGNING_SECRET`:
   ```bash
   aws secretsmanager get-secret-value --secret-id <SigningSecretArn> \
     --query SecretString --output text
   ```
4. Flash the device.

SMS through SNS may need the account to leave the SMS sandbox, or to verify the
destination number, depending on region.

### Test the backend without hardware

```bash
export LAMBDA_URL=<FunctionUrl>
export SIGNING_SECRET=<key from step 3>
python3 tools/send_test_event.py alarm     # also: warning, reminder, clear, fault
```

The script signs the event exactly as the firmware does. You should get
`HTTP 202` and an email.

### Security model

* Signature = `hex(HMAC_SHA256(key, "<X-Timestamp>.<raw body>"))`. Requests more
  than 5 min old or in the future are rejected (the ESP32 gets its time over NTP).
* The device checks the server's TLS certificate against Amazon Root CA 1
  (`root_ca.h`).
* `AllowedDeviceIds` limits which device ids are accepted.
* All devices share one key. For per-device identities, use AWS IoT Core with
  X.509 certificates and an IoT rule that invokes this Lambda.

## Tests

```bash
# Firmware hazard logic and MQ2 maths, run on your computer (g++ with ASan/UBSan)
make -C firmware/test

# Lambda
pip install -r lambda/requirements-dev.txt
cd lambda && python -m pytest -q
```

## Project layout

```
firmware/hazard_monitor/
  hazard_monitor.ino   sensing loop, buzzer patterns, event creation
  hazard_logic.{h,cpp} level classification, debounce, hysteresis, rate of rise (no Arduino deps)
  mq2_math.h           Rs, R0 and ppm conversions
  notifier.{h,cpp}     FreeRTOS queue + HTTPS/HMAC sender with retry
  config.h             pins, thresholds, timing
  secrets.example.h    Wi-Fi, endpoint and key template
  root_ca.h            Amazon Root CA 1
firmware/test/         host-side unit tests
lambda/src/handler.py  verify → validate → dedupe → SNS
lambda/template.yaml   SAM stack
lambda/tests/          pytest suite
tools/send_test_event.py
```

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `send failed (-1)` in the serial log | Wi-Fi, DNS or TLS problem; check `LAMBDA_URL` |
| HTTP 401 `timestamp outside allowed window` | NTP not reachable from your network |
| HTTP 401 `bad signature` | `SIGNING_SECRET` does not match the secret value |
| HTTP 403 from the URL with an AWS error body | the Function URL lacks an invoke permission; check the function's resource policy |
| Gas reads hundreds of ppm in clean air | R0 was calibrated in dirty air or before burn-in; recalibrate. Check `MQ2_RLOAD_KOHM` and the divider |
| `SENSOR_FAULT MQ2` | AO wiring or divider; the ADC reads about 0 V or the full rail |
| No email | SNS subscription not confirmed |
