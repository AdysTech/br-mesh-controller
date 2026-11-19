// Serial console driver (see console.h). See mesh.h for the actions it delegates.
#include "console.h"
#include "MeshController.h"
#include "settings.h"
#include <Network.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void requestProvisioning();   // app shell (main.cpp)
void setDeviceName(const char* name);

void Console::banner() {
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
  Serial.println("  prov                 clear saved WiFi creds + reboot into Improv");
  Serial.println("  hostname [name]      show / set device name (DHCP hostname, 1-15 chars)");
  Serial.println("  showcfg              show all saved settings (passwords masked)");
  Serial.println("  set <k> <v>          save a setting — keys: name ssid pass mhost mport muser mpass");
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
      (strcmp(argv[0], "bootcatch") == 0) || (strcmp(argv[0], "raw") == 0) ||
      (strcmp(argv[0], "rawp") == 0) || (strcmp(argv[0], "rawwake") == 0) ||
      (strcmp(argv[0], "wake") == 0) || (strcmp(argv[0], "wakephone") == 0);
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

  else if (strcmp(argv[0], "readd") == 0) {
    if (argc < 2) { Serial.println("usage: readd <mac> [num]"); return; }
    uint8_t next = 0;
    for (auto &l : mesh_.lights()) if (l.number > next) next = l.number;
    uint8_t num = (argc >= 3) ? (uint8_t)atoi(argv[2]) : (uint8_t)(next + 1);
    mesh_.readd(std::string(argv[1]), num);
  }

  else if (strcmp(argv[0], "forget") == 0) {
    if (argc < 2) { Serial.println("usage: forget <mac|n>   (remove a light from our list + NVS; no air traffic)"); return; }
    mesh_.forget(std::string(argv[1]));
  }

  else if (strcmp(argv[0], "scan") == 0) {
    mesh_.requestScan((argc >= 2) ? atoi(argv[1]) : SCAN_DURATION);
    Serial.println("scan requested");
  }
  else if (strcmp(argv[0], "bootcatch") == 0) mesh_.scan((argc >= 2) ? atoi(argv[1]) : 12);
  else if (strcmp(argv[0], "raw") == 0) mesh_.raw((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "rawp") == 0) mesh_.rawPassive((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "rawwake") == 0) mesh_.rawWake((argc >= 2) ? atoi(argv[1]) : 5);
  else if (strcmp(argv[0], "scanstop") == 0) mesh_.scanStop();

  else if (strcmp(argv[0], "clear") == 0) mesh_.clearLights();

  else if (strcmp(argv[0], "prov") == 0) {
    requestProvisioning();     // clears creds + reboots — no return
    return;
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
  }

  else if (strcmp(argv[0], "set") == 0) {
    if (argc < 3) {
      Serial.println("usage: set <name|ssid|pass|mhost|mport|muser|mpass> <value>");
      Serial.println("       ('showcfg' to see current values)");
      return;
    }
    const char* k = argv[1];
    const std::string v(argv[2]);
    Settings s = loadSettings();
    bool wifiTouched = false;
    if (strcmp(k, "name") == 0) {
      if (v.size() < 1 || v.size() > 15) { Serial.println("name must be 1-15 chars"); return; }
      for (char c : v)
        if (!isalnum((unsigned char)c) && c != '-') { Serial.println("only letters, digits, '-' allowed"); return; }
      s.name = v;
    } else if (strcmp(k, "ssid") == 0 || strcmp(k, "sid") == 0) { s.ssid = v; wifiTouched = true; }
    else if (strcmp(k, "pass") == 0)                            { s.pass = v; wifiTouched = true; }
    else if (strcmp(k, "mhost") == 0 || strcmp(k, "mqtt") == 0) { s.mqttHost = v; }
    else if (strcmp(k, "mport") == 0) {
      int port = atoi(argv[2]);
      if (port < 1 || port > 65535) { Serial.println("port must be 1-65535"); return; }
      s.mqttPort = (uint16_t)port;
    } else if (strcmp(k, "muser") == 0) { s.mqttUser = v; }
    else if (strcmp(k, "mpass") == 0)  { s.mqttPass = v; }
    else {
      Serial.println("unknown key — one of: name ssid pass mhost mport muser mpass");
      return;
    }
    saveSettings(s);
    if (strcmp(k, "name") == 0) {
      // apply to the live netif too (same core quirk as `hostname`)
      NetworkInterface* def = Network.getDefaultInterface();
      if (def && def->setHostname(s.name.c_str()))
        Serial.printf("name '%s' saved + applied\n", s.name.c_str());
      else
        Serial.printf("name '%s' saved — active from the next boot\n", s.name.c_str());
    } else if (wifiTouched) {
      Serial.printf("%s saved — press RESET to apply (STA reconnects with the new creds)\n", k);
    } else {
      Serial.printf("%s saved\n", k);
    }
  }

  else if (strcmp(argv[0], "wake") == 0) mesh_.wakeFor((argc >= 2) ? atoi(argv[1]) : 3);
  else if (strcmp(argv[0], "wakephone") == 0) mesh_.wakePhone((argc >= 2) ? atoi(argv[1]) : 3);

  else Serial.printf("unknown command: %s (try 'help')\n", argv[0]);
}
