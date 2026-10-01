#pragma once
#include <stdint.h>
#include <math.h>

// Change these for EACH board. IDs must be unique, nonzero.
constexpr uint16_t NODE_ID = 3;
constexpr uint16_t ROOT_ID = 2;
constexpr uint16_t ROOM_ID = 2;
constexpr uint32_t NETWORK_ID = 0x4C414231;
constexpr uint8_t RADIO_CHANNEL = 6;
constexpr bool RELAY_ALLOWED = true;

// ETX recommended until per-link energy has been measured. All nodes must match.
enum Metric : uint8_t { ETX = 1, ENERGY = 2 };
constexpr Metric ROUTING_METRIC = ETX;
// Return measured total per-attempt communication energy (mJ) for i -> next.
// Include your chosen TX/RX/ACK/timeout scope; NAN means not calibrated.
inline float attemptEnergyMj(uint16_t next) {
  (void)next;
  // Example syntax ONLY: if (NODE_ID == 3 && next == 1) return measured_value;
  return NAN;
}

// Actual node 1 wiring from sensor_detector.ino. Check other boards individually!
#ifndef ENABLE_SENSORS
#define ENABLE_SENSORS 1
#endif
// Bit 1=DHT11, bit 2=BH1750, bit 4=PIR. Example: DHT+PIR => 5.
#ifndef SENSOR_MASK
#define SENSOR_MASK 7
#endif
constexpr int DHT_PIN = 5;
constexpr int PIR_PIN = 15;
constexpr int SDA_PIN = 8;
constexpr int SCL_PIN = 9;
constexpr uint32_t SAMPLE_MS = 5000;

constexpr unsigned MAX_NEIGHBORS = 8;
constexpr unsigned MAX_PATH = 8; // max nodes in advertised path, including root
constexpr unsigned OUTBOX_SIZE = 16;
constexpr uint32_t ACK_TIMEOUT_MS = 500;
constexpr unsigned MAX_ATTEMPTS = 3;
constexpr uint32_t DATA_TTL_MS = 120000;
constexpr uint32_t PROBE_MS = 2000; // round robin: 8 neighbors => ~16s per neighbor
constexpr uint32_t VERIFIED_TTL_MS = 45000;
constexpr uint32_t ADV_MIN_MS = 2000;
constexpr uint32_t ADV_MAX_MS = 32000;
constexpr uint32_t ROUTE_TTL_MS = 100000;
constexpr uint32_t EVALUATE_MS = 5000;
constexpr float MIN_Q = 0.25f;
constexpr float MIN_RSSI = -90.0f; // experiment parameter, not a universal threshold
constexpr float SWITCH_RATIO = 0.85f;
constexpr unsigned SWITCH_CONFIRMATIONS = 3;
static_assert(NODE_ID && ROOT_ID, "IDs must be nonzero");
static_assert(MAX_PATH <= 8 && MAX_NEIGHBORS <= 8, "Update wire format/capacity first");
