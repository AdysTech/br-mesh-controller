#pragma once
// ============================================================================
//  HaBridge — Phase 3: the Home Assistant MQTT bridge (BRIDGE_PLAN §7/§8).
//
//  Standard *device-based* discovery (HA ≥ 2026.4 key set: dev/o/avty/cmps +
//  uniq_id/via_device, NO object_id), plain retained QoS1 JSON — no
//  home-assistant-integration library. One MQTT client (esp-mqtt, ESP-IDF
//  core) publishes:
//
//    * the hub device (controller): sensors (scan state, lights, RSSI, MQTT
//      link, queue depth) + a "Start scan" button;
//    * one bulb device per registered light (light entity: on/off, brightness
//      0-255, RGB, colour temperature).
//
//  Topics (all under brmesh/<ctrl_id>/, ctrl_id = brmesh_<MAC last 6 hex>):
//    status                    hub availability (birth "online" / LWT "offline")
//    state                     hub sensor JSON (retained, refreshed every 30 s)
//    scan                      command: any payload -> requestScan()
//    l<n>/state | l<n>/set     bulb state JSON (retained) | HA set command
//    l<n>/availability        bulb availability (with the hub status in the
//                             bulb's avty list, so ONE LWT takes all down)
//
//  Rule: nothing is published before MQTT_EVENT_CONNECTED (birth + discovery +
//  state go out on CONNECTED and on `homeassistant/status` = "online").
//  Discovery/state payloads are built with snprintf — this board has no JSON
//  lib; the small JSON readers live in web/ApiHttp.h.
// ============================================================================
#include <Arduino.h>
#include <string>
#include <mqtt_client.h>

#include "MeshController.h"
#include "settings.h"

class HaBridge {
 public:
  explicit HaBridge(MeshController& mesh) : mesh_(mesh) {}

  /// Create + start the MQTT client. No-op (bridge disabled) when no broker
  /// host is configured — the web/console `mhost` is the switch.
  void begin(const Settings& s);

  /// Per-loop: refresh the retained hub-state JSON (~30 s cadence) and run the
  /// MQTT reconnect backoff (stop the client after repeated failures, restart
  /// after the cooldown — see config.h).
  void loop();

  /// Live-update identity (mname) after a settings save; re-publishes the hub
  /// when connected. Broker host/port/user/pass/mclient changes apply next boot.
  void updateSettings(const Settings& s);

  // ---- MeshController hooks ---------------------------------------------
  /// New bulb (keyed or restored at load): availability + discovery + state.
  void bulbRegistered(LightDevice& l);
  /// Renamed (FR-13): re-publish the bulb's discovery config (ids unchanged).
  void bulbRenamed(uint8_t number);
  /// Forgotten (FR-12): purge the bulb's discovery (empty retained payload)
  /// so HA drops the device — even when the purge happens offline.
  void bulbForgotten(uint8_t number);
  /// Scan state flipped (started/ended): re-publish the hub state so the HA
  /// "Scan" sensor tracks it live. isScanning_ is only true during the blocking
  /// scan, which the periodic hub-state publish can't land within — so we push
  /// the state at the transition instead. (publishHubState reads the live flag.)
  void notifyScanState(bool scanning);

  // ---- introspection (web /api/status, console) ---------------------------
  bool enabled() const { return enabled_; }
  bool connected() const { return connected_; }
  const std::string& ctrlId() const { return ctrlId_; }
  const std::string& clientId() const { return clientId_; }

 private:
  MeshController& mesh_;
  esp_mqtt_client_handle_t client_ = nullptr;
  bool enabled_ = false;    // a broker is configured and the client exists
  bool connected_ = false;  // MQTT session is up (events)

  // identity + connection params (stable storage for the esp-mqtt config,
  // whose fields are const char*)
  std::string ctrlId_;
  std::string clientId_;
  std::string statusTopic_;   // stable storage for the LWT topic pointer
  std::string host_;
  uint16_t port_ = 1883;
  std::string user_;
  std::string pass_;
  std::string hubName_;   // dev.name — mname, or `name` when empty
                           // (dev.cu is always derived at publish time: http://<board IP>/)

  uint32_t lastHubStateMs_ = 0;

  // reconnect backoff (see config.h): connectFails_ is bumped in the event
  // context; loop() (app context) does the stop/restart — esp_mqtt_client_stop()
  // blocks on the client task, so it must never run inside the handler.
  uint8_t  connectFails_ = 0;
  bool     stopped_ = false;
  uint32_t nextRestartMs_ = 0;

  // bulbs forgotten while disconnected: purged on the next publishAll()
  uint8_t pendingPurge_[8];
  int     pendingPurgeCount_ = 0;
  SemaphoreHandle_t purgeMux_ = nullptr;

  // ---- topic builders -----------------------------------------------------
  std::string statusTopic() const;
  std::string hubStateTopic() const;
  std::string scanTopic() const;
  std::string lightStateTopic(uint8_t n) const;
  std::string lightSetTopic(uint8_t n) const;
  std::string lightAvtyTopic(uint8_t n) const;
  std::string discoveryTopic(const std::string& uid) const;

  // ---- publishing (retained QoS1, only while connected) -------------------
  void pub(const std::string& topic, const std::string& payload, bool retain);
  void purge(const std::string& topic);                      // empty retained
  std::string hubDiscoveryJson() const;
  std::string bulbDiscoveryJson(const LightDevice& l) const;
  void publishHub();                        // hub discovery + hub state
  void publishBulbDiscovery(const LightDevice& l);
  void publishHubState();
  void publishAll();                        // connect-time: everything
  void purgeBulb(uint8_t n);                // discovery + state + avty

  // ---- event routing (esp-mqtt task context) -------------------------------
  static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
  void subscribeAll();
  void handleData(const esp_mqtt_event_t* e);
  void applyLightCommand(uint8_t n, const std::string& body);
};
