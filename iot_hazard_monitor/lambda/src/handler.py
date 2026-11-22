"""Receives hazard events from the ESP32 monitor and fans them out via SNS.

Invoked through a Lambda Function URL (or API Gateway HTTP API, payload v2).

Request authentication: the device signs ``"<timestamp>.<raw body>"`` with
HMAC-SHA256 using a shared secret (stored in AWS Secrets Manager) and sends
``X-Device-Id``, ``X-Timestamp`` and ``X-Signature`` headers. Requests older
than ``MAX_CLOCK_SKEW_SECONDS`` are rejected to limit replay.

Delivery: every accepted event is recorded in DynamoDB keyed by ``event_id``
(the device retries until it gets a 2xx, so this de-duplicates retries), then
published to SNS when it is notification-worthy. If publishing fails, the
record is removed and a 5xx returned so the device retries.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import logging
import os
import re
import time
from datetime import datetime, timezone
from typing import Any

import boto3
from botocore.exceptions import ClientError

logger = logging.getLogger()
logger.setLevel(os.environ.get("LOG_LEVEL", "INFO"))

MAX_BODY_BYTES = 4096
LEVELS = ("NORMAL", "WARNING", "ALARM")
EVENT_TYPES = ("BOOT", "LEVEL_CHANGED", "REMINDER", "SENSOR_FAULT", "SENSOR_RECOVERED")
KNOWN_REASONS = {
    "GAS_WARN": "smoke/gas above warning level",
    "GAS_ALARM": "smoke/gas above alarm level",
    "TEMP_WARN": "high temperature",
    "TEMP_ALARM": "very high temperature",
    "RISE_WARN": "temperature rising quickly",
    "RISE_ALARM": "temperature rising very quickly",
    "COMBINED": "smoke/gas together with heat (possible fire)",
}
READING_KEYS = ("temperature_c", "humidity_pct", "gas_ppm", "gas_ratio", "temp_rise_c_per_min")
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
EVENT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,128}$")


class RequestError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status
        self.message = message


# --------------------------------------------------------------------- config
def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except ValueError:
        return default


def _config() -> dict[str, Any]:
    return {
        "topic_arn": os.environ.get("ALERT_TOPIC_ARN", ""),
        "table": os.environ.get("EVENTS_TABLE", ""),
        "secret_arn": os.environ.get("SIGNING_SECRET_ARN", ""),
        "max_skew": _env_int("MAX_CLOCK_SKEW_SECONDS", 300),
        "retention_days": _env_int("EVENT_RETENTION_DAYS", 30),
        "notify_boot": os.environ.get("NOTIFY_ON_BOOT", "false").lower() == "true",
        "allowed_devices": {
            d.strip() for d in os.environ.get("ALLOWED_DEVICE_IDS", "").split(",") if d.strip()
        },
    }


# ------------------------------------------------------------- AWS clients
# Created lazily and cached across invocations; tests replace these.
_clients: dict[str, Any] = {}
_secret_cache: dict[str, Any] = {"value": None, "fetched": 0.0}
SECRET_TTL_SECONDS = 300


def _client(name: str):
    if name not in _clients:
        _clients[name] = boto3.client(name)
    return _clients[name]


def _signing_secret(secret_arn: str) -> bytes:
    now = time.time()
    if _secret_cache["value"] is None or now - _secret_cache["fetched"] > SECRET_TTL_SECONDS:
        resp = _client("secretsmanager").get_secret_value(SecretId=secret_arn)
        _secret_cache["value"] = resp["SecretString"].encode()
        _secret_cache["fetched"] = now
    return _secret_cache["value"]


# ------------------------------------------------------------ HTTP helpers
def _response(status: int, body: dict[str, Any]) -> dict[str, Any]:
    return {
        "statusCode": status,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }


def _raw_body(event: dict[str, Any]) -> bytes:
    body = event.get("body") or ""
    raw = base64.b64decode(body) if event.get("isBase64Encoded") else body.encode()
    if len(raw) > MAX_BODY_BYTES:
        raise RequestError(413, "body too large")
    return raw


def _headers(event: dict[str, Any]) -> dict[str, str]:
    return {k.lower(): v for k, v in (event.get("headers") or {}).items()}


def verify_signature(headers: dict[str, str], raw: bytes, secret: bytes,
                     max_skew: int, now: float | None = None) -> str:
    """Returns the authenticated device id or raises RequestError(401)."""
    device_id = headers.get("x-device-id", "")
    ts = headers.get("x-timestamp", "")
    sig = headers.get("x-signature", "").lower()
    if not (device_id and ts and sig):
        raise RequestError(401, "missing signature headers")
    try:
        ts_int = int(ts)
    except ValueError:
        raise RequestError(401, "bad timestamp") from None
    now = time.time() if now is None else now
    if abs(now - ts_int) > max_skew:
        raise RequestError(401, "timestamp outside allowed window")
    expected = hmac.new(secret, ts.encode() + b"." + raw, hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, sig):
        raise RequestError(401, "bad signature")
    return device_id


# -------------------------------------------------------------- validation
def _num_or_none(v: Any) -> float | None:
    if v is None:
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise RequestError(400, "readings must be numbers or null")
    return float(v)


def parse_event(raw: bytes, device_id: str) -> dict[str, Any]:
    try:
        data = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise RequestError(400, "body is not valid JSON") from None
    if not isinstance(data, dict):
        raise RequestError(400, "body must be a JSON object")

    if data.get("device_id") != device_id:
        raise RequestError(400, "device_id does not match X-Device-Id")
    if not ID_RE.match(device_id):
        raise RequestError(400, "invalid device_id")
    event_id = data.get("event_id")
    if not isinstance(event_id, str) or not EVENT_ID_RE.match(event_id):
        raise RequestError(400, "invalid event_id")
    if data.get("type") not in EVENT_TYPES:
        raise RequestError(400, "invalid type")
    for key in ("level", "previous_level"):
        if data.get(key) not in LEVELS:
            raise RequestError(400, f"invalid {key}")
    reasons = data.get("reasons", [])
    if not isinstance(reasons, list) or not all(r in KNOWN_REASONS for r in reasons):
        raise RequestError(400, "invalid reasons")
    sensor = data.get("sensor")
    if sensor is not None and sensor not in ("DHT", "MQ2"):
        raise RequestError(400, "invalid sensor")
    readings_in = data.get("readings") or {}
    if not isinstance(readings_in, dict):
        raise RequestError(400, "invalid readings")
    readings = {k: _num_or_none(readings_in.get(k)) for k in READING_KEYS}

    observed_at = data.get("observed_at")
    if observed_at is not None and (isinstance(observed_at, bool)
                                    or not isinstance(observed_at, (int, float))):
        raise RequestError(400, "invalid observed_at")

    return {
        "event_id": event_id,
        "device_id": device_id,
        "type": data["type"],
        "level": data["level"],
        "previous_level": data["previous_level"],
        "reasons": reasons,
        "sensor": sensor,
        "readings": readings,
        "observed_at": int(observed_at) if observed_at is not None else None,
        "uptime_ms": data.get("uptime_ms") if isinstance(data.get("uptime_ms"), int) else None,
        "firmware": str(data.get("firmware", ""))[:16],
    }


# ------------------------------------------------------------ notification
def should_notify(ev: dict[str, Any], notify_boot: bool) -> bool:
    t = ev["type"]
    if t == "BOOT":
        return notify_boot
    if t in ("SENSOR_FAULT", "SENSOR_RECOVERED", "REMINDER"):
        return True
    # LEVEL_CHANGED: notify on any escalation, and when a hazard clears.
    return t == "LEVEL_CHANGED" and ev["level"] != ev["previous_level"]


def _fmt(v: float | None, unit: str, digits: int = 1) -> str:
    return "n/a" if v is None else f"{v:.{digits}f}{unit}"


def build_message(ev: dict[str, Any]) -> tuple[str, str]:
    """Returns (subject, body) for SNS. Subject is <= 100 chars (SNS limit)."""
    dev, level, prev, t = ev["device_id"], ev["level"], ev["previous_level"], ev["type"]
    r = ev["readings"]

    if t == "SENSOR_FAULT":
        subject = f"[FAULT] {dev}: {ev['sensor']} sensor not responding"
        headline = (f"The {ev['sensor']} sensor on {dev} stopped returning valid readings. "
                    "Hazard detection from that sensor is disabled until it recovers.")
    elif t == "SENSOR_RECOVERED":
        subject = f"[OK] {dev}: {ev['sensor']} sensor recovered"
        headline = f"The {ev['sensor']} sensor on {dev} is returning valid readings again."
    elif t == "BOOT":
        subject = f"[INFO] {dev} started"
        headline = f"{dev} powered up (firmware {ev['firmware'] or 'unknown'})."
    elif t == "REMINDER":
        subject = f"[ALARM] {dev}: hazard still present"
        headline = f"{dev} is still in ALARM."
    elif level == "NORMAL":
        subject = f"[CLEARED] {dev}: conditions back to normal"
        headline = f"{dev} returned to NORMAL (was {prev})."
    elif LEVELS.index(level) > LEVELS.index(prev):
        subject = f"[{level}] {dev}: possible smoke/gas or heat hazard"
        headline = f"{dev} raised its hazard level from {prev} to {level}."
    else:
        subject = f"[{level}] {dev}: hazard reduced to {level}"
        headline = f"{dev} lowered its hazard level from {prev} to {level}."

    when = ev["observed_at"] or int(time.time())
    when_s = datetime.fromtimestamp(when, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines = [headline, ""]
    if ev["reasons"]:
        lines.append("Why: " + "; ".join(KNOWN_REASONS[x] for x in ev["reasons"]))
    lines += [
        f"Temperature: {_fmt(r['temperature_c'], ' C')}"
        f" (rising {_fmt(r['temp_rise_c_per_min'], ' C/min')})",
        f"Humidity: {_fmt(r['humidity_pct'], ' %', 0)}",
        f"Smoke/gas index: {_fmt(r['gas_ppm'], ' ppm', 0)} (Rs/R0 {_fmt(r['gas_ratio'], '', 2)})",
        f"Time: {when_s}",
        f"Event: {ev['event_id']}",
    ]
    if level == "ALARM":
        lines += ["", "If you are on site: check for smoke, fire or a gas leak. "
                      "If in doubt, get out and call emergency services."]
    return subject[:100], "\n".join(lines)


def publish(topic_arn: str, ev: dict[str, Any]) -> str:
    subject, message = build_message(ev)
    resp = _client("sns").publish(
        TopicArn=topic_arn,
        Subject=subject,
        Message=message,
        # Lets subscriptions filter, e.g. SMS only for ALARM.
        MessageAttributes={
            "level": {"DataType": "String", "StringValue": ev["level"]},
            "type": {"DataType": "String", "StringValue": ev["type"]},
            "device_id": {"DataType": "String", "StringValue": ev["device_id"]},
        },
    )
    return resp["MessageId"]


# ---------------------------------------------------------------- storage
def _to_dynamo(v: Any) -> dict[str, Any]:
    if v is None:
        return {"NULL": True}
    if isinstance(v, bool):
        return {"BOOL": v}
    if isinstance(v, (int, float)):
        return {"N": repr(v) if isinstance(v, float) else str(v)}
    if isinstance(v, list):
        return {"L": [_to_dynamo(x) for x in v]}
    if isinstance(v, dict):
        return {"M": {k: _to_dynamo(x) for k, x in v.items()}}
    return {"S": str(v)}


def record_event(table: str, ev: dict[str, Any], retention_days: int) -> bool:
    """Stores the event. Returns False if it was already stored (a retry)."""
    now = int(time.time())
    item = {k: _to_dynamo(v) for k, v in ev.items()}
    item["received_at"] = {"N": str(now)}
    item["expires_at"] = {"N": str(now + retention_days * 86400)}
    try:
        _client("dynamodb").put_item(
            TableName=table,
            Item=item,
            ConditionExpression="attribute_not_exists(event_id)",
        )
        return True
    except ClientError as e:
        if e.response.get("Error", {}).get("Code") == "ConditionalCheckFailedException":
            return False
        raise


def forget_event(table: str, event_id: str) -> None:
    try:
        _client("dynamodb").delete_item(TableName=table, Key={"event_id": {"S": event_id}})
    except ClientError:
        logger.exception("could not roll back event %s", event_id)


# ----------------------------------------------------------------- handler
def lambda_handler(event: dict[str, Any], context: Any) -> dict[str, Any]:
    cfg = _config()
    method = (event.get("requestContext", {}).get("http", {}).get("method") or "POST").upper()
    if method != "POST":
        return _response(405, {"error": "use POST"})

    try:
        raw = _raw_body(event)
        device_id = verify_signature(
            _headers(event), raw, _signing_secret(cfg["secret_arn"]), cfg["max_skew"])
        if cfg["allowed_devices"] and device_id not in cfg["allowed_devices"]:
            raise RequestError(403, "device not allowed")
        ev = parse_event(raw, device_id)
    except RequestError as e:
        logger.warning("rejected request: %s", e.message)
        return _response(e.status, {"error": e.message})

    log_ctx = {k: ev[k] for k in ("event_id", "device_id", "type", "level", "previous_level")}

    if cfg["table"] and not record_event(cfg["table"], ev, cfg["retention_days"]):
        logger.info(json.dumps({"msg": "duplicate event", **log_ctx}))
        return _response(200, {"status": "duplicate", "event_id": ev["event_id"]})

    notified = False
    if cfg["topic_arn"] and should_notify(ev, cfg["notify_boot"]):
        try:
            publish(cfg["topic_arn"], ev)
            notified = True
        except ClientError:
            logger.exception("SNS publish failed for %s", ev["event_id"])
            if cfg["table"]:
                forget_event(cfg["table"], ev["event_id"])  # let the retry through
            return _response(502, {"error": "notification failed, retry"})

    logger.info(json.dumps({"msg": "event accepted", "notified": notified,
                            "reasons": ev["reasons"], "readings": ev["readings"], **log_ctx}))
    return _response(202, {"status": "accepted", "event_id": ev["event_id"],
                           "notified": notified})
