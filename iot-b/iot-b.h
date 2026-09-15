#pragma once
/**
 * @file iot-b.h
 * @brief Wi‑Fi, MQTT, and RTP/UDP helper utilities for ESP32 (Arduino core).
 *
 * This header centralizes helpers to:
 *  - Connect to personal (WPA/WPA2 PSK) or campus‑style WPA2‑Enterprise networks.
 *  - Initialize and use MQTT (TLS or plaintext) via PubSubClient with sane defaults.
 *  - Build and send minimal **RTP over UDP** packets for real‑time streams.
 *
 * The companion implementation lives in **iot-b.cpp**.
 *
 * @author  Auralius Manurung and ChatGPT
 * @version 1.3
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>

#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_eap_client.h>
#include <time.h>

// =================================================================================================
// \defgroup logging Logging macros
// Lightweight wrappers over ESP‑IDF logging, auto‑tagged with the current function name.
// -------------------------------------------------------------------------------------------------
/** @addtogroup logging
 *  @{ */
/** @brief Error log (tag = current function). */
#define LOGE(...) ESP_LOGE(__func__, __VA_ARGS__)
/** @brief Warning log (tag = current function). */
#define LOGW(...) ESP_LOGW(__func__, __VA_ARGS__)
/** @brief Info log (tag = current function). */
#define LOGI(...) ESP_LOGI(__func__, __VA_ARGS__)
/** @brief Debug log (tag = current function). */
#define LOGD(...) ESP_LOGD(__func__, __VA_ARGS__)
/** @brief Verbose log (tag = current function). */
#define LOGV(...) ESP_LOGV(__func__, __VA_ARGS__)
/** @} */

#ifndef MQTT_BUFFER_SIZE
  /** @brief Default PubSubClient buffer size (bytes). */
  #define MQTT_BUFFER_SIZE 1024
#endif

// =================================================================================================
// Time & Certificates
// -------------------------------------------------------------------------------------------------
/**
 * @brief Minimum acceptable UNIX epoch (2023‑01‑01). If the system time is below this value,
 *        the code will block to sync time via SNTP before attempting TLS.
 */
static const time_t kMinGoodEpoch = 1672531200;

/**
 * @brief Root CA (PEM) for secure MQTT examples (e.g., HiveMQ or EMQX Cloud).
 * @note  Provided as `extern` and defined in @ref iot-b.cpp so it resides in flash.
 */
extern const char hivemq_ca_cert[];
extern const char emqx_ca_cert[];

// =================================================================================================
// Wi‑Fi helpers
// -------------------------------------------------------------------------------------------------
/**
 * @brief Connect to a personal/home Wi‑Fi network (WPA/WPA2 PSK).
 *
 * Strategy:
 *  1. Optionally scan for all BSSIDs under the target SSID and pick the strongest AP/channel.
 *  2. Attempt association locked to that BSSID+channel (with fallback to generic connect).
 *  3. Retry on transient failures until connected (blocking), with basic logging of Wi‑Fi events.
 *
 * @param ssid       Target SSID (non‑null, non‑empty).
 * @param password   Passphrase for the SSID.
 * @param use_bssid  If true, actively scan and lock to the best BSSID.
 * @return `true` on successful association (function blocks until connected), `false` on invalid args.
 * @see connect_to_campus_wifi
 */
bool connect_to_home_wifi(const char *ssid, const char *password, bool use_bssid = false);

/**
 * @brief Connect to a campus WPA/WPA2-Enterprise network using PEAP + MSCHAPv2.
 *
 * This profile matches TelU-Connect: PEAP outer authentication, username/password credentials,
 * optional anonymous outer identity, and no CA certificate. The helper retries bounded connection
 * attempts, logs Wi-Fi disconnect reasons, and returns false instead of rebooting when it cannot connect.
 *
 * @param ssid               Enterprise SSID.
 * @param username           PEAP/MSCHAPv2 username.
 * @param password           PEAP/MSCHAPv2 password.
 * @param outer_identity     Optional anonymous (outer) identity; if nullptr/empty, @p username is used.
 * @param lock_to_best_bssid If true, scan and lock to the strongest BSSID for @p ssid.
 *                           Defaults to false so campus AP roaming/fallback remains available.
 * @return `true` on successful association, `false` on invalid args, EAP setup failure, or retry exhaustion.
 * @note   Requires the Arduino-ESP32 core to include `esp_eap_client`.
 */
bool connect_to_campus_wifi(const char *ssid,
                            const char *username,
                            const char *password,
                            const char *outer_identity = nullptr,
                            bool lock_to_best_bssid = false);

// =================================================================================================
// MQTT helpers (PubSubClient)
// -------------------------------------------------------------------------------------------------
/**
 * @brief Configuration for MQTT connection.
 */
struct MqttConfig {
  const char* server    = nullptr;   //!< MQTT broker hostname.
  uint16_t    port      = 8883;      //!< Port: 8883 (TLS) or 1883 (plaintext).
  const char* client_id = nullptr;   //!< Client ID (required).
  const char* username  = nullptr;   //!< Username (optional).
  const char* password  = nullptr;   //!< Password (optional).

  // Optional Last Will
  const char* topic     = nullptr;   //!< Last Will topic (optional).
  const char* payload   = nullptr;   //!< Last Will payload (optional).
  bool        retain    = true;      //!< Last Will retain flag.
};

/**
 * @brief Ensure system time is sane via SNTP, useful before TLS handshakes.
 *
 * Safe to call multiple times; if time is already sane (>= @ref kMinGoodEpoch) it returns immediately.
 * Otherwise it blocks until time is synced.
 *
 * @return `true` if time is already sane or becomes synced.
 */
bool mqtt_sync_time_if_needed();

/**
 * @brief Configure @ref WiFiClientSecure with a CA certificate or set it insecure.
 *
 * Call after Wi‑Fi has connected. If @p verify_cert is true, @p cert (or a default CA) will be installed;
 * otherwise `setInsecure()` is used (not recommended in production).
 *
 * @param net          Secure client reference.
 * @param verify_cert  Whether to enable CA verification (default: true).
 * @param cert         PEM Root CA to trust (defaults to @ref hivemq_ca_cert).
 */
void mqtt_configure_secure_client(WiFiClientSecure& net,
                                  bool verify_cert = true,
                                  const char* cert = hivemq_ca_cert);

/**
 * @brief Initialize a **secure** MQTT client.
 *
 * Sets the network client, server host/port, MQTT callback, and buffer size.
 *
 * @param client   PubSubClient instance.
 * @param net      Connected @ref WiFiClientSecure.
 * @param host     MQTT host.
 * @param port     MQTT port (usually 8883 for TLS).
 * @param callback PubSubClient callback (message handler).
 */
void mqtt_init(PubSubClient& client, WiFiClientSecure& net,
               const char* host, uint16_t port, MQTT_CALLBACK_SIGNATURE);

/**
 * @brief Initialize a **plaintext** MQTT client.
 * @copydetails mqtt_init(PubSubClient&, WiFiClientSecure&, const char*, uint16_t, MQTT_CALLBACK_SIGNATURE)
 */
void mqtt_init(PubSubClient& client, WiFiClient& net,
               const char* host, uint16_t port, MQTT_CALLBACK_SIGNATURE);

/**
 * @brief Connect to an MQTT broker with linear backoff.
 *
 * If @p max_retries is 0, retries indefinitely; otherwise attempts up to @p max_retries times.
 *
 * @param client       PubSubClient instance.
 * @param cfg          Connection configuration.
 * @param max_retries  Maximum attempts (0 = infinite).
 * @param backoff_ms   Base delay (ms) multiplied by attempt number between retries.
 * @return `true` on successful connection, `false` if max retries exceeded or invalid cfg.
 */
bool mqtt_connect(PubSubClient& client, const MqttConfig& cfg,
                  uint8_t max_retries = 10, uint32_t backoff_ms = 500);

/**
 * @brief Publish a UTF‑8 string message.
 * @param client    PubSubClient.
 * @param topic     Topic string (non‑null).
 * @param payload   Null‑terminated payload.
 * @param retained  Retain flag.
 * @return `true` on success.
 */
bool mqtt_publish(PubSubClient& client, const char* topic,
                  const char* payload, bool retained = true);

/**
 * @brief Publish a binary payload.
 * @param client    PubSubClient.
 * @param topic     Topic string (non‑null).
 * @param payload   Pointer to bytes.
 * @param length    Payload length in bytes.
 * @param retained  Retain flag.
 * @return `true` on success.
 */
bool mqtt_publish(PubSubClient& client, const char* topic,
                  const byte *payload, unsigned int length,
                  bool retained);

bool mqtt_publish_stream(PubSubClient& client,
                         const char* topic,
                         const uint8_t* payload,
                         size_t length,
                         bool retained,
                         size_t chunk_bytes);

 bool mqtt_publish_stream_2seg(PubSubClient& client,
                              const char* topic,
                              const uint8_t* seg1, size_t len1,
                              const uint8_t* seg2, size_t len2,
                              bool retained = false,
                              size_t chunk_bytes = 1024);                        
/**
 * @brief Subscribe to a topic.
 * @param client  PubSubClient.
 * @param topic   Topic string (non‑null).
 * @return `true` on success.
 */
bool mqtt_subscribe(PubSubClient& client, const char* topic);

/**
 * @brief Pump PubSubClient I/O. Call frequently in the main loop.
 */
void mqtt_loop(PubSubClient& client);

// =================================================================================================
// Simple course-facing API
// -------------------------------------------------------------------------------------------------
// These helpers own one internal WiFiClient/PubSubClient/MqttConfig instance inside iot-b.cpp.
// They are additive: the legacy object-based API above remains unchanged and can still be used.
// -------------------------------------------------------------------------------------------------

/** @brief MQTT callback type used by the simplified API. */
using IotMqttCallback = void (*)(char*, byte*, unsigned int);

/** @brief Short alias for @ref connect_to_home_wifi. */
bool iot_wifi_home(const char* ssid, const char* password, bool use_bssid = false);

/** @brief Short alias for @ref connect_to_campus_wifi. */
bool iot_wifi_campus(const char* ssid,
                     const char* username,
                     const char* password,
                     const char* outer_identity = nullptr,
                     bool lock_to_best_bssid = false);

/**
 * @brief Set credentials used by the simplified MQTT client.
 * @note May be called before begin(), or changed later and followed by reconnect().
 */
void iot_mqtt_set_credentials(const char* username, const char* password);

/** @brief Clear username/password for the simplified MQTT client. */
void iot_mqtt_clear_credentials();

/** @brief Set or replace the optional Last Will for the simplified MQTT client. */
void iot_mqtt_set_last_will(const char* topic,
                            const char* payload,
                            bool retain = true);

/** @brief Set the callback used by the simplified MQTT client. */
void iot_mqtt_set_callback(IotMqttCallback callback);

/**
 * @brief Set the PubSubClient MQTT buffer size used by the simplified client.
 * @return true if PubSubClient accepted the requested size.
 */
bool iot_mqtt_set_buffer_size(uint16_t bytes);

/**
 * @brief Configure and connect the library-owned plaintext MQTT client.
 * @param host       Broker hostname.
 * @param port       Broker port, normally 1883.
 * @param client_id  Optional client ID. If null/empty, a MAC-derived ID is generated.
 * @param callback   Optional subscription callback.
 */
bool iot_mqtt_begin_plain(const char* host,
                          uint16_t port = 1883,
                          const char* client_id = nullptr,
                          IotMqttCallback callback = nullptr);

/**
 * @brief Configure and connect the library-owned TLS MQTT client.
 * @param host         Broker hostname.
 * @param port         Broker port, normally 8883.
 * @param client_id    Optional client ID. If null/empty, a MAC-derived ID is generated.
 * @param callback     Optional subscription callback.
 * @param verify_cert  Verify the supplied CA certificate when true.
 * @param cert         Root CA certificate.
 */
bool iot_mqtt_begin_tls(const char* host,
                        uint16_t port = 8883,
                        const char* client_id = nullptr,
                        IotMqttCallback callback = nullptr,
                        bool verify_cert = true,
                        const char* cert = hivemq_ca_cert);

/** @brief Reconnect using the last simplified MQTT configuration. */
bool iot_mqtt_reconnect(uint8_t max_retries = 10,
                        uint32_t backoff_ms = 500);

/** @brief Return true when the simplified MQTT client is connected. */
bool iot_mqtt_connected();

/** @brief Disconnect the simplified MQTT client and its underlying socket. */
void iot_mqtt_disconnect();

/** @brief Publish text with the simplified MQTT client. */
bool iot_mqtt_publish_text(const char* topic,
                           const char* payload,
                           bool retained = false);

/** @brief Publish arbitrary binary bytes with the simplified MQTT client. */
bool iot_mqtt_publish_binary(const char* topic,
                             const uint8_t* payload,
                             size_t length,
                             bool retained = false);

/** @brief Subscribe using the simplified MQTT client. */
bool iot_mqtt_subscribe(const char* topic);

/** @brief Pump MQTT I/O for the simplified MQTT client. Call frequently. */
void iot_mqtt_loop();

/** @brief Return the active/generated simplified MQTT client ID. */
const char* iot_mqtt_client_id();

// =================================================================================================
// RTP / UDP helpers (no classes)
// -------------------------------------------------------------------------------------------------
/**
 * @defgroup rtp RTP/UDP helpers
 * @brief Minimal helpers to pack and send RTP frames over UDP.
 *
 * Header fields are always **big‑endian** (network order) per RTP spec. For convenience, the payload
 * helpers here write **little‑endian int16** samples (to match the project’s prior MQTT framing).
 * Keep `seq` and `ts` counters in your sketch and pass them to the helpers.
 *  @{ */

/**
 * @brief Write a 12‑byte RTP header into @p p (BIG‑ENDIAN fields).
 *
 * Layout: V=2,P=0,X=0,CC=0 | M/PT | sequence | timestamp | SSRC.
 *
 * @param p       Destination buffer (>= 12 bytes).
 * @param seq     RTP sequence number.
 * @param ts      RTP timestamp in the media clock domain.
 * @param ssrc    Synchronization source identifier.
 * @param pt      Payload type (7‑bit dynamic/static).
 * @param marker  Set the M bit (frame boundary/keyframe). Optional, default false.
 */
void rtp_write_header_be(uint8_t* p,
                         uint16_t seq,
                         uint32_t ts,
                         uint32_t ssrc,
                         uint8_t  pt,
                         bool     marker = false);

/**
 * @brief Build a complete RTP packet with **little‑endian void** payload into @p out.
 *
 * The function writes the 12‑byte RTP header (BE) followed by @p n samples in LE order.
 * On success it returns the total packet size (12 + 2*n). If @p out_cap is too small, returns 0.
 *
 * @param samples  Pointer to @p n int16 samples (µV) to serialize (little‑endian in payload).
 * @param n        Number of samples.
 * @param out      Destination buffer.
 * @param out_cap  Capacity of @p out in bytes.
 * @param seq      RTP sequence number.
 * @param ts       RTP timestamp (media clock).
 * @param ssrc     SSRC.
 * @param pt       Payload type.
 * @param marker   Marker bit (optional).
 * @return Packet length in bytes, or 0 if @p out_cap is insufficient.
 *
 * @code
 *   uint8_t pkt[12 + N*2];
 *   size_t len = rtp_build_packet_le(samples, N, pkt, sizeof(pkt), seq, ts, SSRC, 97, false);
 *   if (len) { udp_send(udp, ip, port, pkt, len); rtp_advance(&seq, &ts, N); }
 * @endcode
 */
size_t rtp_build_packet_le(const int16_t* samples,
                           uint16_t n,
                           uint8_t* out,
                           size_t out_cap,
                           uint16_t seq,
                           uint32_t ts,
                           uint32_t ssrc,
                           uint8_t  pt,
                           bool     marker = false);

/**
 * @brief Advance RTP sequence/timestamp counters in‑place.
 * @param seq     Pointer to sequence number (incremented by @p seq_inc, default 1).
 * @param ts      Pointer to timestamp (incremented by @p ts_inc).
 * @param ts_inc  Timestamp increment (e.g., samples per packet).
 * @param seq_inc Sequence increment (default 1).
 */
void rtp_advance(uint16_t* seq,
                 uint32_t* ts,
                 uint32_t  ts_inc,
                 uint16_t  seq_inc = 1);

/**
 * @brief Send a datagram via @ref WiFiUDP.
 * @param udp  UDP handle.
 * @param ip   Destination IPv4 string (e.g., "192.168.1.50").
 * @param port Destination port.
 * @param pkt  Pointer to packet bytes.
 * @param len  Packet length in bytes.
 * @return `true` if all bytes were queued for send.
 */
bool udp_send(WiFiUDP& udp,
              const char* ip,
              uint16_t port,
              const uint8_t* pkt,
              size_t len);

/**
 * @brief Send a tiny datagram to prime ARP/NDP/route caches before streaming.
 * @param udp  UDP handle.
 * @param ip   Destination IPv4 string.
 * @param port Destination port.
 */
void udp_warmup(WiFiUDP& udp,
                const char* ip,
                uint16_t port);

/** @} */ // end of group rtp
