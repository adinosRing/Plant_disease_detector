#include "notifier.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "config.h"
#include "mbedtls/md.h"
#include "root_ca.h"
#include "secrets.h"

namespace notifier {
namespace {

struct Item { char body[kMaxBody]; };

QueueHandle_t gQueue = nullptr;

bool hmacSha256Hex(const char* ts, const char* body, char out[65]) {
  unsigned char mac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
            mbedtls_md_hmac_starts(&ctx, (const unsigned char*)SIGNING_SECRET,
                                   strlen(SIGNING_SECRET)) == 0 &&
            mbedtls_md_hmac_update(&ctx, (const unsigned char*)ts, strlen(ts)) == 0 &&
            mbedtls_md_hmac_update(&ctx, (const unsigned char*)".", 1) == 0 &&
            mbedtls_md_hmac_update(&ctx, (const unsigned char*)body, strlen(body)) == 0 &&
            mbedtls_md_hmac_finish(&ctx, mac) == 0;
  mbedtls_md_free(&ctx);
  if (!ok) return false;
  for (int i = 0; i < 32; ++i) sprintf(out + 2 * i, "%02x", mac[i]);
  out[64] = '\0';
  return true;
}

// Returns the HTTP status, or a negative HTTPClient error code.
int post(const char* body) {
  char ts[16];
  snprintf(ts, sizeof ts, "%ld", (long)time(nullptr));
  char sig[65];
  if (!hmacSha256Hex(ts, body, sig)) return -1000;

  WiFiClientSecure client;
  client.setCACert(AWS_ROOT_CA);
  HTTPClient http;
  http.setConnectTimeout(NOTIFY_HTTP_TIMEOUT_MS);
  http.setTimeout(NOTIFY_HTTP_TIMEOUT_MS);
  if (!http.begin(client, LAMBDA_URL)) return -1001;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-Timestamp", ts);
  http.addHeader("X-Signature", sig);
  int code = http.POST((uint8_t*)body, strlen(body));
  if (code > 0 && (code < 200 || code >= 300)) {
    Serial.printf("[notify] HTTP %d: %s\n", code, http.getString().c_str());
  }
  http.end();
  return code;
}

void task(void*) {
  static Item item;  // static: keep the large buffer off the task stack
  uint32_t backoff = NOTIFY_RETRY_MIN_MS;
  for (;;) {
    if (xQueueReceive(gQueue, &item, portMAX_DELAY) != pdTRUE) continue;
    // Retry this event until it is delivered or permanently rejected.
    for (;;) {
      if (WiFi.status() != WL_CONNECTED || !timeSynced()) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // signatures need a correct clock
        continue;
      }
      int code = post(item.body);
      if (code >= 200 && code < 300) {
        Serial.printf("[notify] delivered (HTTP %d), %u queued\n", code,
                      (unsigned)pending());
        backoff = NOTIFY_RETRY_MIN_MS;
        break;
      }
      if (code == 400 || code == 413) {
        Serial.printf("[notify] event rejected (HTTP %d), dropping\n", code);
        break;
      }
      Serial.printf("[notify] send failed (%d), retry in %lu ms\n", code,
                    (unsigned long)backoff);
      vTaskDelay(pdMS_TO_TICKS(backoff));
      backoff = (backoff * 2 > NOTIFY_RETRY_MAX_MS) ? NOTIFY_RETRY_MAX_MS : backoff * 2;
    }
  }
}

}  // namespace

bool begin() {
  gQueue = xQueueCreate(NOTIFY_QUEUE_LEN, sizeof(Item));
  if (!gQueue) return false;
  // TLS handshakes need a generous stack. Core 0 keeps loop() (core 1) free.
  return xTaskCreatePinnedToCore(task, "notifier", 12288, nullptr, 1, nullptr,
                                 0) == pdPASS;
}

bool enqueue(const char* json) {
  static Item item;  // only called from loop(); keep it off the stack
  size_t len = strlen(json);
  if (!gQueue || len >= kMaxBody) return false;
  memcpy(item.body, json, len + 1);
  if (xQueueSend(gQueue, &item, 0) != pdTRUE) {
    static Item dropped;
    xQueueReceive(gQueue, &dropped, 0);  // drop the oldest
    Serial.println("[notify] queue full, dropped oldest event");
    return xQueueSend(gQueue, &item, 0) == pdTRUE;
  }
  return true;
}

size_t pending() { return gQueue ? uxQueueMessagesWaiting(gQueue) : 0; }

bool timeSynced() { return time(nullptr) > 1700000000; }  // after Nov 2023

}  // namespace notifier
