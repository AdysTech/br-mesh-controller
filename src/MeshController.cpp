// ============================================================================
//  MeshController — implementation. See mesh.h for the class contract.
//
//  Everything here is protocol + radio. No WiFi / MQTT / HA / HTTP / console.
// ============================================================================
#include "MeshController.h"
#include "web/WebLog.h" // leveled logger (P2.11): LOGE / LOGF / LOGD
#include <set>

// Light-type table: a *hint* for logging only. Type is never a gate — unknown
// types must still be keyed (control features depend on it).
namespace
{
  struct LightType
  {
    const char *name;
    uint8_t code[2];
  };
  const LightType lightTypes[] = {
      {"Smart", {0x39, 0xae}}, // 44601 - on/off only
      {"RGBW", {0xa1, 0xa8}},  // 43169 - brightness + CT + RGB
      {"RGB", {0xa0, 0xa8}},   // 43168 - brightness + RGB
      {"Color", {0x2a, 0xa8}}, // our 3 test bulbs — RGB-capable (protocol.md §2.6)
  };

  bool isDefaultKey(const std::string &key)
  {
    return key[0] == protocol::DEFAULT_KEY[0] && key[1] == protocol::DEFAULT_KEY[1] &&
           key[2] == protocol::DEFAULT_KEY[2] && key[3] == protocol::DEFAULT_KEY[3];
  }
} // namespace

// ============================================================================
//  State access
// ============================================================================
int MeshController::lightCount() const
{
  int n = 0;
  for (auto &l : myLights)
    if (l.isRegistered)
      n++;
  return n;
}

LightDevice *MeshController::findLightById(const std::string &id)
{
  for (auto &l : myLights)
    if (l.id == id)
      return &l;
  return nullptr;
}

LightDevice *MeshController::findLightByMac(const std::string &mac)
{
  for (auto &l : myLights)
    if (l.address == mac)
      return &l;
  return nullptr;
}

LightDevice *MeshController::findLightByNumber(uint8_t number)
{
  for (auto &l : myLights)
    if (l.isRegistered && l.number == number)
      return &l;
  return nullptr;
}

uint8_t MeshController::firstLightNumber()
{
  for (auto &l : myLights)
    if (l.isRegistered)
      return l.number;
  return 1;
}

// ============================================================================
//  Control frames (broadcast) — shared by HA callbacks and the serial console
// ============================================================================
// Build + arm one FastCon manufacturer advertisement on pAdvertising. `sd` is
// the raw AD structure from protocol::to_adv_data(): [len] 0xFF 0xF0 0xFF <payload>.
// NimBLEAdvertisementData::addData() appends raw bytes verbatim, so the on-air
// frame is exactly the AD structure protocol::to_adv_data() built (no re-encode).
void MeshController::armMfgAdvertise(const std::string &sd)
{
  NimBLEAdvertisementData ad;
  ad.setFlags(0x04); // BLE_HS_ADV_F_BREDR_UNSUP
  ad.addData((const uint8_t *)sd.data(), sd.size());
  pAdvertising->setAdvertisementData(ad);
  pAdvertising->setMinInterval(50);
  pAdvertising->setMaxInterval(50);
}

void MeshController::singleControl(const uint8_t key[4], const uint8_t data[8])
{
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::CONTROL,
                                 std::vector<uint8_t>(data, data + 8), key, true);
  std::string sd = protocol::to_adv_data(rf);
  armMfgAdvertise(sd);
  pAdvertising->start();
  delay(250);
  pAdvertising->stop();
}

// ============================================================================
//  Unified control — ONE broadcast per bulb carrying its ENTIRE desired state.
//
//  The bulbs are broadcast-only (no readback), so the last intent we sent IS the
//  truth. Every control — a user change or the convergence re-transmit — goes
//  out as a single frame with on + brightness + colour/white together. The old
//  path sent a colour frame at max brightness (0x72 d[2]=0xff) then a separate
//  dim frame (0x22), which is exactly the ~10 s full-brightness flicker we saw.
//  Here the 0x72 carries the real brightness in d[2] (protocol.md §2.5b), so one
//  frame fully specifies the bulb and no second dim frame is needed.
// ============================================================================
void MeshController::applyLight(const LightDevice& l)
{
  uint8_t d[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  d[1] = l.number;

  if (!l.on)
  {
    // Off: a single 0x22 with the on-bit clear (brightness/colour are moot when
    // the light is dark). Never send a colour frame to turn a light off.
    d[0] = 0x22;
    d[2] = 0x00;
    singleControl(my_key, d);
    return;
  }

  if (l.colorMode == 0)
  {
    // Plain white: a single 0x22 dim (the proven white-mode encoding; no colour
    // frame, so no flicker). d[2] = on-bit | brightness.
    d[0] = 0x22;
    d[2] = 0x80 | (l.brightness & 0x7f);
    singleControl(my_key, d);
    return;
  }

  // Colour modes (rgb / ct): one 0x72 with on + brightness + colour.
  d[0] = 0x72;
  d[2] = 0x80 | (l.brightness & 0x7f);   // on-bit | brightness (protocol.md §2.5b)
  if (l.colorMode == 1)
  {
    d[3] = l.rgbB;                        // blue
    d[4] = l.rgbR;                        // red
    d[5] = l.rgbG;                        // green
    d[6] = 0;                             // white pair off (RGB mode)
    d[7] = 0;
  }
  else // colorMode == 2 — white (colour-temp) WW/CW crossfade (protocol.md §2.5b).
  // ct is a Kelvin label (2700 warm → 6000 cool); this TX branch is the single
  // point where it is split into the two white channels — only the 0–255 WW/CW
  // pair rides the wire (protocol.md §2.5b).
  {
    int32_t k = l.ct;                                   // clamp Kelvin into the band
    if (k < 2700) k = 2700;
    if (k > 6000) k = 6000;
    uint8_t cool = (uint8_t)((uint32_t)(k - 2700) * 255u / 3300u);   // 0=warm … 255=cool
    d[3] = 0; d[4] = 0; d[5] = 0;                        // RGB triple off (white mode)
    d[6] = 255 - cool;                                   // warm-white LED (WW)
    d[7] = cool;                                         // cool-white LED (CW)
  }
  singleControl(my_key, d);
}

void MeshController::state(uint8_t n, bool on)
{
  if (LightDevice *l = findLightByNumber(n))
  {
    l->on = on;
    stateDirty_ = true;
    applyLight(*l);
  }
}

void MeshController::brightness(uint8_t n, uint8_t b)
{
  if (LightDevice *l = findLightByNumber(n))
  {
    l->brightness = b & 127;
    stateDirty_ = true;
    applyLight(*l);
  }
}

void MeshController::colorTemp(uint8_t n, uint16_t t)
{
  if (LightDevice *l = findLightByNumber(n))
  {
    l->ct = t;
    l->colorMode = 2;
    l->on = true;          // a CT frame implies on
    stateDirty_ = true;
    applyLight(*l);
  }
}

void MeshController::rgb(uint8_t n, uint8_t r, uint8_t g, uint8_t b)
{
  if (LightDevice *l = findLightByNumber(n))
  {
    l->rgbR = r;
    l->rgbG = g;
    l->rgbB = b;
    l->colorMode = 1;
    l->on = true;          // an RGB frame implies on
    stateDirty_ = true;
    applyLight(*l);
  }
}

// ============================================================================
//  Scan plumbing
// ============================================================================
// NimBLE's start() is NON-BLOCKING and takes duration in MILLISECONDS. The
// discovery / SET_KEY recipe must hold WAKE / SET_KEY advertising up for the
// whole scan window, so those paths poll until the scan finishes; the long raw
// dumps run in the background so 'scanstop' can cut them short.
void MeshController::waitScanDone(uint32_t timeoutMs)
{
  uint32_t t0 = millis();
  while (pBLEScan && pBLEScan->isScanning())
  {
    if ((uint32_t)(millis() - t0) > timeoutMs)
      break; // hard cap; scan self-terminates too
    delay(50);
  }
}

// NimBLE's ESP32 controller DROPS the advertiser the moment a scan starts, so the
// Bluedroid-style "advertise WAKE + scan concurrently" isn't available on this stack.
// Instead we time-slice: put the payload (WAKE / SET_KEY) on the air for advMs, then
// listen for scanMs, and repeat. The WAKE keeps re-appearing between scan windows, so
// the woken bulb's identity frames land during one of our scan windows.
void MeshController::slicedAdvertiseScan(int totalSec, int advMs, int scanMs,
                                         NimBLEScanCallbacks *cb, bool active)
{
  uint32_t target = (uint32_t)totalSec * 1000;
  uint32_t t0 = millis();
  int cycles = 0;
  while ((uint32_t)(millis() - t0) < target)
  {
    // 1) payload on the air (WAKE / SET_KEY)
    if (!pAdvertising->isAdvertising())
      pAdvertising->start();
    delay(advMs);
    pAdvertising->stop();
    delay(20); // let the controller release the radio before scanning
    // 2) listen (advertiser is down, so the scan owns the radio)
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(cb, true); // wantDuplicates = true (see raw notes)
    pBLEScan->setDuplicateFilter(0);
    pBLEScan->setInterval(300);
    pBLEScan->setWindow(300);
    pBLEScan->setActiveScan(active);
    pBLEScan->start(scanMs, false, false);
    delay(50); // let the scan actually begin (host task is async)
    waitScanDone((uint32_t)scanMs + 200);
    cycles++;
  }
  pAdvertising->stop();
  LOGD("  [sliced: %d WAKE/scan cycles]", cycles);
}

// ============================================================================
//  Scan callbacks
// ============================================================================
// Discovery: detect a factory-key bulb and add it to the list for keying.
void MeshController::AddDeviceCallback::onResult(const NimBLEAdvertisedDevice *dev)
{
  std::string address = dev->getAddress().toString();
  std::string mfg = dev->getManufacturerData();
  if (mfg.size() != 18)
    return;

  std::string type = mfg.substr(12, 2);
  std::string key = mfg.substr(14, 4);

  bool knownType = false;
  for (auto &t : lightTypes)
  {
    if (type[0] == t.code[0] && type[1] == t.code[1])
    {
      knownType = true;
      break;
    }
  }
  LightDevice l; // unknown types must still be keyed.
  l.address = address;
  l.mfgData.assign(mfg.begin(), mfg.end());
  l.type[0] = (uint8_t)type[0];
  l.type[1] = (uint8_t)type[1];

  if (isDefaultKey(key))
  { // factory key present == BRGD bulb needing keying.
    LightDevice *ex = m_->findLightByMac(address);
    if (ex)
    {
      // Each device boots with the default key, so if we see the same MAC
      // advertising again it has gone back to factory/pairing mode. Refresh the
      // fresh 18-byte manufacturer data we just caught (the source of the SET_KEY
      // payload) and keep its number + registration — do NOT push a duplicate.
      ex->mfgData = l.mfgData;
      ex->type[0] = l.type[0];
      ex->type[1] = l.type[1];
      ex->needsKey = true;          // back in pairing mode — key it this cycle
      LOGD("Known device: %s type=%02X %02X - back in pairing mode, will re-pair",
           address.c_str(), ex->type[0], ex->type[1]);
    }
    else
    {
      // type is a hint, so we will try pairing for unknown types too. The bulb
      // will reject the SET_KEY if it doesn't support it.
      LOGF("New device found: %s type=%02X %02X%s (needs keying)",
           address.c_str(), l.type[0], l.type[1], knownType ? "" : " [unknown type]");
      l.needsKey = true;            // key it this cycle
      m_->myLights.push_back(l);
    }
  }
  else
  { // non-default key == Never seen as each device boots with factory key
    LOGD("  * light found: %s type=%02X %02X%s key=%s",
         address.c_str(), l.type[0], l.type[1], knownType ? "" : " [unknown type]", key.c_str());
  }
}

// Keying: watch for a bulb we just SET_KEY to re-advertise under OUR key, then
// read the number it reports back (XOR-decrypted with our key) and register it.
void MeshController::AddLightCallback::onResult(const NimBLEAdvertisedDevice *dev)
{
  std::string address = dev->getAddress().toString();
  std::string mfg = dev->getManufacturerData();
  if (mfg.size() != 18)
    return;

  for (auto &l : m_->myLights)
  {
    if (l.address != address)
      continue;

    std::string key = mfg.substr(14, 4);
    if (isDefaultKey(key))
      continue; // key not accepted yet
    if (l.isRegistered)
      continue; // already done

    // Read the number the bulb reports back, decrypting with our key.
    std::string md = mfg.substr(2); // 16 bytes
    uint8_t clean[12];
    for (int j = 0; j < 12; j++)
      clean[j] = m_->my_key[j & 3] ^ (uint8_t)md[4 + j];
    l.number = clean[1];

    l.id = l.address.substr(3, 2) + l.address.substr(0, 2);
    l.name = "Light_" + l.id;
    l.isRegistered = true;
    m_->fireRegistered(l);
    m_->saveLights();
    LOGF("  + keyed light #%d (%s) under our key", l.number, l.id.c_str());
    break;
  }
}

// Debug: unfiltered scan — dump EVERY advertiser's manufacturer data.
void MeshController::RawScanCallback::onResult(const NimBLEAdvertisedDevice *dev)
{
  std::string addr = dev->getAddress().toString();
  std::string mfgS = dev->getManufacturerData();
  if (mfgS.length() > 0)
  {
    char hex[96];
    int pos = 0;
    for (size_t i = 0; i < mfgS.length() && pos < (int)sizeof(hex) - 3; i++)
      pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", (uint8_t)mfgS[i]);
    LOGF("RAW %s mfg(%d): %s", addr.c_str(), (int)mfgS.length(), hex);
  }
  else
  {
    LOGF("RAW %s (no mfg data)", addr.c_str());
  }
}

// ============================================================================
//  Discovery / keying
// ============================================================================
void MeshController::scan(int seconds)
{
  LOGF("Discovery: WAKE + concurrent scan (%ds)", seconds);
  // WAKE: the reverse-engineered type-0 FastCon body (6 zero bytes, all-zero key).
  // Byte-identical to the working BRmesh-esp32 reference's do_generate_command(0,...),
  // which reliably wakes the bulbs — the bulb answers by advertising its factory identity.
  uint8_t data[] = {0, 0, 0, 0, 0, 0};
  uint8_t key[] = {0, 0, 0, 0};
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::WAKE, std::vector<uint8_t>(data, data + 6), key, false);
  std::string sd = protocol::to_adv_data(rf);

  armMfgAdvertise(sd);

  // Bluedroid kept WAKE up THROUGH the scan; NimBLE can't (scan drops the advertiser),
  // so we time-slice WAKE + scan instead. wantDuplicates=true (set inside the helper)
  // matters here: the bulb interleaves bare beacons with its factory-identity frame,
  // and without it the per-address dedup locks onto the first (bare) beacon.
  slicedAdvertiseScan(seconds, 400, 200, new AddDeviceCallback(this), true);
  LOGF("Discovery complete");
}

void MeshController::addLight(uint8_t number, LightDevice &light)
{
  LOGF("Setting key on light #%d, MAC: %s", number, light.address.c_str());
  LOGF("new key: %02X %02X %02X %02X", my_key[0], my_key[1], my_key[2], my_key[3]);

  uint8_t data[12];
  if (light.mfgData.size() < 12)
  {
    LOGF("skip keying %s: no manufacturer data captured this session (restore-only entry)",
         light.address.c_str());
    return;
  }
  for (int i = 0; i < 6; i++)
    data[i] = light.mfgData[6 + i]; // mac (from mfg data)
  data[6] = number;                 // requested light id
  data[7] = 0x01;                   // group id
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

void MeshController::addLights(int seconds)
{
  LOGF("Discovery+key: WAKE on the air, key each bulb as its identity is caught (%ds)", seconds);

  // The WAKE payload, built once (reverse-engineered type-0 body, all-zero key —
  // byte-identical to the proven reference). WAKE is what holds a freshly-booted
  // bulb in its pairing window (protocol.md §2.9) so it will accept the SET_KEY
  // we fire on it; the app keeps WAKE on the air the whole pairing, so we do too.
  uint8_t data[] = { 0, 0, 0, 0, 0, 0 };
  uint8_t key[] = { 0, 0, 0, 0 };
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::WAKE, std::vector<uint8_t>(data, data + 6), key, false);
  std::string wakeSd = protocol::to_adv_data(rf);

  // Next number to hand a brand-new bulb.
  int nextNum = 0;
  for (auto &l : myLights)
    if ((int)l.number > nextNum)
      nextNum = l.number;

  // MACs we have CONFIRMED keyed during this run. A bulb we just keyed keeps
  // advertising its factory-identity frame for the rest of its boot window
  // (identity frames are only emitted in the seconds after power-up), so a later
  // cycle catches it again and AddDeviceCallback re-flags it — but it is already
  // ours, and firing SET_KEY a second time is pure noise. A bulb that never
  // confirmed this session is NOT in this set, so it keeps being retried (and is
  // dropped as a ghost at the end). Session-scoped on purpose: a bulb restored
  // from NVS that is back in pairing mode is still re-keyed on the next scan
  // (protocol.md §2.9) — it just isn't re-keyed every cycle within one run.
  std::set<std::string> done;

  uint32_t target = (uint32_t)seconds * 1000;
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < target)
  {
    // 1) WAKE on the air — keeps a just-booted bulb in its pairing window.
    //    (Re)arm WAKE here: a previous cycle's addLight() left SET_KEY armed.
    armMfgAdvertise(wakeSd);
    if (!pAdvertising->isAdvertising())
      pAdvertising->start();
    delay(400);
    pAdvertising->stop();
    delay(20); // let the controller release the radio before scanning

    // 2) listen for a factory-identity frame (AddDeviceCallback pushes / refreshes
    //    it and flags needsKey). wantDuplicates=true: the bulb interleaves bare
    //    beacons with its identity frame, and per-address dedup would drop it.
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(new AddDeviceCallback(this), true);
    pBLEScan->setDuplicateFilter(0);
    pBLEScan->setInterval(300);
    pBLEScan->setWindow(300);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(200, false, false);
    delay(50);
    waitScanDone(400);

    // 3) key each bulb caught THIS cycle — while WAKE is still fresh, so its
    //    pairing window is open. Keying on detection (not after the whole scan
    //    window) is the fix for "dropped unkeyed".
    for (auto &l : myLights)
    {
      if (!l.needsKey || l.mfgData.size() < 12)
        continue;
      if (done.count(l.address))
        continue; // confirmed earlier this run — don't re-key it
      l.needsKey = false;
      pAdvertising->stop(); // clean advertiser state before SET_KEY arms
      if (l.isRegistered)
        addLight(l.number, l);   // re-pair: a known bulb back in pairing mode — keep its number
      else
        addLight(++nextNum, l);  // brand new: hand out the next number
      if (l.isRegistered)
        done.insert(l.address);  // confirmed — leave it alone for the rest of this run
    }
  }
  pAdvertising->stop();

  // A bulb caught but never keyed (confirm never landed) is a ghost (#0, no id,
  // isRegistered=false). Drop it so the next scan re-detects and retries cleanly
  // instead of leaving a permanent unkeyed entry.
  bool dropped = false;
  for (auto it = myLights.begin(); it != myLights.end();)
  {
    if (!it->isRegistered)
    {
      LOGF("dropped unkeyed %s (confirm never landed — rescan to retry)", it->address.c_str());
      it = myLights.erase(it);
      dropped = true;
    }
    else
      ++it;
  }
  if (dropped)
    saveLights(); // reindex the now-consistent list into NVS
  LOGF("Discovery+key complete");
}

void MeshController::requestScan(int seconds)
{
  scanSeconds_ = (seconds > 0) ? seconds : SCAN_DURATION;
  scanRequested_ = true;
}

void MeshController::loop()
{
  if (scanRequested_ && !isScanning_)
  {
    scanRequested_ = false;
    isScanning_ = true;
    LOGF("Re-running discovery");
    addLights(scanSeconds_);
    isScanning_ = false;
    LOGF("Discovery complete");
  }
  saveStateIfDirty(); // FR-05: persist believed state (debounced; no-op when clean)
  retransmitTick();   // P2.9: re-assert last known state on a cadence
}

// End a running (background) raw scan on demand. Only meaningful while a scan is
// active; harmless (no-op) otherwise.
void MeshController::scanStop()
{
  if (pBLEScan && pBLEScan->isScanning())
  {
    pBLEScan->stop();
    LOGF("scanstop: scan stopped");
  }
  else
    LOGF("scanstop: no active scan");
  pAdvertising->stop();
}

// ============================================================================
//  Debug: raw capture / WAKE replay
// ============================================================================
// Shared raw-capture setup for raw()/rawPassive(): they differ only in whether
// active scan (SCAN_REQ) is on. Runs in the background; 'scanstop' ends early.
void MeshController::startRawScan(int seconds, bool active)
{
  pAdvertising->stop();
  pBLEScan = NimBLEDevice::getScan();
  // wantDuplicates = true: fire onResult for EVERY frame (no per-address dedup),
  // so we see all of a bulb's advertisement types across its short boot window.
  pBLEScan->setScanCallbacks(new RawScanCallback(), true);
  pBLEScan->setDuplicateFilter(0); // explicit: also drop the controller-level dup filter
  pBLEScan->setInterval(300);
  pBLEScan->setWindow(300);
  pBLEScan->setActiveScan(active);
  pBLEScan->start(seconds * 1000, false, false); // duration is MILLISECONDS; non-blocking
}

// Active raw scan: normal SCAN_REQ, every frame, unfiltered.
void MeshController::raw(int seconds)
{
  LOGF("RAW scan (~%ds; 'scanstop' to end early): every advertiser, unfiltered", seconds);
  startRawScan(seconds, true);
  LOGF("  (runs in the background; ends on its own or with 'scanstop')");
}

// Passive raw scan: PURE listening (no SCAN_REQ). Best for catching a phone's
// ONE-SHOT FastCon broadcast (e.g. the app's "Forget/Reset"): active scan's
// SCAN_REQ transmissions steal RX time, so passive listens the whole of each
// channel slot and drops fewer one-shots.
void MeshController::rawPassive(int seconds)
{
  LOGF("RAW+PASSIVE (%ds): pure listening, every frame, no SCAN_REQ", seconds);
  startRawScan(seconds, false);
  LOGF("  (runs in the background; ends on its own or with 'scanstop')");
}

// WAKE broadcast + duplicate raw scan: tests whether a WAKE triggers the
// factory-identity frame, capturing EVERY raw frame (no dedup).
void MeshController::rawWake(int seconds)
{
  uint8_t data[] = {0, 0, 0, 0, 0, 0};
  uint8_t key[] = {0, 0, 0, 0};
  std::vector<uint8_t> rf =
      protocol::generate_command(protocol::WAKE, std::vector<uint8_t>(data, data + 6), key, false);
  std::string sd = protocol::to_adv_data(rf);
  armMfgAdvertise(sd);

  LOGF("RAW+WAKE (%ds): time-sliced WAKE + raw scan (every frame)", seconds);
  slicedAdvertiseScan(seconds, 300, 150, new RawScanCallback(), true);
  LOGF("RAW+WAKE done");
}

// ============================================================================
//  Persistence (NVS) - key + registered light list
// ============================================================================
void MeshController::loadKeyOrGenerate()
{
  prefs.begin("brmesh", false);
  if (prefs.getBytes("key", my_key, 4) == 4)
  {
    LOGF("Loaded existing mesh key from NVS");
  }
  else
  {
    uint32_t rk = esp_random();
    my_key[0] = rk & 0xFF;
    my_key[1] = (rk >> 8) & 0xFF;
    my_key[2] = (rk >> 16) & 0xFF;
    my_key[3] = (rk >> 24) & 0xFF;
    prefs.begin("brmesh", false);
    prefs.putBytes("key", my_key, 4);
    LOGF("Generated a NEW mesh key and saved it");
  }
  prefs.end();
}

void MeshController::saveLights()
{
  int n = 0;
  for (auto &l : myLights)
    if (l.isRegistered)
      n++;
  prefs.begin("brmesh", false);
  prefs.putInt("count", n);
  int i = 0;
  for (auto &l : myLights)
  {
    if (!l.isRegistered)
      continue;
    String idk = "id" + String(i);
    String nmk = "nm" + String(i);
    String mck = "mac" + String(i);
    String t0k = "t0" + String(i);
    String t1k = "t1" + String(i);
    String nuk = "num" + String(i);
    prefs.putString(idk.c_str(), l.id.c_str());
    prefs.putString(nmk.c_str(), l.name.c_str());
    prefs.putString(mck.c_str(), l.address.c_str());
    prefs.putInt(t0k.c_str(), l.type[0]);
    prefs.putInt(t1k.c_str(), l.type[1]);
    prefs.putInt(nuk.c_str(), l.number);
    // Believed state (FR-05) — restore on reboot so the UI shows the last intent,
    // not a white box. Keys absent on pre-P2.8 entries load as the defaults.
    prefs.putInt(("on" + String(i)).c_str(), l.on ? 1 : 0);
    prefs.putInt(("bri" + String(i)).c_str(), l.brightness);
    prefs.putInt(("rr" + String(i)).c_str(), l.rgbR);
    prefs.putInt(("rg" + String(i)).c_str(), l.rgbG);
    prefs.putInt(("rb" + String(i)).c_str(), l.rgbB);
    prefs.putInt(("ct" + String(i)).c_str(), l.ct);   // Kelvin — one 16-bit value, one key
    prefs.putInt(("cm" + String(i)).c_str(), l.colorMode);
    i++;
  }
  prefs.end();
  LOGD("Saved %d registered lights to NVS", n);
}

void MeshController::loadLights()
{
  prefs.begin("brmesh", false);
  int n = prefs.getInt("count", 0);
  for (int i = 0; i < n; i++)
  {
    String idk = "id" + String(i);
    String nmk = "nm" + String(i);
    String mck = "mac" + String(i);
    String t0k = "t0" + String(i);
    String t1k = "t1" + String(i);
    String nuk = "num" + String(i);
    LightDevice l;
    l.id = prefs.getString(idk.c_str(), "").c_str();
    l.name = prefs.getString(nmk.c_str(), "Light").c_str();
    l.address = prefs.getString(mck.c_str(), "").c_str(); // empty on pre-mac entries
    l.type[0] = (uint8_t)prefs.getInt(t0k.c_str(), 0);
    l.type[1] = (uint8_t)prefs.getInt(t1k.c_str(), 0);
    l.number = (uint8_t)prefs.getInt(nuk.c_str(), 0);
    // Believed state (FR-05) — defaults (on/full-white) when the keys are absent.
    l.on = prefs.getInt(("on" + String(i)).c_str(), 1) != 0;
    l.brightness = (uint8_t)prefs.getInt(("bri" + String(i)).c_str(), 127);
    l.rgbR = (uint8_t)prefs.getInt(("rr" + String(i)).c_str(), 255);
    l.rgbG = (uint8_t)prefs.getInt(("rg" + String(i)).c_str(), 255);
    l.rgbB = (uint8_t)prefs.getInt(("rb" + String(i)).c_str(), 255);
    l.ct = (uint16_t)prefs.getInt(("ct" + String(i)).c_str(), 4000);   // absent (pre-Kelvin) → neutral
    l.colorMode = (uint8_t)prefs.getInt(("cm" + String(i)).c_str(), 0);
    l.isRegistered = true;
    myLights.push_back(l);
    fireRegistered(myLights.back());
  }
  prefs.end();
  LOGF("Restored %d lights from NVS", n);
}

// ============================================================================
//  Believed state (FR-05) + convergence re-transmit (FR-16 / P2.9)
// ============================================================================
// Debounced NVS persist of the believed state: a control method marks it dirty,
// and loop() commits at most once per 500 ms so a slider drag doesn't hammer NVS.
void MeshController::saveStateIfDirty()
{
  if (!stateDirty_)
    return;
  uint32_t now = millis();
  if ((uint32_t)(now - lastStateSaveMs_) < 500)
    return;
  lastStateSaveMs_ = now;
  stateDirty_ = false;
  saveLights();
}

// Re-assert every registered light's last known state so a bulb that missed a
// command (it was unpowered when we sent it) converges to what the user set.
// One unified applyLight() frame per light — the exact same frame a user change
// sends, so a re-transmit and a control are indistinguishable on the air.
// Skipped while scanning (the radio is time-sliced) and when disabled (0).
void MeshController::retransmitTick()
{
  if (retransmitIntervalMs_ == 0)
    return;
  if (!started_ || isScanning_)
    return;
  uint32_t now = millis();
  if ((uint32_t)(now - lastRetransmitMs_) < retransmitIntervalMs_)
    return;
  lastRetransmitMs_ = now;

  int sent = 0;
  for (auto &l : myLights)
  {
    if (!l.isRegistered)
      continue;
    applyLight(l);
    sent++;
  }
  if (sent)
    LOGD("converge: re-asserted %d light(s)", sent);
  stateDirty_ = false; // the re-transmit re-sent, not changed, the state — don't re-save
}

bool MeshController::rename(uint8_t number, const std::string &name)
{
  for (auto &l : myLights)
  {
    if (l.number == number)
    {
      if (l.name == name)
        return true;
      LOGF("renaming #%d '%s' -> '%s'", number, l.name.c_str(), name.c_str());
      l.name = name;
      saveLights();
      return true;
    }
  }
  LOGF("rename: no light #%d in the list", number);
  return false;
}

void MeshController::forget(const std::string &arg)
{
  bool byMac = (arg.find(':') != std::string::npos);
  bool found = false;
  for (auto it = myLights.begin(); it != myLights.end(); ++it)
  {
    bool match = byMac ? (it->address == arg)
                       : (it->number == (uint8_t)atoi(arg.c_str()));
    if (match)
    {
      LOGF("forgetting #%d %s [%s]", it->number, it->name.c_str(), it->id.c_str());
      myLights.erase(it);
      found = true;
      break;
    }
  }
  if (!found)
  {
    LOGF("no light matching '%s' in the list (use 'list')", arg.c_str());
    return;
  }
  saveLights();
  LOGF("forgotten from the list + NVS. (The bulb's key is untouched — nothing was sent over the air.)");
}

void MeshController::clearLights()
{
  // Wipe the known-lights list (RAM + NVS); keeps our mesh key. Needed before
  // re-keying a bulb whose stale NVS entry would make addLights() skip it.
  prefs.begin("brmesh", false);
  int n = prefs.getInt("count", 0);
  for (int i = 0; i < n; i++)
  {
    prefs.remove(("id" + String(i)).c_str());
    prefs.remove(("nm" + String(i)).c_str());
    prefs.remove(("t0" + String(i)).c_str());
    prefs.remove(("t1" + String(i)).c_str());
    prefs.remove(("num" + String(i)).c_str());
  }
  prefs.putInt("count", 0);
  prefs.end();
  myLights.clear();
  LOGF("Cleared %d light(s) from list + NVS (mesh key kept)", n);
}

// ============================================================================
//  Lifecycle
// ============================================================================
void MeshController::begin()
{
  NimBLEDevice::init("BRMesh Controller");
  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->start();
  loadKeyOrGenerate();
  started_ = true;
}
