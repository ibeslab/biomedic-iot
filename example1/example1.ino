#include "iot-b.h"
#include "akun-telu.h"

// -----------------------------------------------------------------------------
// Campus MQTT broker
// -----------------------------------------------------------------------------
const char* MQTT_HOST = "10.21.72.3";
const uint16_t MQTT_PORT = 1883;

// Change Device01 to a unique student/device name
const char* TOPIC_SUB = "Device01/#";
const char* TOPIC_STATUS = "Device01/status";
const char* TOPIC_DATA = "Device01/topic1";


// Called whenever an MQTT message arrives
void on_mqtt(char* topic, byte* payload, unsigned int length) {
  Serial.printf("MQTT[%s] ", topic);

  for (unsigned int i = 0; i < length; i++) {
    Serial.write(payload[i]);
  }

  Serial.println();
}


void setup() {
  Serial.begin(115200);

  // ---------------------------------------------------------------------------
  // 1. Connect to campus Wi-Fi
  //    PEAP + MSCHAPv2 is handled by our updated iot-b library.
  // ---------------------------------------------------------------------------
  iot_wifi_campus(WIFI_SSID, WIFI_USER, WIFI_PASS, WIFI_USER);  // anonymous / outer identity

  // ---------------------------------------------------------------------------
  // 2. Campus MQTT broker is open:
  //    no MQTT username/password
  // ---------------------------------------------------------------------------
  iot_mqtt_clear_credentials();

  // ---------------------------------------------------------------------------
  // 3. Connect using plain MQTT, no TLS
  //
  //    nullptr -> iot-b generates a device-specific MQTT client ID
  // ---------------------------------------------------------------------------
  iot_mqtt_begin_plain(MQTT_HOST, MQTT_PORT, nullptr, on_mqtt);

  // ---------------------------------------------------------------------------
  // 4. Subscribe and announce
  // ---------------------------------------------------------------------------
  iot_mqtt_subscribe(TOPIC_SUB);

  iot_mqtt_publish_text(
    TOPIC_STATUS,
    "Device 1 online",
    true);
}


void loop() {
  // ---------------------------------------------------------------------------
  // Reconnect MQTT if necessary
  // ---------------------------------------------------------------------------
  if (!iot_mqtt_connected()) {
    if (iot_mqtt_reconnect()) {
      // Subscriptions must be restored after reconnect
      iot_mqtt_subscribe(TOPIC_SUB);
    }
  }

  // Service MQTT
  iot_mqtt_loop();

  // ---------------------------------------------------------------------------
  // Publish once per second
  // ---------------------------------------------------------------------------
  static uint32_t last_pub_ms = 0;
  static int k = 0;

  uint32_t now = millis();

  if (now - last_pub_ms >= 1000) {
    last_pub_ms = now;

    char message[32];

    snprintf(
      message,
      sizeof(message),
      "Pesan ke-%d.",
      k++);

    iot_mqtt_publish_text(
      TOPIC_DATA,
      message,
      false  // don't retain streaming data
    );
  }
}
