#include <M5Dial.h>
#include <NimBLEDevice.h>
#include <strings.h>

#include "config.h"
#include "victron.h"

// ---------------------------------------------------------------------------
// État partagé entre la tâche BLE (callback) et la boucle principale
// ---------------------------------------------------------------------------
struct Slot {
  uint8_t key[16] = {0};
  bool keyOk = false;
  bool valid = false;
  uint32_t lastMs = 0;
  victron::Reading data;
};

static Slot slots[NUM_DEVICES];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static M5Canvas cv(&M5Dial.Display);
static size_t sel = 0;
static long lastEnc = 0;
static int encAccum = 0;
static int brightIdx = 0;
static const uint8_t BRIGHT[] = {200, 100, 30};

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------
class ScanCb : public NimBLEScanCallbacks {
  std::string seen[8];
  int nSeen = 0;

  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveManufacturerData()) return;
    std::string md = dev->getManufacturerData();
    if (md.size() < 3) return;
    const uint8_t* p = (const uint8_t*)md.data();
    if (p[0] != (victron::MANUFACTURER_ID & 0xFF) ||
        p[1] != (victron::MANUFACTURER_ID >> 8))
      return;

    std::string addr = dev->getAddress().toString();

    for (size_t i = 0; i < NUM_DEVICES; i++) {
      if (strcasecmp(addr.c_str(), DEVICES[i].mac) != 0) continue;
      if (!slots[i].keyOk) return;
      victron::Reading r;
      if (victron::decode(md, slots[i].key, r)) {
        portENTER_CRITICAL(&mux);
        slots[i].data = r;
        slots[i].lastMs = millis();
        slots[i].valid = true;
        portEXIT_CRITICAL(&mux);
      }
      return;
    }

    // Appareil Victron non configuré : on l'affiche une fois sur le port série
    for (int i = 0; i < nSeen; i++)
      if (seen[i] == addr) return;
    if (nSeen < 8) seen[nSeen++] = addr;
    Serial.printf("Victron detecte : %s (RSSI %d)\n", addr.c_str(),
                  dev->getRSSI());
  }
};
static ScanCb scanCb;

static void startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCb, true);  // true = garder les doublons
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setMaxResults(0);  // ne pas stocker les résultats en mémoire
  scan->start(0, false, true);
}

// ---------------------------------------------------------------------------
// Affichage (écran rond 240x240)
// ---------------------------------------------------------------------------
static const uint16_t C_BG = 0x0000;
static const uint16_t C_DIM = 0x2124;
static const uint16_t C_TXT = 0xFFFF;
static const uint16_t C_GREY = 0x8410;
static const uint16_t C_GREEN = 0x07E0;
static const uint16_t C_ORANGE = 0xFD20;
static const uint16_t C_RED = 0xF800;
static const uint16_t C_CYAN = 0x07FF;

static void fmt(char* b, size_t n, const char* f, float v) {
  if (isnan(v)) {
    strlcpy(b, "--", n);
  } else {
    snprintf(b, n, f, v);
  }
}

static const char* solarState(uint8_t s) {
  switch (s) {
    case 0: return "Arret";
    case 1: return "Faible puiss.";
    case 2: return "Defaut";
    case 3: return "Bulk";
    case 4: return "Absorption";
    case 5: return "Float";
    case 6: return "Stockage";
    case 7: return "Egalisation";
    case 9: return "Onduleur";
    case 11: return "Alim.";
    case 245: return "Demarrage";
    case 246: return "Absorp. rep.";
    case 247: return "Reconditionn.";
    case 248: return "Batt. sure";
    case 252: return "Ctrl externe";
    default: return "?";
  }
}

static void text(const char* s, int x, int y, const lgfx::IFont* f,
                 uint16_t col) {
  cv.setFont(f);
  cv.setTextColor(col);
  cv.setTextDatum(middle_center);
  cv.drawString(s, x, y);
}

static void drawBattery(const victron::BatteryData& b) {
  char buf[32];
  float s = isnan(b.soc) ? 0 : b.soc / 100.0f;
  uint16_t col = (b.soc < 20) ? C_RED : (b.soc < 50 ? C_ORANGE : C_GREEN);

  cv.fillArc(120, 120, 114, 102, 135, 405, C_DIM);
  if (s > 0.005f) cv.fillArc(120, 120, 114, 102, 135, 135 + 270 * s, col);

  fmt(buf, sizeof(buf), "%.0f%%", b.soc);
  text(buf, 120, 100, &fonts::FreeSansBold24pt7b, C_TXT);

  fmt(buf, sizeof(buf), "%.2f V", b.voltage);
  text(buf, 120, 140, &fonts::FreeSansBold12pt7b, C_CYAN);

  fmt(buf, sizeof(buf), "%.1f A", b.current);
  text(buf, 120, 166, &fonts::FreeSans12pt7b, C_TXT);

  if (b.ttgMin >= 0) {
    snprintf(buf, sizeof(buf), "Reste %dh%02d", (int)(b.ttgMin / 60),
             (int)(b.ttgMin % 60));
  } else {
    strlcpy(buf, "Reste --", sizeof(buf));
  }
  text(buf, 120, 190, &fonts::FreeSans9pt7b, C_GREY);
}

static void drawSolar(const victron::SolarData& s) {
  char buf[32];
  bool active = !isnan(s.pvW) && s.pvW > 0;
  cv.fillArc(120, 120, 114, 108, 0, 360, active ? C_ORANGE : C_DIM);

  fmt(buf, sizeof(buf), "%.0f W", s.pvW);
  text(buf, 120, 100, &fonts::FreeSansBold24pt7b, C_TXT);

  text(solarState(s.state), 120, 138, &fonts::FreeSansBold12pt7b,
       s.error ? C_RED : C_GREEN);

  char v[16], a[16];
  fmt(v, sizeof(v), "%.2f V", s.batteryV);
  fmt(a, sizeof(a), "%.1f A", s.batteryA);
  snprintf(buf, sizeof(buf), "%s  %s", v, a);
  text(buf, 120, 166, &fonts::FreeSans12pt7b, C_CYAN);

  float kwh = isnan(s.yieldWh) ? NAN : s.yieldWh / 1000.0f;
  char y[16];
  fmt(y, sizeof(y), "%.2f", kwh);
  snprintf(buf, sizeof(buf), "Jour %s kWh", y);
  text(buf, 120, 190, &fonts::FreeSans9pt7b, C_GREY);
}

static void draw() {
  Slot snap;
  portENTER_CRITICAL(&mux);
  snap.valid = slots[sel].valid;
  snap.lastMs = slots[sel].lastMs;
  snap.data = slots[sel].data;
  portEXIT_CRITICAL(&mux);

  cv.fillSprite(C_BG);

  bool fresh = snap.valid && (millis() - snap.lastMs < STALE_MS);
  if (fresh) {
    if (snap.data.type == victron::Type::BatteryMonitor)
      drawBattery(snap.data.batt);
    else if (snap.data.type == victron::Type::SolarCharger)
      drawSolar(snap.data.solar);
  } else {
    text("Pas de signal", 120, 120, &fonts::FreeSans12pt7b, C_GREY);
  }

  text(DEVICES[sel].name, 120, 24, &fonts::FreeSansBold9pt7b, C_TXT);

  char pg[8];
  snprintf(pg, sizeof(pg), "%u/%u", (unsigned)(sel + 1), (unsigned)NUM_DEVICES);
  text(pg, 120, 224, &fonts::FreeSans9pt7b, C_GREY);

  cv.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Arduino
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);  // encodeur oui, RFID non
  M5Dial.Display.setBrightness(BRIGHT[brightIdx]);

  cv.setColorDepth(16);
  cv.createSprite(240, 240);

  for (size_t i = 0; i < NUM_DEVICES; i++) {
    slots[i].keyOk = victron::hexToBytes(DEVICES[i].keyHex, slots[i].key, 16);
    if (!slots[i].keyOk) Serial.printf("Cle invalide pour %s\n", DEVICES[i].name);
  }

  NimBLEDevice::init("");
  startScan();

  lastEnc = M5Dial.Encoder.read();
  draw();
}

void loop() {
  M5Dial.update();

  // Encodeur : change d'appareil
  long pos = M5Dial.Encoder.read();
  encAccum += (int)(pos - lastEnc);
  lastEnc = pos;
  if (abs(encAccum) >= ENCODER_COUNTS_PER_DETENT) {
    int dir = encAccum > 0 ? 1 : -1;
    encAccum = 0;
    sel = (sel + NUM_DEVICES + dir) % NUM_DEVICES;
    draw();
  }

  // Bouton : luminosité
  if (M5Dial.BtnA.wasPressed()) {
    brightIdx = (brightIdx + 1) % (int)sizeof(BRIGHT);
    M5Dial.Display.setBrightness(BRIGHT[brightIdx]);
  }

  // Rafraîchissement périodique
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 250) {
    lastDraw = millis();
    draw();
  }

  // Relance du scan s'il s'est arrêté
  static uint32_t lastChk = 0;
  if (millis() - lastChk > 5000) {
    lastChk = millis();
    if (!NimBLEDevice::getScan()->isScanning()) startScan();
  }
}
