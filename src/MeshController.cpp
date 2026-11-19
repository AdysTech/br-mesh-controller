// ============================================================================
//  MeshController — implementation. See mesh.h for the class contract.
//
//  Everything here is protocol + radio. No WiFi / MQTT / HA / HTTP / console.
// ============================================================================
#include "MeshController.h"

// Light-type table: a *hint* for logging only. Type is never a gate — unknown
// types must still be keyed (control features depend on it).
namespace {
struct LightType { const char* name; uint8_t code[2]; };
const LightType lightTypes[] = {
  { "Smart", { 0x39, 0xae } },  // 44601 - on/off only
  { "RGBW",  { 0xa1, 0xa8 } },  // 43169 - brightness + CT + RGB
  { "RGB",   { 0xa0, 0xa8 } },  // 43168 - brightness + RGB
  { "Color", { 0x2a, 0xa8 } },  // observed 2026-11-16 from live capture
};

void dumpHex(const char* label, const uint8_t* data, int len) {
  Serial.print(label);
  for (int i = 0; i < len; i++) Serial.printf("%02X ", data[i]);
  Serial.println("");
}

bool isDefaultKey(const std::string& key) {
  return key[0] == protocol::DEFAULT_KEY[0] && key[1] == protocol::DEFAULT_KEY[1] &&
         key[2] == protocol::DEFAULT_KEY[2] && key[3] == protocol::DEFAULT_KEY[3];
}
}  // namespace

// ============================================================================
//  State access
// ============================================================================
int MeshController::lightCount() const {
  int n = 0;
  for (auto &l : myLights) if (l.isRegistered) n++;
  return n;
}

LightDevice* MeshController::findLightById(const std::string& id) {
  for (auto &l : myLights) if (l.id == id) return &l;
  return nullptr;
}

LightDevice* MeshController::findLightByMac(const std::string& mac) {
  for (auto &l : myLights) if (l.address == mac) return &l;
  return nullptr;
}

uint8_t MeshController::firstLightNumber() {
  for (auto &l : myLights) if (l.isRegistered) return l.number;
  return 1;
}

// ============================================================================
//  Control frames (broadcast) — shared by HA callbacks and the serial console
// ============================================================================
// Build + arm one FastCon manufacturer advertisement on pAdvertising. `sd` is
// the raw AD structure from protocol::to_adv_data(): [len] 0xFF 0xF0 0xFF <payload>.
// NimBLEAdvertisementData::addData() appends raw bytes verbatim, so the on-air
// bytes match the proven build exactly.
void MeshController::armMfgAdvertise(const std::string& sd) {
  NimBLEAdvertisementData ad;
  ad.setFlags(0x04);   // BLE_HS_ADV_F_BREDR_UNSUP
  ad.addData((const uint8_t *)sd.data(), sd.size());
  pAdvertising->setAdvertisementData(ad);
  pAdvertising->setMinInterval(50);
  pAdvertising->setMaxInterval(50);
}

void MeshController::singleControl(const uint8_t key[4], const uint8_t data[8]) {
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::CONTROL,
                                 std::vector<uint8_t>(data, data + 8), key, true);
  std::string sd = protocol::to_adv_data(rf);
  armMfgAdvertise(sd);
  pAdvertising->start();
  delay(250);
  pAdvertising->stop();
}

void MeshController::state(uint8_t n, bool on) {
  uint8_t d[] = { 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  if (on) d[2] = 0x80;
  singleControl(my_key, d);
}

void MeshController::brightness(uint8_t n, uint8_t b) {
  uint8_t d[] = { 0x22, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  d[2] = b & 127;
  singleControl(my_key, d);
}

void MeshController::colorTemp(uint8_t n, uint16_t t) {
  uint8_t d[] = { 0x72, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  d[2] = 0x80;                    // carry a non-zero brightness
  d[6] = t & 127;
  d[7] = (t >> 8) & 127;
  singleControl(my_key, d);
}

void MeshController::rgb(uint8_t n, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t d[] = { 0x72, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
  d[1] = n;
  d[2] = 0x80;                    // carry a non-zero brightness
  d[3] = b;                       // blue
  d[4] = r;                       // red
  d[5] = g;                       // green
  singleControl(my_key, d);
}

// ============================================================================
//  Scan plumbing
// ============================================================================
// NimBLE's start() is NON-BLOCKING and takes duration in MILLISECONDS. The
// discovery / SET_KEY recipe must hold WAKE / SET_KEY advertising up for the
// whole scan window, so those paths poll until the scan finishes; the long raw
// dumps run in the background so 'scanstop' can cut them short.
void MeshController::waitScanDone(uint32_t timeoutMs) {
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
void MeshController::slicedAdvertiseScan(int totalSec, int advMs, int scanMs,
                               NimBLEScanCallbacks* cb, bool active) {
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
    pBLEScan->setScanCallbacks(cb, true);   // wantDuplicates = true (see raw notes)
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

// ============================================================================
//  Scan callbacks
// ============================================================================
// Discovery: detect a factory-key bulb and add it to the list for keying.
void MeshController::AddDeviceCallback::onResult(const NimBLEAdvertisedDevice* dev) {
  std::string address = dev->getAddress().toString();
  std::string mfg = dev->getManufacturerData();
  if (mfg.size() != 18) return;

  std::string type = mfg.substr(12, 2);
  std::string key  = mfg.substr(14, 4);

  bool knownType = false;
  for (auto &t : lightTypes) {
    if (type[0] == t.code[0] && type[1] == t.code[1]) { knownType = true; break; }
  }

  if (isDefaultKey(key)) {   // factory key present == BRGD bulb needing keying.
    if (m_->findLightByMac(address)) return;  // type is a hint, never a gate —
    LightDevice l;                                // unknown types must still be keyed.
    l.address = address;
    l.mfgData.assign(mfg.begin(), mfg.end());
    l.type[0] = (uint8_t)type[0];
    l.type[1] = (uint8_t)type[1];
    m_->myLights.push_back(l);
    Serial.printf("  * factory light found: %s type=%02X %02X%s (needs keying)\n",
                  address.c_str(), l.type[0], l.type[1], knownType ? "" : " [unknown type]");
  }
}

// Keying: watch for a bulb we just SET_KEY to re-advertise under OUR key, then
// read the number it reports back (XOR-decrypted with our key) and register it.
void MeshController::AddLightCallback::onResult(const NimBLEAdvertisedDevice* dev) {
  std::string address = dev->getAddress().toString();
  std::string mfg = dev->getManufacturerData();
  if (mfg.size() != 18) return;

  for (auto &l : m_->myLights) {
    if (l.address != address) continue;

    std::string key = mfg.substr(14, 4);
    if (isDefaultKey(key)) continue;         // key not accepted yet
    if (l.isRegistered) continue;            // already done

    // Read the number the bulb reports back, decrypting with our key.
    std::string md = mfg.substr(2);          // 16 bytes
    uint8_t clean[12];
    for (int j = 0; j < 12; j++) clean[j] = m_->my_key[j & 3] ^ (uint8_t)md[4 + j];
    l.number = clean[1];

    l.id = l.address.substr(3, 2) + l.address.substr(0, 2);
    l.name = "Light_" + l.id;
    l.isRegistered = true;
    m_->fireRegistered(l);
    m_->saveLights();
    Serial.printf("  + keyed light #%d (%s) under our key\n", l.number, l.id.c_str());
    break;
  }
}

// Debug: unfiltered scan — dump EVERY advertiser's manufacturer data.
void MeshController::RawScanCallback::onResult(const NimBLEAdvertisedDevice* dev) {
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

// ============================================================================
//  Discovery / keying
// ============================================================================
void MeshController::scan(int seconds) {
  Serial.printf("Discovery: WAKE + concurrent scan (%ds)\n", seconds);
  // WAKE: the reverse-engineered type-0 FastCon body (6 zero bytes, all-zero key).
  // Byte-identical to the working BRmesh-esp32 reference's do_generate_command(0,...),
  // which reliably wakes the bulbs — the bulb answers by advertising its factory identity.
  uint8_t data[] = { 0, 0, 0, 0, 0, 0 };
  uint8_t key[]  = { 0, 0, 0, 0 };
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::WAKE, std::vector<uint8_t>(data, data + 6), key, false);
  std::string sd = protocol::to_adv_data(rf);

  armMfgAdvertise(sd);

  // Bluedroid kept WAKE up THROUGH the scan; NimBLE can't (scan drops the advertiser),
  // so we time-slice WAKE + scan instead. wantDuplicates=true (set inside the helper)
  // matters here: the bulb interleaves bare beacons with its factory-identity frame,
  // and without it the per-address dedup locks onto the first (bare) beacon.
  slicedAdvertiseScan(seconds, 400, 200, new AddDeviceCallback(this), true);
  Serial.println("Discovery complete");
}

void MeshController::addLight(uint8_t number, LightDevice& light) {
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
  slicedAdvertiseScan(2, 300, 150, new AddLightCallback(this), true);
}

void MeshController::addLights(int seconds) {
  scan(seconds);
  delay(1000);   // bulbs need a brief pause after the scan
  int maxNum = 0;
  for (auto &l : myLights) if (l.number > maxNum) maxNum = l.number;
  for (auto &l : myLights) {
    if (l.isRegistered) continue;    // already known - don't re-key
    maxNum++;
    addLight(maxNum, l);
  }
}

void MeshController::requestScan(int seconds) {
  scanSeconds_ = (seconds > 0) ? seconds : SCAN_DURATION;
  scanRequested_ = true;
}

void MeshController::loop() {
  if (scanRequested_ && !isScanning_) {
    scanRequested_ = false;
    isScanning_ = true;
    Serial.println("Re-running discovery");
    addLights(scanSeconds_);
    isScanning_ = false;
    Serial.println("Discovery complete");
  }
}

// End a running (background) raw scan on demand. Only meaningful while a scan is
// active; harmless (no-op) otherwise.
void MeshController::scanStop() {
  if (pBLEScan && pBLEScan->isScanning()) { pBLEScan->stop(); Serial.println("scanstop: scan stopped"); }
  else Serial.println("scanstop: no active scan");
  pAdvertising->stop();
}

// ============================================================================
//  Debug: raw capture / WAKE replay
// ============================================================================
void MeshController::raw(int seconds) {
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
void MeshController::rawPassive(int seconds) {
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
// factory-identity frame, capturing EVERY raw frame (no dedup).
void MeshController::rawWake(int seconds) {
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
void MeshController::wakeFor(int seconds) {
  uint8_t data[] = { 0, 0, 0, 0, 0, 0 };
  uint8_t key[]  = { 0, 0, 0, 0 };
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
void MeshController::wakePhone(int seconds) {
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
//  Persistence (NVS) - key + registered light list
// ============================================================================
void MeshController::loadKeyOrGenerate() {
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

void MeshController::saveLights() {
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

void MeshController::loadLights() {
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
    fireRegistered(myLights.back());
  }
  prefs.end();
  Serial.printf("Restored %d lights from NVS\n", n);
}

// ============================================================================
//  List management (no air traffic)
// ============================================================================
// Re-register an already-keyed bulb into the light list + NVS, WITHOUT any air
// activity or SET_KEY. For when the NVS list records are lost but the key (and
// the bulb's key) survived — proven by a control command still working.
// id/name use the exact scheme AddLightCallback assigns (70:e7:.. -> "e770").
void MeshController::readd(const std::string& mac, uint8_t number) {
  for (auto &l : myLights) {
    if (l.address == mac) {
      Serial.printf("already in the list as #%d\n", l.number);
      return;
    }
  }
  LightDevice l;
  l.address = mac;
  l.type[0] = 0x2A;    // Color (2A A8) — the type of our bulbs
  l.type[1] = 0xA8;
  l.number = number;
  l.id = mac.substr(3, 2) + mac.substr(0, 2);
  l.name = "Light_" + l.id;
  l.isRegistered = true;
  myLights.push_back(l);
  fireRegistered(l);
  saveLights();
  Serial.printf("  + re-added light #%d (%s, %s) under current key\n",
                number, l.id.c_str(), mac.c_str());
}

void MeshController::forget(const std::string& arg) {
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
  Serial.println("forgotten from the list + NVS. (The bulb's key is untouched — nothing was sent over the air.)");
}

void MeshController::clearLights() {
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
  Serial.printf("Cleared %d light(s) from list + NVS (mesh key kept)\n", n);
}

// ============================================================================
//  Lifecycle
// ============================================================================
void MeshController::begin() {
  NimBLEDevice::init("BRMesh Controller");
  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->start();
  loadKeyOrGenerate();
  started_ = true;
}
