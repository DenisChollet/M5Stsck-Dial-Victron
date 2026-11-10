#pragma once
#include <Arduino.h>
#include <string>
#include <math.h>
#include "mbedtls/aes.h"

// Décodage des publicités BLE "Instant Readout" de Victron.
// Format des données constructeur (après l'identifiant 0x02E1) :
//   [0]=0x10  [1..2]=modèle  [3]=type  [4..5]=IV (compteur)  [6]=1er octet clé
//   [7..]=données chiffrées AES-128-CTR (bloc initial = IV en little-endian)

namespace victron {

constexpr uint16_t MANUFACTURER_ID = 0x02E1;

enum class Type : uint8_t {
  Unknown = 0,
  SolarCharger = 0x01,
  BatteryMonitor = 0x02,
};

struct SolarData {
  uint8_t state = 0xFF;
  uint8_t error = 0;
  float batteryV = NAN;
  float batteryA = NAN;
  float yieldWh = NAN;
  float pvW = NAN;
  float loadA = NAN;
};

struct BatteryData {
  float voltage = NAN;
  float current = NAN;     // A (négatif = décharge)
  float soc = NAN;         // %
  float consumedAh = NAN;  // Ah (négatif = consommés)
  int32_t ttgMin = -1;     // minutes, -1 = inconnu
  float auxV = NAN;        // tension auxiliaire (démarrage / point milieu)
  float tempC = NAN;       // température si l'entrée aux est une sonde
  uint16_t alarm = 0;
};

struct Reading {
  Type type = Type::Unknown;
  uint16_t model = 0;
  SolarData solar;
  BatteryData batt;
};

inline bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  if (strlen(hex) != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char* end;
    long v = strtol(b, &end, 16);
    if (*end != 0) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

inline bool decrypt(const uint8_t key[16], uint16_t iv, const uint8_t* in,
                    size_t len, uint8_t* out) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }
  uint8_t counter[16] = {0};
  counter[0] = iv & 0xFF;
  counter[1] = iv >> 8;
  uint8_t stream[16];
  size_t off = 0;
  int rc = mbedtls_aes_crypt_ctr(&ctx, len, &off, counter, stream, in, out);
  mbedtls_aes_free(&ctx);
  return rc == 0;
}

inline uint64_t readLE(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

inline void parseSolar(const uint8_t* d, SolarData& o) {  // 12 octets
  o.state = d[0];
  o.error = d[1];
  int16_t v = (int16_t)(d[2] | d[3] << 8);
  int16_t a = (int16_t)(d[4] | d[5] << 8);
  uint16_t y = d[6] | d[7] << 8;
  uint16_t pv = d[8] | d[9] << 8;
  uint16_t load = (d[10] | d[11] << 8) & 0x1FF;
  o.batteryV = (v == 0x7FFF) ? NAN : v * 0.01f;
  o.batteryA = (a == 0x7FFF) ? NAN : a * 0.1f;
  o.yieldWh = (y == 0xFFFF) ? NAN : y * 10.0f;
  o.pvW = (pv == 0xFFFF) ? NAN : (float)pv;
  o.loadA = (load == 0x1FF) ? NAN : load * 0.1f;
}

inline void parseBattery(const uint8_t* d, BatteryData& o) {  // 15 octets
  uint16_t ttg = d[0] | d[1] << 8;
  int16_t v = (int16_t)(d[2] | d[3] << 8);
  o.alarm = d[4] | d[5] << 8;
  uint16_t aux = d[6] | d[7] << 8;
  uint64_t bits = readLE(d + 8, 7);

  uint8_t auxMode = bits & 0x3;
  int32_t cur = (int32_t)((bits >> 2) & 0x3FFFFF);
  if (cur & 0x200000) cur -= (1 << 22);  // extension de signe 22 bits
  uint32_t cons = (uint32_t)((bits >> 24) & 0xFFFFF);
  uint16_t soc = (uint16_t)((bits >> 44) & 0x3FF);

  o.ttgMin = (ttg == 0xFFFF) ? -1 : (int32_t)ttg;
  o.voltage = (v == 0x7FFF) ? NAN : v * 0.01f;
  o.current = (cur == 0x1FFFFF) ? NAN : cur * 0.001f;
  o.consumedAh = (cons == 0xFFFFF) ? NAN : -(float)cons * 0.1f;
  o.soc = (soc == 0x3FF) ? NAN : soc * 0.1f;

  o.auxV = NAN;
  o.tempC = NAN;
  if (auxMode == 0 || auxMode == 1) {
    o.auxV = aux * 0.01f;
  } else if (auxMode == 2) {
    o.tempC = aux * 0.01f - 273.15f;
  }
}

// Décode une trame complète (std::string = données constructeur BLE, avec
// l'identifiant constructeur en tête). Retourne false si trame non reconnue
// ou clé incorrecte.
inline bool decode(const std::string& md, const uint8_t key[16], Reading& r) {
  if (md.size() < 10) return false;
  const uint8_t* p = (const uint8_t*)md.data();
  if (p[0] != (MANUFACTURER_ID & 0xFF) || p[1] != (MANUFACTURER_ID >> 8))
    return false;
  if (p[2] != 0x10) return false;

  uint16_t model = p[3] | p[4] << 8;
  uint8_t type = p[5];
  uint16_t iv = p[6] | p[7] << 8;
  if (p[8] != key[0]) return false;  // vérification rapide de la clé

  size_t len = md.size() - 9;
  if (len > 16) len = 16;
  uint8_t plain[16];
  if (!decrypt(key, iv, p + 9, len, plain)) return false;

  r.model = model;
  if (type == (uint8_t)Type::SolarCharger && len >= 12) {
    r.type = Type::SolarCharger;
    parseSolar(plain, r.solar);
    return true;
  }
  if (type == (uint8_t)Type::BatteryMonitor && len >= 15) {
    r.type = Type::BatteryMonitor;
    parseBattery(plain, r.batt);
    return true;
  }
  return false;  // autres types non gérés ici
}

}  // namespace victron
