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
//  The FastCon byte codec — the load-bearing part — lives separately in
//  protocol.{h,cpp}, pure and unit-testable. This class only drives it.
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
  LightDevice* findLightByNumber(uint8_t number);
  /// Number of the first registered light (used as the default control target).
  uint8_t firstLightNumber();

  // ---- control (broadcast) ------------------------------------------------
  // Every control STAMPS the believed state and ENQUEUES the light — the pacer
  // in loop() is the only sender (BRIDGE_PLAN §8; nothing transmits directly).
  // Returns true when the command was accepted, false when rejected (a scan is
  // running — the queue and the scan are mutually exclusive, §8.2).
  bool state(uint8_t number, bool on);
  bool brightness(uint8_t number, uint8_t level);      // 0-127
  /// Colour temperature — the separate white LED (RGBW). `t` is a Kelvin label,
  /// 2700 (warm) → 6000 (cool); split into a WW/CW crossfade only at TX
  /// (protocol.md §2.5b).
  bool colorTemp(uint8_t number, uint16_t t);
  bool rgb(uint8_t number, uint8_t r, uint8_t g, uint8_t b);

  /// Queue a light for transmission (latest-wins per light; rejected while a
  /// scan owns the radio). The control methods call this; drivers may too.
  bool enqueue(uint8_t number);
  int queueDepth() const;        // pending commands (P3.9 /api/status)
  unsigned long queueOverflow() const;  // commands dropped for a full queue

  // ---- discovery / keying -------------------------------------------------
  /// WAKE + listen for factory-key bulbs, add them to the list (NO keying).
  void scan(int seconds);
  /// scan() + key every newly-detected factory bulb under our key.
  void addLights(int seconds);
  /// Queue addLights() to run on the next loop() (non-blocking request).
  void requestScan(int seconds = SCAN_DURATION);
  bool isScanning() const { return isScanning_; }

  /// Rename a light by number (the friendly name; the HA object id and the
  /// number never change). Persists to NVS. Returns false when no light has
  /// that number. (FR-13.)
  bool rename(uint8_t number, const std::string& name);
  /// Remove a light from list + NVS by mac ("aa:bb:..") or number. Bulb key
  /// untouched — nothing is sent over the air.
  void forget(const std::string& macOrNumber);
  /// Wipe the light list + its NVS records; keeps our mesh key.
  void clearLights();

  // ---- debug (raw capture / WAKE replay) ----------------------------------
  void raw(int seconds);           // active scan, every frame, unfiltered
  void rawPassive(int seconds);    // passive listen, every frame (one-shot capture)
  void rawWake(int seconds);       // time-sliced WAKE + raw capture
  void scanStop();                 // end a running background scan

  // ---- persistence --------------------------------------------------------
  /// Restore registered lights from NVS (fires the onRegistered hook for each).
  void loadLights();
  void saveLights();
  /// Backup/restore (web /api/restore): overwrite the stored mesh key with a
  /// restored 4-byte key. The web handler calls this, then reboots — begin()'s
  /// loadKeyOrGenerate() picks the restored key up and no re-keying happens.
  void restoreKey(const uint8_t key[4]);
  /// Replace the light list (registered entries only) and persist it. The web
  /// restore handler calls this, then reboots so loadLights() re-fires the
  /// onRegistered hook (HA re-publish) for the restored list.
  void restoreLights(const std::vector<LightDevice>& lights);

  // ---- driver hooks -------------------------------------------------------
  /// Called whenever a light becomes registered — on key and on load. Bridge
  /// mode uses it to create the HA entity (publish discovery + retained state);
  /// POC leaves it unset (no-op).
  void setOnRegistered(std::function<void(LightDevice&)> cb) { onRegistered_ = std::move(cb); }
  /// Renamed by number (FR-13) — the bridge re-publishes the bulb's discovery
  /// config with the new dev.name (ids/uniq_id unchanged, §7.2).
  void setOnRenamed(std::function<void(uint8_t)> cb) { onRenamed_ = std::move(cb); }
  /// Forgotten (FR-12) — the bridge purges the bulb's discovery (empty retained
  /// payload on the config topic) so HA drops the device.
  void setOnForgotten(std::function<void(uint8_t)> cb) { onForgotten_ = std::move(cb); }

 private:
  // --- control plumbing ---
  void armMfgAdvertise(const std::string& sd);
  void singleControl(const uint8_t key[4], const uint8_t data[8]);
  // Send ONE broadcast carrying a light's ENTIRE desired state (on + brightness
  // + colour/white). There is no "brightness-only" or "colour-only" TX — this is
  // the single sender behind state/brightness/colorTemp/rgb AND the re-transmit.
  void applyLight(const LightDevice& l);

  // --- scan plumbing ---
  void waitScanDone(uint32_t timeoutMs);
  void slicedAdvertiseScan(int totalSec, int advMs, int scanMs,
                           NimBLEScanCallbacks* cb, bool active);
  // Shared raw-capture setup for raw()/rawPassive() (differ only in active-scan).
  void startRawScan(int seconds, bool active);

  // --- keying ---
  void addLight(uint8_t number, LightDevice& light);

  // --- persistence plumbing ---
  void loadKeyOrGenerate();
  void fireRegistered(LightDevice& l) { if (onRegistered_) onRegistered_(l); }

  // --- believed state (FR-05) + convergence (FR-16) ------------------------
  /// Persist a dirty believed-state (debounced — called from loop()).
  void saveStateIfDirty();
  /// P2.9: re-assert every light's last known state so a bulb that missed a
  /// command (unpowered) converges. Off stays off; skipped while scanning.
  void retransmitTick();

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
  std::function<void(uint8_t)>      onRenamed_;
  std::function<void(uint8_t)>      onForgotten_;

  // --- command queue (P3.9, BRIDGE_PLAN §8) --------------------------------
  // Bounded FIFO of light NUMBERS; latest-wins per light (re-enqueue moves it
  // to the tail). Guarded by cmdMux_ — enqueue() is called from MQTT/web/serial
  // contexts, drained from loop(). Full queue: drop oldest, count the overflow.
  uint8_t         cmdQueue_[CMD_QUEUE_SIZE];
  int             cmdHead_ = 0;   // index of the oldest pending entry
  int             cmdCount_ = 0;  // pending entries (0..CMD_QUEUE_SIZE)
  SemaphoreHandle_t cmdMux_ = nullptr;
  unsigned long   queueOverflowCount_ = 0;
  // Pacer state (loop-context only): timestamp of the last TX slot sent.
  uint32_t        lastQueueSlotMs_ = 0;
  // Send ONE broadcast for the oldest queued light (loop-context; the caller
  // owns the T_TX_MS gap between slots).
  void sendOneQueuedLight();
  // Drain as many queue slots as T_TX_MS allows this loop() pass.
  void drainCommandQueue();

  bool scanRequested_ = false;
  int scanSeconds_ = SCAN_DURATION;
  bool isScanning_ = false;
  bool started_ = false;

  // Believed state (FR-05): dirty flag + debounce so rapid slider drags don't
  // hammer NVS. Re-transmit (FR-16/P2.9): cadence + last-fire timestamp.
  bool      stateDirty_ = false;
  uint32_t  lastStateSaveMs_ = 0;
  uint32_t  lastRetransmitMs_ = 0;
  uint32_t  retransmitIntervalMs_ = 10000;   // default 10 s
};
