// Copy this file to secrets.h and fill in your values. secrets.h is ignored
// by git so credentials are never committed.
#pragma once

#define WIFI_SSID      "your-wifi-ssid"
#define WIFI_PASSWORD  "your-wifi-password"

// Letters, digits, '-' and '_' only (it is embedded in JSON and headers).
#define DEVICE_ID      "kitchen-monitor-01"

// The FunctionUrl output of the SAM stack, e.g.
// "https://abc123xyz.lambda-url.eu-west-1.on.aws/"
#define LAMBDA_URL     "https://REPLACE_ME.lambda-url.REGION.on.aws/"

// Shared HMAC key: the value of the SigningSecret created by the stack.
//   aws secretsmanager get-secret-value --secret-id <SigningSecretArn> \
//       --query SecretString --output text
#define SIGNING_SECRET "REPLACE_WITH_SIGNING_SECRET"
