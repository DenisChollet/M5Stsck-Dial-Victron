#pragma once

// ---------------------------------------------------------------------------
// Appareils Victron à lire.
//
// Clé de chiffrement : VictronConnect > appareil > Réglages > Info produit >
// "Afficher" les données de chiffrement. Activez aussi "Lecture instantanée
// via Bluetooth" dans les réglages de l'appareil.
//
// Adresse MAC : si vous ne la connaissez pas, laissez une valeur factice,
// flashez, ouvrez le moniteur série : les adresses des appareils Victron
// détectés y sont affichées.
// ---------------------------------------------------------------------------

struct DeviceCfg {
  const char* name;    // nom affiché (ASCII, 10 caractères max conseillés)
  const char* mac;     // "aa:bb:cc:dd:ee:ff"
  const char* keyHex;  // 32 caractères hexadécimaux
};

static const DeviceCfg DEVICES[] = {
    {"MPPT", "aa:bb:cc:dd:ee:01", "00112233445566778899aabbccddeeff"},
    {"BMV", "aa:bb:cc:dd:ee:02", "ffeeddccbbaa99887766554433221100"},
};

static constexpr size_t NUM_DEVICES = sizeof(DEVICES) / sizeof(DEVICES[0]);

// Délai sans réception avant d'afficher "Pas de signal" (ms)
static constexpr uint32_t STALE_MS = 15000;

// Crans de l'encodeur par cran mécanique (ajustez à 2 ou 4 selon le ressenti)
static constexpr int ENCODER_COUNTS_PER_DETENT = 4;
