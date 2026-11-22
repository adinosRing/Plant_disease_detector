import hashlib
import hmac
import json
import os
import sys
import time

import pytest
from botocore.exceptions import ClientError

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
import handler  # noqa: E402

SECRET = b"test-secret"
TOPIC = "arn:aws:sns:eu-west-1:123456789012:alerts"


class FakeSecrets:
    def get_secret_value(self, SecretId):
        return {"SecretString": SECRET.decode()}


class FakeSns:
    def __init__(self, fail=False):
        self.published = []
        self.fail = fail

    def publish(self, **kw):
        if self.fail:
            raise ClientError({"Error": {"Code": "Throttled", "Message": "x"}}, "Publish")
        self.published.append(kw)
        return {"MessageId": "m-1"}


class FakeDynamo:
    def __init__(self):
        self.items = {}

    def put_item(self, TableName, Item, ConditionExpression):
        key = Item["event_id"]["S"]
        if key in self.items:
            raise ClientError(
                {"Error": {"Code": "ConditionalCheckFailedException", "Message": "x"}}, "PutItem")
        self.items[key] = Item

    def delete_item(self, TableName, Key):
        self.items.pop(Key["event_id"]["S"], None)


@pytest.fixture
def aws(monkeypatch):
    sns, ddb = FakeSns(), FakeDynamo()
    monkeypatch.setattr(handler, "_clients",
                        {"sns": sns, "dynamodb": ddb, "secretsmanager": FakeSecrets()})
    monkeypatch.setattr(handler, "_secret_cache", {"value": None, "fetched": 0.0})
    monkeypatch.setenv("ALERT_TOPIC_ARN", TOPIC)
    monkeypatch.setenv("EVENTS_TABLE", "events")
    monkeypatch.setenv("SIGNING_SECRET_ARN", "arn:secret")
    monkeypatch.delenv("ALLOWED_DEVICE_IDS", raising=False)
    monkeypatch.delenv("NOTIFY_ON_BOOT", raising=False)
    return sns, ddb


def payload(**over):
    p = {
        "schema": 1,
        "event_id": "dev-1-0badc0de-3",
        "device_id": "dev-1",
        "type": "LEVEL_CHANGED",
        "level": "ALARM",
        "previous_level": "WARNING",
        "reasons": ["GAS_ALARM", "RISE_WARN"],
        "sensor": None,
        "observed_at": 1795000000,
        "uptime_ms": 123456,
        "firmware": "1.0.0",
        "readings": {"temperature_c": 31.0, "humidity_pct": 40.0, "gas_ppm": 1450,
                     "gas_ratio": 1.21, "temp_rise_c_per_min": 4.5},
    }
    p.update(over)
    return p


def request(body, device="dev-1", ts=None, secret=SECRET, sig=None, method="POST"):
    raw = body if isinstance(body, str) else json.dumps(body)
    ts = str(int(time.time()) if ts is None else ts)
    if sig is None:
        sig = hmac.new(secret, f"{ts}.{raw}".encode(), hashlib.sha256).hexdigest()
    return {
        "headers": {"X-Device-Id": device, "X-Timestamp": ts, "X-Signature": sig,
                    "content-type": "application/json"},
        "body": raw,
        "isBase64Encoded": False,
        "requestContext": {"http": {"method": method}},
    }


def call(evt):
    resp = handler.lambda_handler(evt, None)
    return resp["statusCode"], json.loads(resp["body"])


# ----------------------------------------------------------------- happy path
def test_alarm_is_stored_and_published(aws):
    sns, ddb = aws
    status, body = call(request(payload()))
    assert status == 202 and body["notified"] is True
    assert "dev-1-0badc0de-3" in ddb.items
    assert ddb.items["dev-1-0badc0de-3"]["readings"]["M"]["gas_ppm"] == {"N": "1450.0"}
    msg = sns.published[0]
    assert msg["TopicArn"] == TOPIC
    assert msg["Subject"].startswith("[ALARM] dev-1")
    assert len(msg["Subject"]) <= 100
    assert "smoke/gas above alarm level" in msg["Message"]
    assert "1450 ppm" in msg["Message"]
    assert msg["MessageAttributes"]["level"]["StringValue"] == "ALARM"


def test_base64_body(aws):
    import base64
    evt = request(payload())
    evt["body"] = base64.b64encode(evt["body"].encode()).decode()
    evt["isBase64Encoded"] = True
    assert call(evt)[0] == 202


def test_duplicate_is_not_republished(aws):
    sns, _ = aws
    assert call(request(payload()))[0] == 202
    status, body = call(request(payload()))
    assert status == 200 and body["status"] == "duplicate"
    assert len(sns.published) == 1


def test_publish_failure_rolls_back_so_retry_works(aws, monkeypatch):
    _, ddb = aws
    handler._clients["sns"] = FakeSns(fail=True)
    assert call(request(payload()))[0] == 502
    assert ddb.items == {}
    good = FakeSns()
    handler._clients["sns"] = good
    assert call(request(payload()))[0] == 202
    assert len(good.published) == 1


def test_cleared_message(aws):
    sns, _ = aws
    call(request(payload(level="NORMAL", previous_level="ALARM", reasons=[])))
    assert sns.published[0]["Subject"].startswith("[CLEARED]")


def test_boot_not_notified_by_default(aws, monkeypatch):
    sns, ddb = aws
    status, body = call(request(payload(type="BOOT", level="NORMAL", previous_level="NORMAL",
                                        reasons=[])))
    assert status == 202 and body["notified"] is False and not sns.published
    assert ddb.items  # still recorded
    monkeypatch.setenv("NOTIFY_ON_BOOT", "true")
    call(request(payload(event_id="boot-2", type="BOOT", level="NORMAL",
                         previous_level="NORMAL", reasons=[])))
    assert sns.published[0]["Subject"] == "[INFO] dev-1 started"


def test_sensor_fault_message(aws):
    sns, _ = aws
    call(request(payload(type="SENSOR_FAULT", sensor="DHT", level="NORMAL",
                         previous_level="NORMAL", reasons=[],
                         readings={"temperature_c": None, "humidity_pct": None})))
    msg = sns.published[0]
    assert msg["Subject"] == "[FAULT] dev-1: DHT sensor not responding"
    assert "Temperature: n/a" in msg["Message"]


def test_works_without_table(aws, monkeypatch):
    monkeypatch.delenv("EVENTS_TABLE")
    assert call(request(payload()))[0] == 202


# ------------------------------------------------------------------- security
@pytest.mark.parametrize("mutate", [
    lambda e: e["headers"].update({"X-Signature": "0" * 64}),
    lambda e: e["headers"].pop("X-Signature"),
    lambda e: e.update({"body": e["body"].replace("ALARM", "NORMAL", 1)}),  # tampered
])
def test_bad_signature_rejected(aws, mutate):
    sns, ddb = aws
    evt = request(payload())
    mutate(evt)
    assert call(evt)[0] == 401
    assert not sns.published and not ddb.items


def test_wrong_secret_rejected(aws):
    assert call(request(payload(), secret=b"other"))[0] == 401


@pytest.mark.parametrize("offset", [-301, 301])
def test_stale_or_future_timestamp_rejected(aws, offset):
    assert call(request(payload(), ts=int(time.time()) + offset))[0] == 401


def test_device_id_must_match_header(aws):
    assert call(request(payload(), device="dev-2"))[0] == 400


def test_allow_list(aws, monkeypatch):
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "kitchen, garage")
    assert call(request(payload()))[0] == 403


def test_non_post_rejected(aws):
    assert call(request(payload(), method="GET"))[0] == 405


def test_body_too_large(aws):
    big = payload(firmware="x" * 5000)
    assert call(request(big))[0] == 413


# ----------------------------------------------------------------- validation
@pytest.mark.parametrize("over", [
    {"type": "EXPLODE"},
    {"level": "PANIC"},
    {"reasons": ["NOPE"]},
    {"reasons": "GAS_ALARM"},
    {"event_id": "bad id with spaces"},
    {"readings": {"gas_ppm": "lots"}},
    {"readings": {"gas_ppm": True}},
    {"sensor": "CO2"},
    {"observed_at": "yesterday"},
])
def test_invalid_payloads(aws, over):
    assert call(request(payload(**over)))[0] == 400


def test_not_json(aws):
    assert call(request("not json"))[0] == 400


def test_subject_truncated_for_long_device_ids(aws):
    sns, _ = aws
    dev = "d" * 64
    call(request(payload(device_id=dev, event_id="e1"), device=dev))
    assert len(sns.published[0]["Subject"]) <= 100
