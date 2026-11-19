#pragma once
// ============================================================================
//  MeshController — the BRMesh/FastCon bridge core.
//
//  Owns everything that is *protocol + radio*: the 4-byte mesh key, the light
//  list, FastCon advertising (WAKE / SET_KEY / control), discovery + keying,
//  and NVS persistence of the key + lights. It is stack-specific (NimBLE) but
//  transport-agnostic — it knows nothing about WiFi, MQTT, Home Assistant,
//  HTTP, or the serial console.
//
//  Drivers consume a MeshController instance through the public methods below:
//    * the serial console (Phase 0/1 UI)                 -> console.cpp
//    * HTTP + HA/MQTT (bridge front ends, Phase 1/3)     -> main.cpp / web/
//  Adding a new driver never touches this class.
//
//  The FastCon byte codec (the load-bearing, proven part) lives separately in
//  protocol.{h,cpp} — pure and unit-testable. This class only drives it.
// ============================================================================
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEAdvertising.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <Preferences.h>

#include <functional>
#include <string>
#include <vector>

#include "config.h"
#include "protocol.h"
#include "light.h"

class MeshController {
 public:
  MeshController() = default;

  // ---- lifecycle ----------------------------------------------------------
  /// NimBLE init + restore (or generate + persist) the mesh key. Does NOT
  /// restore the light list — call loadLights() when you're ready. (Bridge mode
  /// needs that ordering: HA discovery is lost if entities exist before
  /// mqtt->begin(); see BRIDGE_PLAN.md §7.)
  void begin();

  /// True once begin() has run. The console uses it to reject radio commands
  /// on a provisioning boot (bridge mode), where the engine is deliberately
  /// never started — Improv owns the BLE radio for that boot.
  bool started() const { return started_; }

  /// Per-loop work: run a pending background scan request (requestScan()).
  /// Cheap to call every loop(); a no-op when nothing is queued.
  void loop();

  // ---- state (read) -------------------------------------------------------
  const uint8_t* key() const { return my_key; }
  const std::vector<LightDevice>& lights() const { return myLights; }
  int lightCount() const;
  LightDevice* findLightById(const std::string& id);
  LightDevice* findLightByMac(const std::string& mac);
  /// Number of the first registered light (used as the default control target).
  uint8_t firstLightNumber();

  // ---- control (broadcast) ------------------------------------------------
  void state(uint8_t number, bool on);
  void brightness(uint8_t number, uint8_t level);      // 0-127
  void colorTemp(uint8_t number, uint16_t kelvin);
  void rgb(uint8_t number, uint8_t r, uint8_t g, uint8_t b);

  // ---- discovery / keying -------------------------------------------------
  /// WAKE + listen for factory-key bulbs, add them to the list (NO keying).
  /// The "bootcatch" behaviour: catch a bulb's identity while power-cycling it.
  void scan(int seconds);
  /// scan() + key every newly-detected factory bulb under our key.
  void addLights(int seconds);
  /// Queue addLights() to run on the next loop() (non-blocking request).
  void requestScan(int seconds = SCAN_DURATION);
  bool isScanning() const { return isScanning_; }

  /// Re-register an already-keyed bulb (list recovery; NO air traffic).
  void readd(const std::string& mac, uint8_t number);
  /// Remove a light from list + NVS by mac ("aa:bb:..") or number. Bulb key
  /// untouched — nothing is sent over the air.
  void forget(const std::string& macOrNumber);
  /// Wipe the light list + its NVS records; keeps our mesh key.
  void clearLights();

  // ---- debug (raw capture / WAKE replay) ----------------------------------
  void raw(int seconds);           // active scan, every frame, unfiltered
  void rawPassive(int seconds);    // passive listen, every frame (one-shot capture)
  void rawWake(int seconds);       // time-sliced WAKE + raw capture
  void wakeFor(int seconds);       // broadcast our WAKE only (no scan)
  void wakePhone(int seconds);     // replay the phone's captured WAKE bytes
  void scanStop();                 // end a running background scan

  // ---- persistence --------------------------------------------------------
  /// Restore registered lights from NVS (fires the onRegistered hook for each).
  void loadLights();
  void saveLights();

  // ---- driver hooks -------------------------------------------------------
  /// Called whenever a light becomes registered — on key and on load. Bridge
  /// mode uses it to create the HA entity; POC leaves it unset (no-op).
  void setOnRegistered(std::function<void(LightDevice&)> cb) { onRegistered_ = std::move(cb); }

 private:
  // --- control plumbing ---
  void armMfgAdvertise(const std::string& sd);
  void singleControl(const uint8_t key[4], const uint8_t data[8]);

  // --- scan plumbing ---
  void waitScanDone(uint32_t timeoutMs);
  void slicedAdvertiseScan(int totalSec, int advMs, int scanMs,
                           NimBLEScanCallbacks* cb, bool active);

  // --- keying ---
  void addLight(uint8_t number, LightDevice& light);

  // --- persistence plumbing ---
  void loadKeyOrGenerate();
  void fireRegistered(LightDevice& l) { if (onRegistered_) onRegistered_(l); }

  // --- NimBLE scan callbacks (nested so they reach private state) ---------
  class AddDeviceCallback : public NimBLEScanCallbacks {
   public:
    explicit AddDeviceCallback(MeshController* m) : m_(m) {}
    void onResult(const NimBLEAdvertisedDevice* dev);
   private:
    MeshController* m_;
  };
  class AddLightCallback : public NimBLEScanCallbacks {
   public:
    explicit AddLightCallback(MeshController* m) : m_(m) {}
    void onResult(const NimBLEAdvertisedDevice* dev);
   private:
    MeshController* m_;
  };
  class RawScanCallback : public NimBLEScanCallbacks {
   public:
    void onResult(const NimBLEAdvertisedDevice* dev);
  };

  // --- state ---
  uint8_t my_key[4] = { 0, 0, 0, 0 };
  std::vector<LightDevice> myLights;
  NimBLEAdvertising* pAdvertising = nullptr;
  NimBLEScan* pBLEScan = nullptr;
  Preferences prefs;
  std::function<void(LightDevice&)> onRegistered_;

  bool scanRequested_ = false;
  int scanSeconds_ = SCAN_DURATION;
  bool isScanning_ = false;
  bool started_ = false;
};
