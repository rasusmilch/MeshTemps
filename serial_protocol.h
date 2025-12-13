#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Shared serial protocol between the headless mesh bridge (root) and the
// GUI/LCD consumer. Messages are newline-delimited JSON documents.
//
// Envelope shape:
//   {
//     "type": "mesh_frame" | "bridge_hello" | "bridge_status" |
//             "time_sync" | "time_request",
//     ...
//   }
//
// "mesh_frame" messages carry the original mesh payload in the "payload"
// field and metadata about when/where it was observed.
//
// "time_sync" messages carry UTC epoch seconds and timezone metadata from the
// bridge so the GUI node can set its RTC without running Wi‑Fi/NTP locally.
//
// "bridge_status" messages report mesh connectivity. The bridge emits an
// optional `reason` (e.g., "mesh_event" or "boot"), a `connections` count, and
// a `nodes` array of the currently connected mesh node IDs with optional
// `lastSeenMs` latency derived from the most recent payload observed from each
// node.
//
// "time_request" messages are sent from the GUI to the bridge to ask for the
// current epoch/timezone and optionally force an NTP sync on the bridge even if
// its periodic resync timer has not fired yet.
//
// Pins/baud can be overridden before including this header.
#ifndef BRIDGE_GUI_BAUD
#define BRIDGE_GUI_BAUD 115200
#endif

#ifndef BRIDGE_GUI_TX_PIN
#define BRIDGE_GUI_TX_PIN 17
#endif

#ifndef BRIDGE_GUI_RX_PIN
#define BRIDGE_GUI_RX_PIN 18
#endif

// Helper to emit a JsonDocument as a single line on the provided serial port.
inline void SendJsonLine(HardwareSerial &port, const JsonDocument &doc) {
  String line;
  serializeJson(doc, line);
  port.println(line);
}
