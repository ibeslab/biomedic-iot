#include "iot-b.h"

const char* WIFI_SSID = "Polo";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* MQTT_HOST = "d1220813.ala.asia-southeast1.emqxsl.com";
const uint16_t MQTT_PORT = 8883;

const char* MQTT_USER = "Device01";
const char* MQTT_PASS = "Device01";

const char* TOPIC_SUB    = "Device01/#";
const char* TOPIC_STATUS = "Device01/status";
const char* TOPIC_DATA   = "Device01/topic1";


// Called whenever an MQTT message arrives
void on_mqtt(char* topic, byte* payload, unsigned int length) {
  Serial.printf("MQTT[%s] ", topic);

  for (unsigned int i = 0; i < length; i++) 
    Serial.write(payload[i]);

  Serial.println();
}


void setup() {
  Serial.begin(115200);

  // 1. Wi-Fi
  iot_wifi_home(WIFI_SSID, WIFI_PASS);

  // 2. MQTT credentials
  iot_mqtt_set_credentials(MQTT_USER, MQTT_PASS);

  // 3. Connect to MQTT using TLS
  iot_mqtt_begin_tls(
    MQTT_HOST, MQTT_PORT,
    "Device01",
    on_mqtt,
    true,
    emqx_ca_cert
  );

  // 4. Subscribe and announce
  iot_mqtt_subscribe(TOPIC_SUB);
  iot_mqtt_publish_text(TOPIC_STATUS, "Device 1 online", true);
}


void loop() {
  // Keep MQTT connected
  if (!iot_mqtt_connected()) {
    if (iot_mqtt_reconnect()) {
      // MQTT subscriptions are lost after reconnect
      iot_mqtt_subscribe(TOPIC_SUB);
    }
  }

  // Service MQTT
  iot_mqtt_loop();

  // Publish every second
  static unsigned long last_pub_ms = 0;
  static int k = 0;

  unsigned long now = millis();

  if (now - last_pub_ms >= 1000) {
    char message[32];

    snprintf(message, sizeof(message), "Pesan ke-%d.", k++);
    iot_mqtt_publish_text(TOPIC_DATA, message, true);
    last_pub_ms = now;
  }
}
