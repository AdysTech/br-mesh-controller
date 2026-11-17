// ============================================================================
//  BRMesh Controller
//
//  An ESP32-S3 that bridges BRMesh / BRGD BLE "mesh" lights into Home Assistant
//  over MQTT (home-assistant-integration), OR runs as a standalone BLE test
//  harness (ENABLE_WIFI=0) with a serial console.
//
//  Core (always active):
//   * The ESP is the KEY ORIGINATOR: it generates its own 4-byte mesh key and
//     writes it onto factory-keyed bulbs. (No adb / key-extraction.)
//   * Discovery: broadcast a WAKE, catch each factory-key bulb's 18-byte
//     manufacturer advertisement, then write (SET_KEY) our own key + a light
//     number, and confirm when it re-advertises under our key.
//   * PERSISTENT: key + registered-light list live in NVS; on boot we restore
//     existing lights (no re-keying) and only key NEW factory-key bulbs.
//   * Control frames (on/off, brightness, colour temp, RGB) are broadcast.
//
//  ENABLE_WIFI=0  -> serial console drives control frames directly (no HA).
//  ENABLE_WIFI=1  -> each light is exposed as its own HA entity via MQTT.
// ============================================================================

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEAdvertising.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <Preferences.h>
#include <string>
#include <vector>

#if ENABLE_WIFI
  #include <WiFi.h>
  #include <ArduinoHA.h>
  #include <WebServer.h>
#endif

#include "config.h"
#include "protocol.h"

// ============================================================================
//  Globals
// ============================================================================
struct LightDevice {
  std::string address;              // BLE MAC address (identity + logging)
  std::vector<uint8_t> mfgData;     // raw 18-byte manufacturer data (used to key)
  uint8_t type[2] = { 0, 0 };       // light-type code (e.g. A8A1 = RGBW)
  bool isRegistered = false;        // true once it holds our key
  std::string id;                   // HA unique id
  std::string name;                 // HA friendly name
  uint8_t number = 0;               // light number we assigned it
#if ENABLE_WIFI
  HALight* light = nullptr;
#endif
};

struct LightType { const char* name; uint8_t code[2]; };
const std::vector<LightType> lightTypes = {
  { "Smart", { 0x39, 0xae } },  // 44601 - on/off only
  { "RGBW",  { 0xa1, 0xa8 } },  // 43169 - brightness + CT + RGB
  { "RGB",   { 0xa0, 0xa8 } },  // 43168 - brightness + RGB
  { "Color", { 0x2a, 0xa8 } },  // observed 2026-11-16 from live capture - RGB-capable
};

uint8_t my_key[4] = { 0, 0, 0, 0 };
std::vector<LightDevice> myLights;
NimBLEAdvertising* pAdvertising = nullptr;
NimBLEScan* pBLEScan = nullptr;
Preferences prefs;

bool scanRequested = false;
bool isScanning = false;
int g_target = -1;                 // serial-console target light number (-1 = first)
int g_scanSeconds = SCAN_DURATION; // duration for the next user-requested discovery

#if ENABLE_WIFI
WiFiClient client;
HADevice device;
HAMqtt* mqtt = nullptr;
WebServer server;
#endif

// ============================================================================
//  Forward declarations
// ============================================================================
void single_control(const uint8_t key[4], const uint8_t data[8]);
uint8_t firstLightNumber();
void cmd_state(uint8_t n, bool on);
void cmd_brightness(uint8_t n, uint8_t b);
void cmd_ct(uint8_t n, uint16_t t);
void cmd_rgb(uint8_t n, uint8_t r, uint8_t g, uint8_t b);
void createLightEntity(LightDevice &l);
void scan();
void rawScan(int seconds = 5);
void rawScanPassive(int seconds = 5);
void wakeFor(int seconds);
void wakePhone(int seconds);
void addLight(uint8_t number, LightDevice &l);
void addLights();
void loadConfig();
void saveLights();
void loadLights();
void readdLight(const std::string &mac, uint8_t number);
void printHelp();
void handleCommand(char* line);
void serialConsole();
#if ENABLE_WIFI
void handleStatus();
void handleScan();
#endif

// ============================================================================
//  Logging helper
// ============================================================================
void dumpHex(const char* label, const uint8_t* data, int len) {
  Serial.print(label);
  for (int i = 0; i < len; i++) Serial.printf("%02X ", data[i]);
  Serial.println("");
}

LightDevice* getLight(const std::string &id) {
  for (auto &l : myLights) if (l.id == id) return &l;
  return nullptr;
}

uint8_t firstLightNumber() {
  for (auto &l : myLights) if (l.isRegistered) return l.number;
  return 1;
}

// ============================================================================
//  Control frames (shared by HA callbacks and the serial console)
// ============================================================================
// Build + arm one FastCon manufacturer advertisement on pAdvertising. `sd` is
// the raw AD structure from protocol::to_adv_data(): [len] 0xFF 0xF0 0xFF <payload>.
// NimBLEAdvertisementData::addData() appends raw bytes verbatim (identical to the
// old Bluedroid addData), so the on-air bytes match the proven build exactly.
void armMfgAdvertise(const std::string &sd) {
  NimBLEAdvertisementData ad;
  ad.setFlags(0x04);   // BLE_HS_ADV_F_BREDR_UNSUP
  ad.addData((const uint8_t *)sd.data(), sd.size());
  pAdvertising->setAdvertisementData(ad);
  pAdvertising->setMinInterval(50);
  pAdvertising->setMaxInterval(50);
}

void single_control(const uint8_t key[4], const uint8_t data[8]) {
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::CONTROL,
                                 std::vector<uint8_t>(data, data + 8), key, true);
  std::string sd = protocol::to_adv_data(rf);

  armMfgAdvertise(sd);
  pAdvertising->start();
  delay(250);
  pAdvertising->stop();
}

void cmd_state(uint8_t n, bool on) {
  uint8_t d[] = { 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  if (on) d[2] = 0x80;
  single_control(my_key, d);
}

void cmd_brightness(uint8_t n, uint8_t b) {
  uint8_t d[] = { 0x22, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  d[2] = b & 127;
  single_control(my_key, d);
}

void cmd_ct(uint8_t n, uint16_t t) {
  uint8_t d[] = { 0x72, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  d[2] = 0x80;                    // carry a non-zero brightness
  d[6] = t & 127;
  d[7] = (t >> 8) & 127;
  single_control(my_key, d);
}

void cmd_rgb(uint8_t n, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t d[] = { 0x72, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  d[2] = 0x80;                    // carry a non-zero brightness
  d[3] = b;                       // blue
  d[4] = r;                       // red
  d[5] = g;                       // green
  single_control(my_key, d);
}

// ============================================================================
//  HA light entity (only when building for the MQTT bridge)
// ============================================================================
#if ENABLE_WIFI
void onStateCommand(bool state, HALight* sender) {
  LightDevice* l = getLight(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA state %s -> #%d\n", state ? "ON" : "OFF", l->number);
  cmd_state(l->number, state);
  sender->setState(state);
}
void onBrightnessCommand(uint8_t brightness, HALight* sender) {
  LightDevice* l = getLight(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA brightness %d -> #%d\n", brightness, l->number);
  cmd_brightness(l->number, brightness);
  sender->setBrightness(brightness);
}
void onColorTemperatureCommand(uint16_t temperature, HALight* sender) {
  LightDevice* l = getLight(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA colorTemp %d -> #%d\n", temperature, l->number);
  cmd_ct(l->number, temperature);
  sender->setColorTemperature(temperature);
}
void onRGBColorCommand(HALight::RGBColor color, HALight* sender) {
  LightDevice* l = getLight(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA RGB %d,%d,%d -> #%d\n", color.red, color.green, color.blue, l->number);
  cmd_rgb(l->number, color.red, color.green, color.blue);
  sender->setRGBColor(color);
}
#endif

void createLightEntity(LightDevice &l) {
#if ENABLE_WIFI
  const bool rgbw = (l.type[0] == 0xa1 && l.type[1] == 0xa8) || (l.type[0] == 0x2a && l.type[1] == 0xa8);
  const bool rgb  = (l.type[0] == 0xa0 && l.type[1] == 0xa8);

  HALight* light = nullptr;
  if (rgbw) {
    light = new HALight(l.id.c_str(),
                        HALight::BrightnessFeature | HALight::ColorTemperatureFeature | HALight::RGBFeature);
    light->onStateCommand(onStateCommand);
    light->onBrightnessCommand(onBrightnessCommand);
    light->onColorTemperatureCommand(onColorTemperatureCommand);
    light->onRGBColorCommand(onRGBColorCommand);
    light->setBrightnessScale(127);
    light->setBrightness(127);
  } else if (rgb) {
    light = new HALight(l.id.c_str(), HALight::BrightnessFeature | HALight::RGBFeature);
    light->onStateCommand(onStateCommand);
    light->onBrightnessCommand(onBrightnessCommand);
    light->onRGBColorCommand(onRGBColorCommand);
    light->setBrightnessScale(127);
    light->setBrightness(127);
  } else {
    light = new HALight(l.id.c_str());   // Smart - on/off only
    light->onStateCommand(onStateCommand);
  }
  light->setName(l.name.c_str());
  l.light = light;
  Serial.printf("  created HA light '%s' (id=%s)\n", l.name.c_str(), l.id.c_str());
#else
  (void)l;   // standalone mode: no HA entity needed
#endif
}

// ============================================================================
//  Discovery callbacks
// ============================================================================
class AddDeviceCallback : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice *dev) {
    std::string address = dev->getAddress().toString();
    std::string mfg = dev->getManufacturerData();
    if (mfg.size() != 18) return;

    std::string type = mfg.substr(12, 2);
    std::string key  = mfg.substr(14, 4);

    bool knownType = false;
    for (auto &t : lightTypes) {
      if (type[0] == t.code[0] && type[1] == t.code[1]) { knownType = true; break; }
    }
    bool defaultKey = (key[0] == protocol::DEFAULT_KEY[0] && key[1] == protocol::DEFAULT_KEY[1] &&
                       key[2] == protocol::DEFAULT_KEY[2] && key[3] == protocol::DEFAULT_KEY[3]);

    if (defaultKey) {   // factory key present == BRGD bulb needing keying.
      bool alreadyKnown = false;                        // Type is only a hint (control
      for (auto &l : myLights) if (l.address == address) { alreadyKnown = true; break; }   // features), never a gate -
      if (!alreadyKnown) {                              // unknown types must still be keyed.
        LightDevice l;
        l.address = address;
        l.mfgData.assign(mfg.begin(), mfg.end());
        l.type[0] = (uint8_t)type[0];
        l.type[1] = (uint8_t)type[1];
        myLights.push_back(l);
        Serial.printf("  * factory light found: %s type=%02X %02X%s (needs keying)\n",
                      address.c_str(), l.type[0], l.type[1], knownType ? "" : " [unknown type]");
      }
    }
  }
};

class AddLightCallback : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice *dev) {
    std::string address = dev->getAddress().toString();
    std::string mfg = dev->getManufacturerData();
    if (mfg.size() != 18) return;

    for (auto &l : myLights) {
      if (l.address != address) continue;

      std::string key = mfg.substr(14, 4);
      bool stillDefault = (key[0] == protocol::DEFAULT_KEY[0] && key[1] == protocol::DEFAULT_KEY[1] &&
                           key[2] == protocol::DEFAULT_KEY[2] && key[3] == protocol::DEFAULT_KEY[3]);
      if (stillDefault) continue;         // key not accepted yet
      if (l.isRegistered) continue;       // already done

      // Read the number the bulb reports back, decrypting with our key.
      std::string md = mfg.substr(2);     // 16 bytes
      uint8_t clean[12];
      for (int j = 0; j < 12; j++) clean[j] = my_key[j & 3] ^ (uint8_t)md[4 + j];
      l.number = clean[1];

      l.id = l.address.substr(3, 2) + l.address.substr(0, 2);
      l.name = "Light_" + l.id;
      l.isRegistered = true;
      createLightEntity(l);
      saveLights();
      Serial.printf("  + keyed light #%d (%s) under our key\n", l.number, l.id.c_str());
      break;
    }
  }
};

// ============================================================================
//  Debug: unfiltered scan - dump EVERY advertiser's manufacturer data
//  (lets us see a bulb's real advertising key without assuming factory key)
// ============================================================================
class RawScanCallback : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice *dev) {
    std::string addr = dev->getAddress().toString();
    std::string mfgS = dev->getManufacturerData();
    if (mfgS.length() > 0) {
      Serial.printf("RAW %s mfg(%d): ", addr.c_str(), (int)mfgS.length());
      for (size_t i = 0; i < mfgS.length(); i++) Serial.printf("%02X ", (uint8_t)mfgS[i]);
      Serial.println();
    } else {
      Serial.printf("RAW %s (no mfg data)\n", addr.c_str());
    }
  }
};

// NimBLE's start() is NON-BLOCKING (unlike Bluedroid's) and takes duration in
// MILLISECONDS. The discovery / SET_KEY recipe must hold WAKE / SET_KEY advertising
// up for the whole scan window, so those paths poll until the scan finishes; the
// long raw dumps run in the background so 'scanstop' can cut them short.
static void waitScanDone(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (pBLEScan && pBLEScan->isScanning()) {
    if ((uint32_t)(millis() - t0) > timeoutMs) break;   // hard cap; scan self-terminates too
    delay(50);
  }
}

// NimBLE's ESP32 controller DROPS the advertiser the moment a scan starts, so the
// Bluedroid-style "advertise WAKE + scan concurrently" isn't available on this stack.
// Instead we time-slice: put the payload (WAKE / SET_KEY) on the air for advMs, then
// listen for scanMs, and repeat. The WAKE keeps re-appearing between scan windows, so
// the woken bulb's identity frames land during one of our scan windows. (Same
// start/delay/stop rhythm that wakePhone() already relies on to fire reliably.)
void slicedAdvertiseScan(int totalSec, int advMs, int scanMs,
                         NimBLEScanCallbacks *cb, bool active) {
  uint32_t target = (uint32_t)totalSec * 1000;
  uint32_t t0 = millis();
  int cycles = 0;
  while ((uint32_t)(millis() - t0) < target) {
    // 1) payload on the air (WAKE / SET_KEY)
    if (!pAdvertising->isAdvertising()) pAdvertising->start();
    delay(advMs);
    pAdvertising->stop();
    delay(20);                     // let the controller release the radio before scanning
    // 2) listen (advertiser is down, so the scan owns the radio)
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(cb, true);   // wantDuplicates = true (see rawScan notes)
    pBLEScan->setDuplicateFilter(0);
    pBLEScan->setInterval(300);
    pBLEScan->setWindow(300);
    pBLEScan->setActiveScan(active);
    pBLEScan->start(scanMs, false, false);
    delay(50);                     // let the scan actually begin (host task is async)
    waitScanDone((uint32_t)scanMs + 200);
    cycles++;
  }
  pAdvertising->stop();
  Serial.printf("  [sliced: %d WAKE/scan cycles]\n", cycles);
}

void rawScan(int seconds) {
  pAdvertising->stop();
  Serial.printf("RAW scan STARTING (~%ds; send 'scanstop' to end early): every advertiser, unfiltered\n", seconds);
  pBLEScan = NimBLEDevice::getScan();
  // wantDuplicates = true: fire onResult for EVERY frame (no per-address dedup),
  // so we see all of a bulb's advertisement types across its short boot window.
  pBLEScan->setScanCallbacks(new RawScanCallback(), true);
  pBLEScan->setDuplicateFilter(0);        // explicit: also drop the controller-level dup filter
  pBLEScan->setInterval(300);
  pBLEScan->setWindow(300);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(seconds * 1000, false, false);   // duration is MILLISECONDS; non-blocking
  Serial.println("  (scan runs in the background; ends on its own or with 'scanstop')");
}

// Passive raw scan: PURE listening (no SCAN_REQ) at 100% duty, wantDuplicates.
// Best for catching a phone's ONE-SHOT FastCon broadcast (e.g. the app's
// "Forget/Reset"): active scan's SCAN_REQ transmissions steal RX time, so passive
// listens the whole of each channel slot and drops fewer one-shots.
void rawScanPassive(int seconds) {
  pAdvertising->stop();
  Serial.printf("RAW+PASSIVE (%ds): pure listening, every frame, no SCAN_REQ\n", seconds);
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new RawScanCallback(), true);
  pBLEScan->setDuplicateFilter(0);
  pBLEScan->setInterval(300);
  pBLEScan->setWindow(300);
  pBLEScan->setActiveScan(false);
  pBLEScan->start(seconds * 1000, false, false);   // duration is MILLISECONDS; non-blocking
  Serial.println("  (scan runs in the background; ends on its own or with 'scanstop')");
}

// WAKE broadcast + duplicate raw scan: tests whether a WAKE triggers the
// factory-identity frame, capturing EVERY raw frame (no dedup) so we see
// both spontaneous and WAKE-triggered advertisements.
void rawWake(int seconds) {
  uint8_t data[] = { 0, 0, 0, 0, 0, 0 };
  uint8_t key[]  = { 0, 0, 0, 0 };
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::WAKE, std::vector<uint8_t>(data, data + 6), key, false);
  std::string sd = protocol::to_adv_data(rf);

  armMfgAdvertise(sd);

  Serial.printf("RAW+WAKE (%ds): time-sliced WAKE + raw scan (every frame)\n", seconds);
  slicedAdvertiseScan(seconds, 300, 150, new RawScanCallback(), true);
  Serial.println("RAW+WAKE done");
}

// Broadcast our own WAKE for N seconds (NO scan) - used to confirm our WAKE
// actually wakes the bulbs: run 'wake 3' then 'raw 5' and look for factory frames.
void wakeFor(int seconds) {
  uint8_t data[] = { 0, 0, 0, 0, 0, 0 };
  uint8_t key[] = { 0, 0, 0, 0 };
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::WAKE, std::vector<uint8_t>(data, data + 6), key, false);
  std::string sd = protocol::to_adv_data(rf);
  armMfgAdvertise(sd);
  Serial.printf("Broadcasting WAKE for %ds (no scan)...\n", seconds);
  pAdvertising->start();
  delay(seconds * 1000);
  pAdvertising->stop();
  Serial.println("WAKE done");
}

// Replay the PHONE's exact WAKE advertisement (captured live from the BRMesh app)
// verbatim as ours. The bulbs reliably respond to this 24-byte payload but not to
// our reverse-engineered 16-byte FastCon body, so for the POC we just replay it.
// The byte at offset 7 is a rolling counter (observed 0x80..0xDF); we advance it a
// little each round so the bulbs see a fresh WAKE rather than one deduplicated frame.
void wakePhone(int seconds) {
  // F0 FF | 6D B6 43 68 93 1D 0D <ctr> 5C FF C6 3E 5F 0F 65 CA 0A 67 AA 63 13 0B 79 78
  const uint8_t base[] = { 0x6D, 0xB6, 0x43, 0x68, 0x93, 0x1D, 0x0D, 0x87,
                           0x5C, 0xFF, 0xC6, 0x3E, 0x5F, 0x0F, 0x65, 0xCA,
                           0x0A, 0x67, 0xAA, 0x63, 0x13, 0x0B, 0x79, 0x78 };
  const int roundMs = 400;
  uint8_t ctr = 0x80;
  int rounds = (seconds * 1000) / roundMs;
  for (int r = 0; r < rounds; r++) {
    uint8_t payload[sizeof(base)];
    memcpy(payload, base, sizeof(base));
    payload[7] = ctr++;  // advance the rolling counter each round
    std::string sd;
    sd += (char)(sizeof(payload) + 3);  // AD length
    sd += (char)0xFF;                   // mfg specific
    sd += (char)0xF0;
    sd += (char)0xFF;                   // company 0xFFF0
    for (auto b : payload) sd += (char)b;
    armMfgAdvertise(sd);
    pAdvertising->start();
    delay(roundMs);
    pAdvertising->stop();
  }
  Serial.println("PHONE WAKE done");
}

// ============================================================================
//  Discovery flow (wake -> detect factory bulbs -> key each new one)
// ============================================================================
void scan(int seconds) {
  Serial.printf("Discovery: WAKE + concurrent scan (%ds)\n", seconds);
  // WAKE: the reverse-engineered type-0 FastCon body (6 zero bytes, all-zero key).
  // This is byte-identical to the working BRmesh-esp32 reference's do_generate_command(0,...),
  // which reliably wakes the bulbs — the bulb answers by advertising its factory identity.
  uint8_t data[] = { 0, 0, 0, 0, 0, 0 };
  uint8_t key[] = { 0, 0, 0, 0 };
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::WAKE, std::vector<uint8_t>(data, data + 6), key, false);
  std::string sd = protocol::to_adv_data(rf);

  armMfgAdvertise(sd);

  // Bluedroid kept WAKE up THROUGH the scan; NimBLE can't (scan drops the advertiser),
  // so we time-slice WAKE + scan instead. wantDuplicates=true (set inside the helper)
  // matters here: the bulb interleaves bare beacons with its factory-identity frame,
  // and without it the per-address dedup locks onto the first (bare) beacon.
  slicedAdvertiseScan(seconds, 400, 200, new AddDeviceCallback(), true);
  Serial.println("Discovery complete");
}

void scan() { scan(SCAN_DURATION); }

void addLight(uint8_t number, LightDevice &light) {
  Serial.printf("Setting key on light #%d, MAC: ", number);
  Serial.println(light.address.c_str());
  dumpHex("new key: ", my_key, 4);

  uint8_t data[12];
  for (int i = 0; i < 6; i++) data[i] = light.mfgData[6 + i];  // mac (from mfg data)
  data[6] = number;                                            // requested light id
  data[7] = 0x01;                                              // group id
  data[8] = my_key[0];
  data[9] = my_key[1];
  data[10] = my_key[2];
  data[11] = my_key[3];

  // Sent under the factory key (the bulb still has it); payload carries our new key.
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::SET_KEY, std::vector<uint8_t>(data, data + 12),
                                 protocol::DEFAULT_KEY, false);
  std::string sd = protocol::to_adv_data(rf);

  armMfgAdvertise(sd);
  // SET_KEY on the air for ~2 s, time-sliced with the confirmation scan so the
  // keyed-identity frame lands even though it's interleaved with bare beacons.
  slicedAdvertiseScan(2, 300, 150, new AddLightCallback(), true);
}

void addLights() {
  scan(g_scanSeconds);
  delay(1000);   // bulbs need a brief pause after the scan
  int maxNum = 0;
  for (auto &l : myLights) if (l.number > maxNum) maxNum = l.number;
  for (auto &l : myLights) {
    if (l.isRegistered) continue;    // already known - don't re-key
    maxNum++;
    addLight(maxNum, l);
  }
}

// End a running (background) raw scan on demand. Only meaningful while a scan is
// active; harmless (no-op) otherwise.
void scanStop() {
  if (pBLEScan && pBLEScan->isScanning()) { pBLEScan->stop(); Serial.println("scanstop: scan stopped"); }
  else Serial.println("scanstop: no active scan");
  pAdvertising->stop();
}

// Parse "aa:bb:cc:dd:ee:ff" into 6 bytes in standard order.
static bool parseMac(const std::string &s, uint8_t out[6]) {
  if (s.size() != 17) return false;   // "aa:bb:cc:dd:ee:ff"
  for (int i = 0; i < 6; i++) {
    if (i && s[3 * i - 1] != ':') return false;   // colon at 2,5,8,11,14
    out[i] = (uint8_t)strtoul(s.substr(3 * i, 2).c_str(), nullptr, 16);   // pair at 3*i
  }
  return true;
}

// ============================================================================
//  Persistence (NVS) - key + registered light list
// ============================================================================
void loadConfig() {
  prefs.begin("brmesh", false);
  if (prefs.getBytes("key", my_key, 4) == 4) {
    Serial.println("Loaded existing mesh key from NVS");
  } else {
    uint32_t rk = esp_random();
    my_key[0] = rk & 0xFF;
    my_key[1] = (rk >> 8) & 0xFF;
    my_key[2] = (rk >> 16) & 0xFF;
    my_key[3] = (rk >> 24) & 0xFF;
    prefs.begin("brmesh", false);
    prefs.putBytes("key", my_key, 4);
    Serial.println("Generated a NEW mesh key and saved it");
  }
  prefs.end();
}

void saveLights() {
  int n = 0;
  for (auto &l : myLights) if (l.isRegistered) n++;
  prefs.begin("brmesh", false);
  prefs.putInt("count", n);
  int i = 0;
  for (auto &l : myLights) {
    if (!l.isRegistered) continue;
    String idk = "id" + String(i);
    String nmk = "nm" + String(i);
    String t0k = "t0" + String(i);
    String t1k = "t1" + String(i);
    String nuk = "num" + String(i);
    prefs.putString(idk.c_str(), l.id.c_str());
    prefs.putString(nmk.c_str(), l.name.c_str());
    prefs.putInt(t0k.c_str(), l.type[0]);
    prefs.putInt(t1k.c_str(), l.type[1]);
    prefs.putInt(nuk.c_str(), l.number);
    i++;
  }
  prefs.end();
  Serial.printf("Saved %d registered lights to NVS\n", n);
}

void loadLights() {
  prefs.begin("brmesh", false);
  int n = prefs.getInt("count", 0);
  for (int i = 0; i < n; i++) {
    String idk = "id" + String(i);
    String nmk = "nm" + String(i);
    String t0k = "t0" + String(i);
    String t1k = "t1" + String(i);
    String nuk = "num" + String(i);
    LightDevice l;
    l.id = prefs.getString(idk.c_str(), "").c_str();
    l.name = prefs.getString(nmk.c_str(), "Light").c_str();
    l.type[0] = (uint8_t)prefs.getInt(t0k.c_str(), 0);
    l.type[1] = (uint8_t)prefs.getInt(t1k.c_str(), 0);
    l.number = (uint8_t)prefs.getInt(nuk.c_str(), 0);
    l.isRegistered = true;
    myLights.push_back(l);
    createLightEntity(myLights.back());
  }
  prefs.end();
  Serial.printf("Restored %d lights from NVS\n", n);
}

// Re-register an already-keyed bulb into the light list + NVS, WITHOUT any
// air activity or SET_KEY. For when the NVS list records are lost but the key
// (and the bulb's key) survived — proven by a control command still working.
// id/name use the exact scheme AddLightCallback assigns (70:e7:.. -> "e770").
void readdLight(const std::string &mac, uint8_t number) {
  for (auto &l : myLights) {
    if (l.address == mac) {
      Serial.printf("already in the list as #%d\n", l.number);
      return;
    }
  }
  LightDevice l;
  l.address = mac;
  l.type[0] = 0x2A;    // Color (2A A8) — the type of all three of our bulbs
  l.type[1] = 0xA8;
  l.number = number;
  l.id = mac.substr(3, 2) + mac.substr(0, 2);
  l.name = "Light_" + l.id;
  l.isRegistered = true;
  myLights.push_back(l);
  createLightEntity(l);
  saveLights();
  Serial.printf("  + re-added light #%d (%s, %s) under current key\n",
                number, l.id.c_str(), mac.c_str());
}

// ============================================================================
//  Serial console (works in both modes)
// ============================================================================
void printHelp() {
  Serial.println("---- BRMesh serial console ----");
  Serial.println("  help                 this help");
  Serial.println("  list                 known lights");
  Serial.println("  key                  show current mesh key (NVS state)");
  Serial.println("  sel <n>              set target light number");
  Serial.println("  on [n]  off [n]      state (n optional)");
  Serial.println("  br <0-127> [n]       brightness");
  Serial.println("  ct <0-32767> [n]     colour temperature");
  Serial.println("  rgb r g b [n]        colour (0-255 each)");
  Serial.println("  readd <mac> [num]   re-register a keyed light (list recovery, no air traffic)");
  Serial.println("  forget <mac|n>      remove a light from our list + NVS (no air traffic; bulb's key untouched)");
  Serial.println("  scan [sec=5]         re-run discovery / key new bulbs");
  Serial.println("  bootcatch [sec=12]   WAKE+scan; use while power-cycling a bulb");
  Serial.println("  raw [sec=5]          unfiltered BLE dump (debug)");
  Serial.println("  rawp [sec=5]         PASSIVE raw dump — best for a phone one-shot (Forget/Reset)");
  Serial.println("  rawwake [sec=5]      WAKE broadcast + every frame (debug)");
  Serial.println("  wake [sec=3]         broadcast our WAKE only (no scan)");
  Serial.println("  wakephone [sec=3]    replay the phone's WAKE bytes (no scan)");
  Serial.println("  scanstop             end a running raw scan on demand");
  Serial.println("  clear                remove all known lights (list + NVS; keeps key)");
  Serial.println("  --------------------------");
}

void handleCommand(char* line) {
  Serial.print("> ");
  Serial.println(line);

  char* argv[8];
  int argc = 0;
  char* tok = strtok(line, " \t");
  while (tok && argc < 8) { argv[argc++] = tok; tok = strtok(nullptr, " \t"); }
  if (argc == 0) return;

  uint8_t n = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  uint8_t target = (g_target >= 1) ? (uint8_t)g_target : firstLightNumber();

  if (strcmp(argv[0], "help") == 0) printHelp();

  else if (strcmp(argv[0], "list") == 0) {
    if (myLights.empty()) Serial.println("  (no lights)");
    for (auto &l : myLights)
      Serial.printf("  #%d %s [%s] %s\n", l.number, l.name.c_str(), l.id.c_str(),
                    l.isRegistered ? "registered" : "factory-key");
  }

  else if (strcmp(argv[0], "key") == 0) {
    Serial.printf("current mesh key: %02X %02X %02X %02X\n",
                  my_key[0], my_key[1], my_key[2], my_key[3]);
  }

  else if (strcmp(argv[0], "sel") == 0) {
    if (argc >= 2) { g_target = atoi(argv[1]); Serial.printf("target set to #%d\n", g_target); }
  }

  else if (strcmp(argv[0], "on") == 0)   { if (argc >= 2) target = n; cmd_state(target, true);  Serial.println("sent ON"); }
  else if (strcmp(argv[0], "off") == 0)  { if (argc >= 2) target = n; cmd_state(target, false); Serial.println("sent OFF"); }

  else if (strcmp(argv[0], "br") == 0) {
    if (argc < 2) { Serial.println("usage: br <0-127> [n]"); return; }
    if (argc >= 3) target = (uint8_t)atoi(argv[2]);
    cmd_brightness(target, (uint8_t)atoi(argv[1]));
    Serial.printf("sent brightness=%d to #%d\n", atoi(argv[1]), target);
  }

  else if (strcmp(argv[0], "ct") == 0) {
    if (argc < 2) { Serial.println("usage: ct <value> [n]"); return; }
    if (argc >= 3) target = (uint8_t)atoi(argv[2]);
    cmd_ct(target, (uint16_t)atoi(argv[1]));
    Serial.printf("sent ct=%d to #%d\n", atoi(argv[1]), target);
  }

  else if (strcmp(argv[0], "rgb") == 0) {
    if (argc < 4) { Serial.println("usage: rgb r g b [n]"); return; }
    if (argc >= 5) target = (uint8_t)atoi(argv[4]);
    cmd_rgb(target, (uint8_t)atoi(argv[1]), (uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]));
    Serial.printf("sent rgb=%d,%d,%d to #%d\n", atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), target);
  }

  else if (strcmp(argv[0], "readd") == 0) {
    if (argc < 2) { Serial.println("usage: readd <mac> [num]"); return; }
    uint8_t next = 0;
    for (auto &l : myLights) if (l.number > next) next = l.number;
    uint8_t num = (argc >= 3) ? (uint8_t)atoi(argv[2]) : (uint8_t)(next + 1);
    readdLight(std::string(argv[1]), num);
  }

  else if (strcmp(argv[0], "forget") == 0) {
    if (argc < 2) { Serial.println("usage: forget <mac|n>   (remove a light from our list + NVS; no air traffic)"); return; }
    std::string arg = argv[1];
    bool byMac = (arg.find(':') != std::string::npos);
    bool found = false;
    for (auto it = myLights.begin(); it != myLights.end(); ++it) {
      bool match = byMac ? (it->address == arg)
                         : (it->number == (uint8_t)atoi(arg.c_str()));
      if (match) {
        Serial.printf("forgetting #%d %s [%s]\n", it->number, it->name.c_str(), it->id.c_str());
        myLights.erase(it);
        found = true;
        break;
      }
    }
    if (!found) { Serial.printf("no light matching '%s' in the list (use 'list')\n", arg.c_str()); return; }
    saveLights();
    // NOTE(bridge, deferred): also drop the matching HA entity here once ENABLE_WIFI is on.
    Serial.println("forgotten from the list + NVS. (The bulb's key is untouched — nothing was sent over the air.)");
  }

  else if (strcmp(argv[0], "scan") == 0) {
    if (argc >= 2) g_scanSeconds = atoi(argv[1]);
    scanRequested = true;
  }
  else if (strcmp(argv[0], "bootcatch") == 0) scan((argc >= 2) ? atoi(argv[1]) : 12);
  else if (strcmp(argv[0], "raw") == 0) rawScan((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "rawp") == 0) rawScanPassive((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "rawwake") == 0) rawWake((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "scanstop") == 0) scanStop();

  else if (strcmp(argv[0], "clear") == 0) {
    // Wipe the known-lights list (RAM + NVS); keeps our mesh key. Needed before
    // re-keying a bulb whose stale NVS entry would make addLights() skip it.
    prefs.begin("brmesh", false);
    int n = prefs.getInt("count", 0);
    for (int i = 0; i < n; i++) {
      prefs.remove(("id" + String(i)).c_str());
      prefs.remove(("nm" + String(i)).c_str());
      prefs.remove(("t0" + String(i)).c_str());
      prefs.remove(("t1" + String(i)).c_str());
      prefs.remove(("num" + String(i)).c_str());
    }
    prefs.putInt("count", 0);
    prefs.end();
    myLights.clear();
    g_target = 0;
    Serial.printf("Cleared %d light(s) from list + NVS (mesh key kept)\n", n);
  }
  else if (strcmp(argv[0], "wake") == 0) wakeFor((argc >= 2) ? atoi(argv[1]) : 3);
  else if (strcmp(argv[0], "wakephone") == 0) wakePhone((argc >= 2) ? atoi(argv[1]) : 3);

  else Serial.printf("unknown command: %s (try 'help')\n", argv[0]);
}

void serialConsole() {
  static char buf[128];
  static int len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = 0;
        handleCommand(buf);
        len = 0;
      }
    } else if (len < (int)sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

// ============================================================================
//  HTTP (HA-bridge mode only)
// ============================================================================
#if ENABLE_WIFI
void handleStatus() {
  String s = "BRMesh Controller\n\n";
  s += "Registered lights:\n";
  if (myLights.empty()) s += "  (none)\n";
  for (auto &l : myLights) {
    if (!l.isRegistered) continue;
    s += String("  #") + String(l.number) + " " + String(l.name.c_str());
    s += " [" + String(l.id.c_str()) + "]\n";
  }
  s += "\nGET /scan to re-run discovery\n";
  server.send(200, "text/plain", s);
}
void handleScan() {
  if (isScanning) { server.send(200, "text/plain", "already scanning"); return; }
  scanRequested = true;
  server.send(200, "text/plain", "scan requested");
}
#endif

// ============================================================================
//  Setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\n=== BRMesh Controller ===");
  Serial.printf("ENABLE_WIFI=%d\n", ENABLE_WIFI);

  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);   // on while setting up / discovering
  }

  // 1) Key: restore, or generate + persist.
  loadConfig();
  dumpHex("Mesh key: ", my_key, 4);

#if ENABLE_WIFI
  byte mac[6];
  WiFi.macAddress(mac);
  device.setUniqueId(mac, sizeof(mac));
  device.setName("BRMesh");
  device.setManufacturer("BRMesh");
  device.setModel("BRMesh");
  mqtt = new HAMqtt(client, device);
#endif

  // 2) BLE (NimBLE host) - advertising + scanning.
  NimBLEDevice::init("BRMesh Controller");
  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->start();

#if ENABLE_WIFI
  // 3) WiFi.
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.println("Connecting to WiFi..."); }
  Serial.print("WiFi IP: ");
  Serial.println(WiFi.localIP());
#endif

  // 4) Restore previously-registered lights (no re-keying).
  loadLights();

  // 5) Discover NEW factory-key bulbs and key them under our key.
  addLights();

  if (LED_PIN >= 0) digitalWrite(LED_PIN, LOW);

#if ENABLE_WIFI
  // 6) HTTP status / rescan.
  server.on("/", handleStatus);
  server.on("/scan", handleScan);
  server.begin();

  // 7) Start MQTT (publishes HA auto-discovery for every light).
  mqtt->begin(MQTT_BROKER_ADDR, MQTT_PORT);
#endif

  Serial.println("Ready.");
  if (!ENABLE_WIFI) {
    Serial.println("Test mode: use the serial console (type 'help').");
    Serial.println("To key the bulb, power-cycle it, then type 'scan'.");
  }
  printHelp();
}

void loop() {
  // On-demand rescan (serial 'scan' or HTTP /scan).
  if (scanRequested && !isScanning) {
    scanRequested = false;
    isScanning = true;
    Serial.println("Re-running discovery");
    addLights();
    isScanning = false;
    Serial.println("Discovery complete");
  }

  serialConsole();
#if ENABLE_WIFI
  mqtt->loop();
  server.handleClient();
#endif
}
