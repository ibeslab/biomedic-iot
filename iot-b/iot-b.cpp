#include "esp_system.h"
/**
 * @file iot-b.cpp
 * @brief Implementation of Wi‑Fi & MQTT helpers for ESP32 (Arduino core).
 */

#include "iot-b.h"
#include <cstring>

// -------------------------------------------------------------------------------------------------
// Simplified course-facing MQTT state
// -------------------------------------------------------------------------------------------------
// Kept private to this translation unit so sketches do not need to instantiate WiFiClient,
// WiFiClientSecure, PubSubClient, or MqttConfig objects.
static WiFiClient       s_iot_net;
static WiFiClientSecure s_iot_secure_net;
static PubSubClient     s_iot_mqtt;
static MqttConfig       s_iot_cfg;
static bool             s_iot_tls = false;
static uint16_t         s_iot_buffer_size = MQTT_BUFFER_SIZE;
static IotMqttCallback  s_iot_callback = nullptr;
static char             s_iot_generated_client_id[40] = {0};

static void iot_noop_callback(char*, byte*, unsigned int) {}

static const char* iot_resolve_client_id(const char* requested) {
  if (requested && *requested) return requested;

  if (!s_iot_generated_client_id[0]) {
    const uint32_t short_id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFULL);
    snprintf(s_iot_generated_client_id,
             sizeof(s_iot_generated_client_id),
             "esp32-%08lX",
             (unsigned long)short_id);
  }
  return s_iot_generated_client_id;
}

// -------------------------------------------------------------------------------------------------
// Wi‑Fi event logging (attach once)
// -------------------------------------------------------------------------------------------------
static bool s_wifiEventsAttached = false;
static constexpr uint8_t  MAX_WIFI_RETRIES = 5;
static constexpr uint32_t CAMPUS_WIFI_ATTEMPT_TIMEOUT_MS = 20000;
static constexpr uint32_t CAMPUS_WIFI_POLL_MS = 250;

static void attach_wifi_events_once() {
  if (s_wifiEventsAttached) return;
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info){
    switch (e) {
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        LOGW("[wifi] DISCONNECTED reason=%d", info.wifi_sta_disconnected.reason);
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        LOGI("[wifi] CONNECTED  BSSID=%02X:%02X:%02X:%02X:%02X:%02X ch=%u",
             info.wifi_sta_connected.bssid[0], info.wifi_sta_connected.bssid[1],
             info.wifi_sta_connected.bssid[2], info.wifi_sta_connected.bssid[3],
             info.wifi_sta_connected.bssid[4], info.wifi_sta_connected.bssid[5],
             info.wifi_sta_connected.channel);
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        LOGI("[wifi] GOT IP    %s", WiFi.localIP().toString().c_str());
        break;
      default:
        LOGD("[wifi] event=%d", (int)e);
        break;
    }
  });
  s_wifiEventsAttached = true;
}

// -------------------------------------------------------------------------------------------------
// Scan & pick best BSSID for a given SSID
// -------------------------------------------------------------------------------------------------
struct BssidPick { uint8_t bssid[6] = {0}; int32_t channel = 0; int32_t rssi = -999; bool found = false; };

static BssidPick pick_best_bssid_for_ssid(const char* ssid) {
  BssidPick pick;
  const int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (n <= 0) {
    LOGW("[wifi] scan found %d networks", n);
  }
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == ssid) {
      const int32_t r = WiFi.RSSI(i);
      const int32_t ch = WiFi.channel(i);
      const uint8_t* b = WiFi.BSSID(i);
      LOGD("[wifi] candidate %s RSSI=%ld ch=%ld BSSID=%02X:%02X:%02X:%02X:%02X:%02X",
           ssid, (long)r, (long)ch, b[0], b[1], b[2], b[3], b[4], b[5]);
      if (r > pick.rssi) {
        pick.rssi = r;
        pick.channel = ch;
        memcpy(pick.bssid, b, 6);
        pick.found = true;
      }
    }
  }
  WiFi.scanDelete();
  if (pick.found) {
    LOGI("[wifi] best BSSID for \"%s\": RSSI=%ld ch=%ld %02X:%02X:%02X:%02X:%02X:%02X",
         ssid, (long)pick.rssi, (long)pick.channel,
         pick.bssid[0], pick.bssid[1], pick.bssid[2],
         pick.bssid[3], pick.bssid[4], pick.bssid[5]);
  } else {
    LOGW("[wifi] no matching BSSID found for \"%s\"", ssid);
  }
  return pick;
}

// -------------------------------------------------------------------------------------------------
// Personal Wi‑Fi (WPA/WPA2 PSK)
// -------------------------------------------------------------------------------------------------
bool connect_to_home_wifi(const char *ssid, const char *password, bool use_bssid) {
  if (!ssid || !*ssid) {
    LOGE("empty SSID");
    return false;
  }

  attach_wifi_events_once();

  WiFi.persistent(false);     // avoid NVS churn
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);       // keep radio awake during auth/handshake

  // (Optional) set country (ID = Indonesia; channels 1..13). Adjust for your region.
  wifi_country_t c = { "ID", 1, 13, WIFI_COUNTRY_POLICY_MANUAL };
  esp_wifi_set_country(&c);

  WiFi.disconnect(true, true);
  delay(1000);

  BssidPick pick;
  if (use_bssid) {
    pick = pick_best_bssid_for_ssid(ssid);
  }

  if (use_bssid && pick.found) {
    LOGI("Connecting to \"%s\" via best BSSID %02X:%02X:%02X:%02X:%02X:%02X ch=%ld",
         ssid,
         pick.bssid[0], pick.bssid[1], pick.bssid[2],
         pick.bssid[3], pick.bssid[4], pick.bssid[5],
         (long)pick.channel);
    WiFi.begin(ssid, password, pick.channel, pick.bssid, true);
  } else {
    LOGI("Connecting to \"%s\" (generic)", ssid);
    WiFi.begin(ssid, password);
  }

  wl_status_t st = WL_IDLE_STATUS;
  uint32_t wifi_connect_retries = 0;

  while ((st = WiFi.status()) != WL_CONNECTED) {
    delay(2000);

    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
      wifi_connect_retries = wifi_connect_retries + 1;
      if (wifi_connect_retries >= MAX_WIFI_RETRIES) esp_restart();

      LOGW("quick re-begin due to status=%d", (int)st);
      WiFi.disconnect(false, false);
      delay(500);
      static uint32_t last_scan_ms = 0;
      uint32_t now = millis();
      if (use_bssid && (now - last_scan_ms > 10000)) {
        pick = pick_best_bssid_for_ssid(ssid);
        last_scan_ms = now;
      }
      if (use_bssid && pick.found) WiFi.begin(ssid, password, pick.channel, pick.bssid, true);
      else                         WiFi.begin(ssid, password);
    }
  }

  LOGI("connected. IP=%s RSSI=%d",
       WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// -------------------------------------------------------------------------------------------------
// WPA2‑Enterprise
// -------------------------------------------------------------------------------------------------
bool connect_to_campus_wifi(const char *ssid,
                            const char *username,
                            const char *password,
                            const char *outer_identity,
                            bool lock_to_best_bssid) {
  if (!ssid || !*ssid || !username || !*username || !password || !*password) {
    LOGE("invalid args");
    return false;
  }

  attach_wifi_events_once();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // Clear any previously saved AP selection, but keep the Wi-Fi radio enabled.
  WiFi.disconnect(false, true);
  delay(300);

  // Indonesia: channels 1..13.
  wifi_country_t c = { "ID", 1, 13, WIFI_COUNTRY_POLICY_MANUAL };
  esp_err_t err = esp_wifi_set_country(&c);
  if (err != ESP_OK) {
    LOGW("esp_wifi_set_country failed: %d", (int)err);
  }

  const char* identity = (outer_identity && *outer_identity) ? outer_identity : username;

  // TelU-Connect profile:
  //   outer EAP : PEAP
  //   inner auth: MSCHAPv2 (handled by PEAP username/password credentials)
  //   CA cert   : not required by the campus profile
  err = esp_eap_client_set_identity((const uint8_t*)identity, strlen(identity));
  if (err != ESP_OK) {
    LOGE("set EAP identity failed: %d", (int)err);
    return false;
  }

  err = esp_eap_client_set_username((const uint8_t*)username, strlen(username));
  if (err != ESP_OK) {
    LOGE("set EAP username failed: %d", (int)err);
    return false;
  }

  err = esp_eap_client_set_password((const uint8_t*)password, strlen(password));
  if (err != ESP_OK) {
    LOGE("set EAP password failed: %d", (int)err);
    return false;
  }

  err = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP);
  if (err != ESP_OK) {
    LOGE("select PEAP failed: %d", (int)err);
    return false;
  }

  // Match the campus client profile: no CA certificate is required.
  err = esp_eap_client_set_ca_cert(nullptr, 0);
  if (err != ESP_OK) {
    LOGE("clear EAP CA certificate failed: %d", (int)err);
    return false;
  }

  err = esp_wifi_sta_enterprise_enable();
  if (err != ESP_OK) {
    LOGE("enable enterprise Wi-Fi failed: %d", (int)err);
    return false;
  }

  BssidPick pick = {};

  for (uint8_t attempt = 1; attempt <= MAX_WIFI_RETRIES; ++attempt) {
    if (attempt > 1) {
      WiFi.disconnect(false, false);
      delay(500);
    }

    if (lock_to_best_bssid) {
      // Re-scan on every retry so a stale/weak AP is not kept forever.
      pick = pick_best_bssid_for_ssid(ssid);
      if (pick.found) {
        LOGI("campus attempt %u/%u: PEAP connect to '%s' via BSSID "
             "%02X:%02X:%02X:%02X:%02X:%02X ch=%ld RSSI=%ld",
             (unsigned)attempt, (unsigned)MAX_WIFI_RETRIES,
             ssid,
             pick.bssid[0], pick.bssid[1], pick.bssid[2],
             pick.bssid[3], pick.bssid[4], pick.bssid[5],
             (long)pick.channel, (long)pick.rssi);
        WiFi.begin(ssid, "", pick.channel, pick.bssid, true);
      } else {
        LOGW("campus attempt %u/%u: '%s' not found by scan; trying generic PEAP connect",
             (unsigned)attempt, (unsigned)MAX_WIFI_RETRIES, ssid);
        WiFi.begin(ssid);
      }
    } else {
      LOGI("campus attempt %u/%u: PEAP connect to '%s' (AP selection unlocked)",
           (unsigned)attempt, (unsigned)MAX_WIFI_RETRIES, ssid);
      WiFi.begin(ssid);
    }

    const uint32_t started_ms = millis();
    uint32_t last_progress_ms = started_ms;
    wl_status_t st = WiFi.status();

    while ((uint32_t)(millis() - started_ms) < CAMPUS_WIFI_ATTEMPT_TIMEOUT_MS) {
      st = WiFi.status();
      if (st == WL_CONNECTED) {
        LOGI("campus connected. IP=%s RSSI=%d",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
      }

      // No SSID / immediate connection failure should retry promptly rather than
      // waiting the full enterprise-auth timeout.
      if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) {
        LOGW("campus attempt %u failed early, status=%d",
             (unsigned)attempt, (int)st);
        break;
      }

      const uint32_t now = millis();
      if ((uint32_t)(now - last_progress_ms) >= 5000) {
        LOGI("campus attempt %u: waiting for PEAP/DHCP, status=%d elapsed=%lu ms",
             (unsigned)attempt, (int)st,
             (unsigned long)(now - started_ms));
        last_progress_ms = now;
      }

      delay(CAMPUS_WIFI_POLL_MS);
      yield();
    }

    st = WiFi.status();
    LOGW("campus attempt %u/%u did not connect (status=%d)",
         (unsigned)attempt, (unsigned)MAX_WIFI_RETRIES, (int)st);
  }

  WiFi.disconnect(false, false);
  LOGE("campus Wi-Fi failed after %u attempts; returning false",
       (unsigned)MAX_WIFI_RETRIES);
  return false;
}

// -------------------------------------------------------------------------------------------------
// Time sync (SNTP) for TLS
// -------------------------------------------------------------------------------------------------
static bool mqtt_sync_time_internal() {
  static bool ntpConfigured = false;
  if (!ntpConfigured) {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    ntpConfigured = true;
    LOGI("NTP config issued");
  }

  time_t now = 0;
  time(&now);
  if (now >= kMinGoodEpoch) {
    LOGI("time already synced: %ld", (long)now);
    return true;
  }

  // Loop until SNTP gives a good epoch
  while (true) {
    delay(250);
    yield();
    time(&now);
    if (now >= kMinGoodEpoch) {
      LOGI("time synced: %ld", (long)now);
      return true;
    }
  }
}

bool mqtt_sync_time_if_needed() {
  time_t now = 0;
  time(&now);
  if (now >= kMinGoodEpoch) return true;
  return mqtt_sync_time_internal();
}

// -------------------------------------------------------------------------------------------------
// Secure client configuration
// -------------------------------------------------------------------------------------------------
extern const char hivemq_ca_cert[];

void mqtt_configure_secure_client(WiFiClientSecure& net, bool verify_cert, const char *cert) {
  mqtt_sync_time_if_needed();
  if (verify_cert) {
    net.setCACert(cert);
    LOGI("secure client: CA cert set");
  } else {
    net.setInsecure();
    LOGW("secure client: setInsecure()");
  }
  net.setHandshakeTimeout(15);
}

// -------------------------------------------------------------------------------------------------
// MQTT init (secure/plain)
// -------------------------------------------------------------------------------------------------
void mqtt_init(PubSubClient& client, WiFiClientSecure& net,
               const char* host, uint16_t port, MQTT_CALLBACK_SIGNATURE) {
  client.setClient(net);
  client.setServer(host, port);
  client.setCallback(callback);
  client.setBufferSize(MQTT_BUFFER_SIZE);
  LOGI("MQTT init (secure) host=%s port=%u", host, port);
}

void mqtt_init(PubSubClient& client, WiFiClient& net,
               const char* host, uint16_t port, MQTT_CALLBACK_SIGNATURE) {
  client.setClient(net);
  client.setServer(host, port);
  client.setCallback(callback);
  client.setBufferSize(MQTT_BUFFER_SIZE);
  LOGI("MQTT init (plain) host=%s port=%u", host, port);
}

// -------------------------------------------------------------------------------------------------
// MQTT connect/publish/subscribe/loop
// -------------------------------------------------------------------------------------------------
bool mqtt_connect(PubSubClient& client, const MqttConfig& cfg, 
                  uint8_t max_retries, uint32_t backoff_ms) {
  if (!cfg.server || !cfg.client_id) {
    LOGE("mqtt_connect: missing server/client_id");
    return false;
  }

  uint32_t attempt = 1;
  while (true) {
    bool ok = false;

    if (cfg.username && cfg.password) {
      if (cfg.topic && cfg.payload) {
        ok = client.connect(cfg.client_id, cfg.username, cfg.password,
                            cfg.topic, 0, cfg.retain, cfg.payload);
      } else {
        ok = client.connect(cfg.client_id, cfg.username, cfg.password);
      }
    } else {
      if (cfg.topic && cfg.payload) {
        ok = client.connect(cfg.client_id, nullptr, nullptr,
                            cfg.topic, 0, cfg.retain, cfg.payload);
      } else {
        ok = client.connect(cfg.client_id);
      }
    }

    if (ok) {
      LOGI("MQTT connected");
      return true;
    }

    LOGW("MQTT connect failed (state=%d) attempt %lu", client.state(), (unsigned long)attempt);

    if (max_retries != 0 && attempt >= max_retries) {
      LOGE("MQTT connect giving up after %lu attempts → reboot!", (unsigned long)attempt);
      esp_restart();
      return false;
    }

    delay(backoff_ms);
    yield();
    attempt++;
  }
}

bool mqtt_publish(PubSubClient& client, const char* topic,
                  const char *payload, bool retained) {
  if (!topic) {
    LOGE("publish: null topic");
    return false;
  }
  bool ok = client.publish(topic, payload, retained);
  if (!ok) LOGW("publish failed (state=%d)", client.state());
  return ok;
}

bool mqtt_publish(PubSubClient& client, const char* topic,
                  const byte *payload, unsigned int length,
                  bool retained) {
  if (!topic) {
    LOGE("publish: null topic");
    return false;
  }
  bool ok = client.publish(topic, payload, length, retained);
  if (!ok) LOGW("publish failed (state=%d)", client.state());
  return ok;
}

bool mqtt_publish_stream(PubSubClient& client,
                         const char* topic,
                         const uint8_t* payload,
                         size_t length,
                         bool retained,
                         size_t chunk_bytes) {
  if (!topic || !payload) {
    LOGE("publish_stream: null topic/payload");
    return false;
  }
  if (!client.beginPublish(topic, length, retained)) {
    LOGW("beginPublish failed (state=%d)", client.state());
    return false;
  }
  size_t written = 0;
  while (written < length) {
    size_t n = length - written;
    if (n > chunk_bytes) n = chunk_bytes;
    client.write(payload + written, n);
    written += n;
    yield();  // keep TCP/Wi-Fi healthy
  }
  if (!client.endPublish()) {
    LOGW("endPublish failed (state=%d)", client.state());
    return false;
  }
  return true;
}

bool mqtt_publish_stream_2seg(PubSubClient& client,
                              const char* topic,
                              const uint8_t* seg1, size_t len1,
                              const uint8_t* seg2, size_t len2,
                              bool retained,
                              size_t chunk_bytes) {
  if (!topic || (!seg1 && len1) || (!seg2 && len2)) {
    LOGE("publish_stream_2seg: null segment");
    return false;
  }
  const size_t total = len1 + len2;
  if (!client.beginPublish(topic, total, retained)) {
    LOGW("beginPublish failed (state=%d)", client.state());
    return false;
  }
  // write seg1
  size_t written = 0;
  while (written < len1) {
    size_t n = len1 - written;
    if (n > chunk_bytes) n = chunk_bytes;
    client.write(seg1 + written, n);
    written += n;
    yield();
  }
  // write seg2
  written = 0;
  while (written < len2) {
    size_t n = len2 - written;
    if (n > chunk_bytes) n = chunk_bytes;
    client.write(seg2 + written, n);
    written += n;
    yield();
  }
  if (!client.endPublish()) {
    LOGW("endPublish failed (state=%d)", client.state());
    return false;
  }
  return true;
}

bool mqtt_subscribe(PubSubClient& client, const char* topic) {
  if (!topic) {
    LOGE("subscribe: null topic");
    return false;
  }
  bool ok = client.subscribe(topic);
  if (!ok) LOGW("subscribe failed for '%s' (state=%d)", topic, client.state());
  else     LOGI("subscribed to '%s'", topic);
  return ok;
}

void mqtt_loop(PubSubClient& client) {
  client.loop();
}

// -------------------------------------------------------------------------------------------------
// Simplified course-facing API
// -------------------------------------------------------------------------------------------------

bool iot_wifi_home(const char* ssid, const char* password, bool use_bssid) {
  return connect_to_home_wifi(ssid, password, use_bssid);
}

bool iot_wifi_campus(const char* ssid,
                     const char* username,
                     const char* password,
                     const char* outer_identity,
                     bool lock_to_best_bssid) {
  return connect_to_campus_wifi(ssid,
                                username,
                                password,
                                outer_identity,
                                lock_to_best_bssid);
}

void iot_mqtt_set_credentials(const char* username, const char* password) {
  s_iot_cfg.username = username;
  s_iot_cfg.password = password;
}

void iot_mqtt_clear_credentials() {
  s_iot_cfg.username = nullptr;
  s_iot_cfg.password = nullptr;
}

void iot_mqtt_set_last_will(const char* topic,
                            const char* payload,
                            bool retain) {
  s_iot_cfg.topic = topic;
  s_iot_cfg.payload = payload;
  s_iot_cfg.retain = retain;
}

void iot_mqtt_set_callback(IotMqttCallback callback) {
  s_iot_callback = callback;
  s_iot_mqtt.setCallback(callback ? callback : iot_noop_callback);
}

bool iot_mqtt_set_buffer_size(uint16_t bytes) {
  if (bytes == 0) return false;
  s_iot_buffer_size = bytes;
  return s_iot_mqtt.setBufferSize(bytes);
}

bool iot_mqtt_begin_plain(const char* host,
                          uint16_t port,
                          const char* client_id,
                          IotMqttCallback callback) {
  if (!host || !*host) {
    LOGE("iot_mqtt_begin_plain: empty host");
    return false;
  }

  s_iot_tls = false;
  s_iot_cfg.server = host;
  s_iot_cfg.port = port;
  s_iot_cfg.client_id = iot_resolve_client_id(client_id);

  if (callback) s_iot_callback = callback;

  mqtt_init(s_iot_mqtt,
            s_iot_net,
            host,
            port,
            s_iot_callback ? s_iot_callback : iot_noop_callback);

  s_iot_mqtt.setBufferSize(s_iot_buffer_size);
  return mqtt_connect(s_iot_mqtt, s_iot_cfg);
}

bool iot_mqtt_begin_tls(const char* host,
                        uint16_t port,
                        const char* client_id,
                        IotMqttCallback callback,
                        bool verify_cert,
                        const char* cert) {
  if (!host || !*host) {
    LOGE("iot_mqtt_begin_tls: empty host");
    return false;
  }

  s_iot_tls = true;
  s_iot_cfg.server = host;
  s_iot_cfg.port = port;
  s_iot_cfg.client_id = iot_resolve_client_id(client_id);

  if (callback) s_iot_callback = callback;

  mqtt_configure_secure_client(s_iot_secure_net,
                               verify_cert,
                               cert ? cert : hivemq_ca_cert);

  mqtt_init(s_iot_mqtt,
            s_iot_secure_net,
            host,
            port,
            s_iot_callback ? s_iot_callback : iot_noop_callback);

  s_iot_mqtt.setBufferSize(s_iot_buffer_size);
  return mqtt_connect(s_iot_mqtt, s_iot_cfg);
}

bool iot_mqtt_reconnect(uint8_t max_retries, uint32_t backoff_ms) {
  if (s_iot_mqtt.connected()) return true;
  return mqtt_connect(s_iot_mqtt, s_iot_cfg, max_retries, backoff_ms);
}

bool iot_mqtt_connected() {
  return s_iot_mqtt.connected();
}

void iot_mqtt_disconnect() {
  s_iot_mqtt.disconnect();
  if (s_iot_tls) s_iot_secure_net.stop();
  else           s_iot_net.stop();
}

bool iot_mqtt_publish_text(const char* topic,
                           const char* payload,
                           bool retained) {
  return mqtt_publish(s_iot_mqtt, topic, payload, retained);
}

bool iot_mqtt_publish_binary(const char* topic,
                             const uint8_t* payload,
                             size_t length,
                             bool retained) {
  if (length > 0xFFFFFFFFu) {
    LOGE("binary publish too large");
    return false;
  }
  return mqtt_publish(s_iot_mqtt,
                      topic,
                      (const byte*)payload,
                      (unsigned int)length,
                      retained);
}

bool iot_mqtt_subscribe(const char* topic) {
  return mqtt_subscribe(s_iot_mqtt, topic);
}

void iot_mqtt_loop() {
  mqtt_loop(s_iot_mqtt);
}

const char* iot_mqtt_client_id() {
  return s_iot_cfg.client_id ? s_iot_cfg.client_id : iot_resolve_client_id(nullptr);
}

void mqtt_hard_reset(PubSubClient& client, WiFiClientSecure& net) {
  LOGE("HARD RESET!");
  client.disconnect();   // PubSubClient
  net.stop();            // WiFiClientSecure socket
  delay(50);
}


// -------------------------------------------------------------------------------------------------
// RTP / UDP helpers
// -------------------------------------------------------------------------------------------------
void rtp_write_header_be(uint8_t* p,
                         uint16_t seq,
                         uint32_t ts,
                         uint32_t ssrc,
                         uint8_t  pt,
                         bool     marker) {
  // Byte 0: V=2, P=0, X=0, CC=0
  p[0] = 0x80;
  // Byte 1: M + PT
  p[1] = (marker ? 0x80 : 0x00) | (pt & 0x7F);
  // Sequence (BE)
  p[2] = (uint8_t)(seq >> 8);
  p[3] = (uint8_t)(seq & 0xFF);
  // Timestamp (BE)
  p[4] = (uint8_t)(ts >> 24);
  p[5] = (uint8_t)(ts >> 16);
  p[6] = (uint8_t)(ts >> 8);
  p[7] = (uint8_t)(ts & 0xFF);
  // SSRC (BE)
  p[8]  = (uint8_t)(ssrc >> 24);
  p[9]  = (uint8_t)(ssrc >> 16);
  p[10] = (uint8_t)(ssrc >> 8);
  p[11] = (uint8_t)(ssrc & 0xFF);
}

size_t rtp_build_packet_le(const void* samples,
                               uint16_t n,
                               size_t   elem_size,
                               uint8_t* out,
                               size_t   out_cap,
                               uint16_t seq,
                               uint32_t ts,
                               uint32_t ssrc,
                               uint8_t  pt,
                               bool     marker) {
  if (!samples || !out || elem_size == 0) return 0;
  const size_t payload_bytes = (size_t)n * elem_size;
  const size_t need = 12 + payload_bytes;
  if (out_cap < need) return 0;

  // RTP header (network order)
  rtp_write_header_be(out, seq, ts, ssrc, pt, marker);

  // Payload (ESP32 is little-endian → copy as-is)
  memcpy(out + 12, samples, payload_bytes);
  return need;
}

void rtp_advance(uint16_t* seq,
                 uint32_t* ts,
                 uint32_t  ts_inc,
                 uint16_t  seq_inc) {
  *seq = (uint16_t)(*seq + seq_inc);
  *ts  = *ts + ts_inc;
}

bool udp_send(WiFiUDP& udp,
              const char* ip,
              uint16_t port,
              const uint8_t* pkt,
              size_t len) {
  udp.beginPacket(ip, port);
  const int w = udp.write(pkt, len);
  udp.endPacket();
  return w == (int)len;
}

void udp_warmup(WiFiUDP& udp,
                const char* ip,
                uint16_t port) {
  udp.beginPacket(ip, port);
  udp.write((const uint8_t*)"hi", 2);
  udp.endPacket();
}


// -------------------------------------------------------------------------------------------------
// Embedded Root CA (example for HiveMQ Cloud). Define here to live in flash.
// -------------------------------------------------------------------------------------------------
// Root CA for secure MQTT (HiveMQ Cloud). Update as needed.
const char hivemq_ca_cert[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// -------------------------------------------------------------------------------------------------
// Embedded Root CA (example for HiveMQ Cloud). Define here to live in flash.
// -------------------------------------------------------------------------------------------------
// Root CA for secure MQTT (HiveMQ Cloud). Update as needed.
const char emqx_ca_cert[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB
CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97
nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt
43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P
T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4
gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO
BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR
TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw
DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr
hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg
06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF
PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls
YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk
CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=
-----END CERTIFICATE-----
)EOF";