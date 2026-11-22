#!/usr/bin/env python3
"""Send a signed test event to the hazard-monitor Lambda, exactly as the ESP32 does.

    export LAMBDA_URL=https://xxxx.lambda-url.eu-west-1.on.aws/
    export SIGNING_SECRET=$(aws secretsmanager get-secret-value \
        --secret-id <SigningSecretArn> --query SecretString --output text)
    python3 tools/send_test_event.py alarm
    python3 tools/send_test_event.py clear

Only uses the Python standard library.
"""

import argparse
import hashlib
import hmac
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request

SCENARIOS = {
    "warning": dict(type="LEVEL_CHANGED", level="WARNING", previous_level="NORMAL",
                    reasons=["GAS_WARN"],
                    readings=dict(temperature_c=24.0, humidity_pct=45.0, gas_ppm=420,
                                  gas_ratio=2.71, temp_rise_c_per_min=0.0)),
    "alarm": dict(type="LEVEL_CHANGED", level="ALARM", previous_level="WARNING",
                  reasons=["GAS_ALARM", "RISE_WARN", "COMBINED"],
                  readings=dict(temperature_c=33.0, humidity_pct=38.0, gas_ppm=1600,
                                gas_ratio=1.08, temp_rise_c_per_min=5.5)),
    "reminder": dict(type="REMINDER", level="ALARM", previous_level="ALARM",
                     reasons=["GAS_ALARM"],
                     readings=dict(temperature_c=35.0, humidity_pct=36.0, gas_ppm=1800,
                                   gas_ratio=1.0, temp_rise_c_per_min=1.0)),
    "clear": dict(type="LEVEL_CHANGED", level="NORMAL", previous_level="ALARM", reasons=[],
                  readings=dict(temperature_c=23.0, humidity_pct=44.0, gas_ppm=25,
                                gas_ratio=8.6, temp_rise_c_per_min=-0.5)),
    "fault": dict(type="SENSOR_FAULT", sensor="DHT", level="NORMAL", previous_level="NORMAL",
                  reasons=[], readings=dict(temperature_c=None, humidity_pct=None, gas_ppm=22,
                                            gas_ratio=9.1, temp_rise_c_per_min=None)),
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", choices=sorted(SCENARIOS))
    ap.add_argument("--device-id", default="test-device")
    ap.add_argument("--url", default=os.environ.get("LAMBDA_URL"))
    args = ap.parse_args()

    secret = os.environ.get("SIGNING_SECRET")
    if not args.url or not secret:
        sys.exit("Set LAMBDA_URL (or --url) and SIGNING_SECRET.")

    now = int(time.time())
    event = {
        "schema": 1,
        "event_id": f"{args.device_id}-{secrets.token_hex(4)}-1",
        "device_id": args.device_id,
        "sensor": None,
        "observed_at": now,
        "uptime_ms": 0,
        "firmware": "test",
        **SCENARIOS[args.scenario],
    }
    body = json.dumps(event, separators=(",", ":")).encode()
    ts = str(now)
    sig = hmac.new(secret.encode(), ts.encode() + b"." + body, hashlib.sha256).hexdigest()

    req = urllib.request.Request(args.url, data=body, method="POST", headers={
        "Content-Type": "application/json",
        "X-Device-Id": args.device_id,
        "X-Timestamp": ts,
        "X-Signature": sig,
    })
    print("->", body.decode())
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            print(f"<- HTTP {resp.status}: {resp.read().decode()}")
            return 0
    except urllib.error.HTTPError as e:
        print(f"<- HTTP {e.code}: {e.read().decode()}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
