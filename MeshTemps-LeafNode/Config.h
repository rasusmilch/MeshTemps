#pragma once
#ifndef DBG_BAUD
#define DBG_BAUD 9600
#endif

// Set this to your LED pin if you have one (common ESP32-S3 devkits use 38 or
// 48; else -1 to disable)
#ifndef DBG_LED_PIN
#define DBG_LED_PIN -1
#endif

// ===== Mesh credentials (routerless) =====
#define MESH_PREFIX "MeshTemps"
#define MESH_PASSWORD "mesh-pass-1234" // >= 8 chars
#define MESH_PORT 5555
#define MESH_CHANNEL 6
#define MESH_HIDDEN false

// ===== Leaf DS18B20 settings =====
#ifndef ONEWIRE_PIN
#define ONEWIRE_PIN 4
#endif
#ifndef SEND_PERIOD_MS
#define SEND_PERIOD_MS 10000UL
#endif

// ===== Root broadcast cadence =====
#ifndef ROOT_ANNOUNCE_MS
#define ROOT_ANNOUNCE_MS 20000UL
#endif

// ===== ArduinoJson v7 capacities =====
#define JSON_SMALL 512
#define JSON_MED 1024
#define JSON_BIG 4096

inline String addrToHex(const uint8_t *addr) {
  static const char *H = "0123456789ABCDEF";
  char s[17] = {0};
  for (int i = 0; i < 8; ++i) {
    s[i * 2] = H[(addr[i] >> 4) & 0xF];
    s[i * 2 + 1] = H[(addr[i]) & 0xF];
  }
  return String(s);
}
