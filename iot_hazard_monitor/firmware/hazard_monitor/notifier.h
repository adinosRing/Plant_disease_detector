// Delivers JSON events to the AWS Lambda endpoint from a background FreeRTOS
// task, so slow Wi-Fi / TLS never blocks sensing or the buzzer.
//
// Each request carries:
//   X-Device-Id: <DEVICE_ID>
//   X-Timestamp: <unix seconds>
//   X-Signature: hex(HMAC-SHA256(SIGNING_SECRET, "<timestamp>.<body>"))
// Events are retried with exponential back-off until the server accepts them
// (2xx) or rejects them as malformed (400/413). When the queue is full the
// oldest event is dropped.
#pragma once

#include <stddef.h>

namespace notifier {

constexpr size_t kMaxBody = 768;

bool begin();
// Copies `json` into the queue. Returns false if it is too large.
bool enqueue(const char* json);
// Events waiting to be sent (excluding the one in flight).
size_t pending();
bool timeSynced();

}  // namespace notifier
