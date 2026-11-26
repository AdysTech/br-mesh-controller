// Serial console driver (see console.h). See mesh.h for the actions it delegates.
#include "console.h"
#include "MeshController.h"
#include "HaBridge.h"
#include "settings.h"
#include "web/WebLog.h"
#include <Network.h>
#include <esp_heap_caps.h>   // min-seen heap + largest free internal block (mem)
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void requestProvisioning();   // app shell (main.cpp)
void setDeviceName(const char* name);
extern HaBridge g_bridge;     // app shell (main.cpp); `mqtt` command + live mname

void Console::banner() {
  Serial.println("---- BRMesh serial console ----");
  Serial.println("  help                 this help");
  Serial.println("  list                 known lights");
  Serial.println("  key                  show current mesh key (NVS state)");
  Serial.println("  sel <n>              set target light number");
  Serial.println("  on [n]  off [n]      state (n optional)");
  Serial.println("  br <0-127> [n]       brightness");
  Serial.println("  ct <2700-6000> [n]   colour temp in Kelvin (2700=warm → 6000=cool)");
  Serial.println("  rgb r g b [n]        colour (0-255 each)");
  Serial.println("  rename <n> <text>   rename a light (friendly name; number + id unchanged)");
  Serial.println("  scan [sec]           re-run discovery / key new bulbs (default = configured window)");
  Serial.println("  forget <mac|n>      remove a light from our list + NVS (no air traffic; bulb's key untouched)");
  Serial.println("  raw [sec=5]          unfiltered BLE dump (debug)");
  Serial.println("  rawp [sec=5]         PASSIVE raw dump (no WAKE, debug)");
  Serial.println("  rawwake [sec=5]      WAKE broadcast + every frame (debug)");
  Serial.println("  scanstop             end a running raw scan on demand");
  Serial.println("  loglevel [lvl]       show / set log level — 0=quiet 1=normal 2=verbose (or the name)");
  Serial.println("  clear                remove all known lights (list + NVS; keeps key)");
  Serial.println("  prov                 clear saved WiFi creds + reboot into Improv");
  Serial.println("  mem                  show heap + PSRAM memory stats (free / min-seen / largest)");
  Serial.println("  reboot               restart the board now");
  Serial.println("  hostname [name]      show / set device name (DHCP hostname, 1-15 chars)");
  Serial.println("  mqtt                 HA bridge status (ctrl_id, link, queue)");
  Serial.println("  showcfg              show all saved settings (passwords masked)");
  Serial.println("  set <k> <v>          save a setting — keys: name ssid pass mhost mport muser mpass mclient mname scandur");
  Serial.println("  --------------------------");
}

void Console::poll() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len_ > 0) {
        buf_[len_] = 0;
        handle(buf_);
        len_ = 0;
      }
    } else if (len_ < (int)sizeof(buf_) - 1) {
      buf_[len_++] = c;
    }
  }
}

void Console::handle(const char* line) {
  Serial.print("> ");
  Serial.println(line);

  char bufCopy[128];
  strncpy(bufCopy, line, sizeof(bufCopy) - 1);
  bufCopy[sizeof(bufCopy) - 1] = 0;
  char* p = bufCopy;

  char* argv[8];
  int argc = 0;
  char* tok = strtok(p, " \t");
  while (tok && argc < 8) { argv[argc++] = tok; tok = strtok(nullptr, " \t"); }
  if (argc == 0) return;

  uint8_t n = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  uint8_t target = (target_ >= 1) ? (uint8_t)target_ : mesh_.firstLightNumber();

  // Radio commands need the engine started. On a provisioning boot (bridge
  // mode) the engine is deliberately offline — Improv owns the radio, and
  // touching it here would crash on a null advertiser.
  const bool radioCmd =
      (strcmp(argv[0], "on") == 0) || (strcmp(argv[0], "off") == 0) ||
      (strcmp(argv[0], "br") == 0) || (strcmp(argv[0], "ct") == 0) ||
      (strcmp(argv[0], "rgb") == 0) || (strcmp(argv[0], "scan") == 0) ||
      (strcmp(argv[0], "raw") == 0) || (strcmp(argv[0], "rawp") == 0) ||
      (strcmp(argv[0], "rawwake") == 0);
  if (radioCmd && !mesh_.started()) {
    Serial.println("engine offline (provisioning mode) — radio command rejected");
    return;
  }

  if (strcmp(argv[0], "help") == 0) banner();

  else if (strcmp(argv[0], "list") == 0) {
    const auto& lights = mesh_.lights();
    if (lights.empty()) Serial.println("  (no lights)");
    for (auto &l : lights)
      Serial.printf("  #%d %s [%s] %s\n", l.number, l.name.c_str(), l.id.c_str(),
                    l.isRegistered ? "registered" : "factory-key");
  }

  else if (strcmp(argv[0], "key") == 0) {
    const uint8_t* k = mesh_.key();
    Serial.printf("current mesh key: %02X %02X %02X %02X\n", k[0], k[1], k[2], k[3]);
  }

  else if (strcmp(argv[0], "sel") == 0) {
    if (argc >= 2) { target_ = atoi(argv[1]); Serial.printf("target set to #%d\n", target_); }
  }

  else if (strcmp(argv[0], "on") == 0)   { if (argc >= 2) target = n; mesh_.state(target, true);  Serial.println("sent ON"); }
  else if (strcmp(argv[0], "off") == 0)  { if (argc >= 2) target = n; mesh_.state(target, false); Serial.println("sent OFF"); }

  else if (strcmp(argv[0], "br") == 0) {
    if (argc < 2) { Serial.println("usage: br <0-127> [n]"); return; }
    if (argc >= 3) target = (uint8_t)atoi(argv[2]);
    mesh_.brightness(target, (uint8_t)atoi(argv[1]));
    Serial.printf("sent brightness=%d to #%d\n", atoi(argv[1]), target);
  }

  else if (strcmp(argv[0], "ct") == 0) {
    if (argc < 2) { Serial.println("usage: ct <value> [n]"); return; }
    if (argc >= 3) target = (uint8_t)atoi(argv[2]);
    mesh_.colorTemp(target, (uint16_t)atoi(argv[1]));
    Serial.printf("sent ct=%d to #%d\n", atoi(argv[1]), target);
  }

  else if (strcmp(argv[0], "rgb") == 0) {
    if (argc < 4) { Serial.println("usage: rgb r g b [n]"); return; }
    if (argc >= 5) target = (uint8_t)atoi(argv[4]);
    mesh_.rgb(target, (uint8_t)atoi(argv[1]), (uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]));
    Serial.printf("sent rgb=%d,%d,%d to #%d\n", atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), target);
  }

  else if (strcmp(argv[0], "rename") == 0) {
    if (argc < 3) { Serial.println("usage: rename <n> <text...>   (e.g. rename 1 'living room ceiling')"); return; }
    std::string nm(argv[2]);
    for (int i = 3; i < argc; i++) nm += std::string(" ") + std::string(argv[i]);
    if (nm.empty()) { Serial.println("name must be non-empty"); return; }
    if (mesh_.rename((uint8_t)atoi(argv[1]), nm))
      Serial.printf("renamed #%s\n", argv[1]);
    else
      Serial.println("no such light number (see 'list')");
  }

  else if (strcmp(argv[0], "forget") == 0) {
    if (argc < 2) { Serial.println("usage: forget <mac|n>   (remove a light from our list + NVS; no air traffic)"); return; }
    mesh_.forget(std::string(argv[1]));
  }

  else if (strcmp(argv[0], "scan") == 0) {
    mesh_.requestScan((argc >= 2) ? atoi(argv[1]) : loadSettings().scanSec);
    Serial.println("scan requested");
  }
  else if (strcmp(argv[0], "raw") == 0) mesh_.raw((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "rawp") == 0) mesh_.rawPassive((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "rawwake") == 0) mesh_.rawWake((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "scanstop") == 0) mesh_.scanStop();

  else if (strcmp(argv[0], "loglevel") == 0) {
    // P2.11: show or set the runtime log level. 0=quiet, 1=normal, 2=verbose.
    static const char* names[3] = { "quiet", "normal", "verbose" };
    if (argc < 2) {
      Serial.printf("log level: %d = %s  ('loglevel <0|1|2|quiet|normal|verbose>' to change; resets on reboot)\n",
                    (int)g_logLevel, names[(int)g_logLevel]);
      return;
    }
    int lvl = -1;
    if (strcmp(argv[1], "quiet") == 0 || strcmp(argv[1], "error") == 0) lvl = 0;
    else if (strcmp(argv[1], "normal") == 0 || strcmp(argv[1], "info") == 0) lvl = 1;
    else if (strcmp(argv[1], "verbose") == 0 || strcmp(argv[1], "debug") == 0) lvl = 2;
    else { lvl = atoi(argv[1]); if (lvl < 0 || lvl > 2) { Serial.println("level must be 0, 1 or 2"); return; } }
    g_logLevel = (int8_t)lvl;
    Serial.printf("log level set to %d = %s\n", lvl, names[lvl]);
  }

  else if (strcmp(argv[0], "clear") == 0) mesh_.clearLights();

  else if (strcmp(argv[0], "prov") == 0) {
    requestProvisioning();     // clears creds + reboots — no return
    return;
  }

  else if (strcmp(argv[0], "mem") == 0) {
    Serial.printf("  heap free          : %lu KB\n", (unsigned long)(esp_get_free_heap_size() / 1024));
    Serial.printf("  min-seen heap      : %lu KB\n", (unsigned long)(esp_get_minimum_free_heap_size() / 1024));
    Serial.printf("  largest free (int) : %lu KB\n",
                  (unsigned long)((uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    Serial.printf("  PSRAM              : %lu KB free / %lu KB total\n",
                  (unsigned long)(ESP.getFreePsram() / 1024), (unsigned long)(ESP.getPsramSize() / 1024));
    return;
  }

  else if (strcmp(argv[0], "reboot") == 0 || strcmp(argv[0], "restart") == 0) {
    Serial.println("restarting in 2 s…");
    delay(2000);
    ESP.restart();
    return;   // unreachable
  }

  else if (strcmp(argv[0], "hostname") == 0) {
    if (argc < 2) {
      Serial.printf("device name (NVS 'name'): %s  —  'hostname <name>' to change\n",
                    loadSettings().name.c_str());
      return;
    }
    const std::string nm(argv[1]);
    if (nm.size() < 1 || nm.size() > 15) { Serial.println("name must be 1-15 chars"); return; }
    for (char c : nm)
      if (!isalnum((unsigned char)c) && c != '-') { Serial.println("only letters, digits, '-' allowed"); return; }
    setDeviceName(nm.c_str());
  }
  else if (strcmp(argv[0], "mqtt") == 0) {
    Serial.printf("  enabled : %s\n", g_bridge.enabled() ? "yes" : "no (set mhost to enable)");
    if (g_bridge.enabled()) {
      Serial.printf("  ctrl_id : %s\n", g_bridge.ctrlId().c_str());
      Serial.printf("  link    : %s\n", g_bridge.connected() ? "online" : "offline (auto-reconnect)");
      Serial.printf("  lights  : %d\n", (int)mesh_.lightCount());
      Serial.printf("  queue   : depth %d, overflow %lu\n",
                    (int)mesh_.queueDepth(), (unsigned long)mesh_.queueOverflow());
    }
    return;
  }
  else if (strcmp(argv[0], "showcfg") == 0) {
    const Settings s = loadSettings();
    Serial.printf("  name    : %s\n", s.name.c_str());
    Serial.printf("  ssid    : %s\n",
                  s.ssid.empty() ? "(empty — boots into Improv)" : s.ssid.c_str());
    Serial.printf("  pass    : %s\n", s.pass.empty() ? "(empty)" : "****");
    Serial.printf("  mhost   : %s\n", s.mqttHost.c_str());
    Serial.printf("  mport   : %d\n", s.mqttPort);
    Serial.printf("  muser   : %s\n", s.mqttUser.empty() ? "(none)" : s.mqttUser.c_str());
    Serial.printf("  mpass   : %s\n", s.mqttPass.empty() ? "(none)" : "****");
    Serial.printf("  mclient : %s\n",
                  s.mqttClient.empty() ? ("(derived: " + g_bridge.clientId() + ")").c_str()
                                      : s.mqttClient.c_str());
    Serial.printf("  mname   : %s\n",
                  s.mqttName.empty() ? "(derived: " + s.name + ")" : s.mqttName.c_str());
    Serial.printf("  scandur : %d s   (discovery window — SPA + HA scan)\n", s.scanSec);
  }

  else if (strcmp(argv[0], "set") == 0) {
    if (argc < 3) {
      Serial.println("usage: set <name|ssid|pass|mhost|mport|muser|mpass|mclient|mname|scandur> <value>");
      Serial.println("       ('showcfg' to see current values)");
      return;
    }
    const char* k = argv[1];
    const std::string v(argv[2]);
    Settings s = loadSettings();
    bool wifiTouched = false;
    bool mqttTouched = false;
    if (strcmp(k, "name") == 0) {
      if (v.size() < 1 || v.size() > 15) { Serial.println("name must be 1-15 chars"); return; }
      for (char c : v)
        if (!isalnum((unsigned char)c) && c != '-') { Serial.println("only letters, digits, '-' allowed"); return; }
      s.name = v;
      mqttTouched = true;    // the HA hub name falls back to `name`
    } else if (strcmp(k, "ssid") == 0 || strcmp(k, "sid") == 0) { s.ssid = v; wifiTouched = true; }
    else if (strcmp(k, "pass") == 0)                            { s.pass = v; wifiTouched = true; }
    else if (strcmp(k, "mhost") == 0 || strcmp(k, "mqtt") == 0) { s.mqttHost = v; mqttTouched = true; }
    else if (strcmp(k, "mport") == 0) {
      int port = atoi(argv[2]);
      if (port < 1 || port > 65535) { Serial.println("port must be 1-65535"); return; }
      s.mqttPort = (uint16_t)port;
      mqttTouched = true;
    } else if (strcmp(k, "muser") == 0)   { s.mqttUser = v; mqttTouched = true; }
    else if (strcmp(k, "mpass") == 0)    { s.mqttPass = v; mqttTouched = true; }
    else if (strcmp(k, "mclient") == 0) { s.mqttClient = v; mqttTouched = true; }
    else if (strcmp(k, "mname") == 0)    { s.mqttName = v; mqttTouched = true; }
    else if (strcmp(k, "scandur") == 0)  {
      int sd = atoi(argv[2]);
      if (sd < 1 || sd > 600) { Serial.println("scandur must be 1-600 seconds"); return; }
      s.scanSec = sd;         // live value — no bridge update, no reboot
    }
    else {
      Serial.println("unknown key — one of: name ssid pass mhost mport muser mpass mclient mname scandur");
      return;
    }
    saveSettings(s);
    if (mqttTouched) g_bridge.updateSettings(s);
    if (strcmp(k, "name") == 0) {
      // apply to the live netif too (same core quirk as `hostname`)
      NetworkInterface* def = Network.getDefaultInterface();
      if (def && def->setHostname(s.name.c_str()))
        Serial.printf("name '%s' saved + applied\n", s.name.c_str());
      else
        Serial.printf("name '%s' saved — active from the next boot\n", s.name.c_str());
    } else if (wifiTouched) {
      Serial.printf("%s saved — press RESET to apply (STA reconnects with the new creds)\n", k);
    } else if (mqttTouched) {
      Serial.printf("%s saved — identity applied now; broker host/port/user/pass active from the next boot\n", k);
    } else {
      Serial.printf("%s saved\n", k);
    }
  }

  else Serial.printf("unknown command: %s (try 'help')\n", argv[0]);
}
