# MeshTemps GUI Node

This sketch hosts the LVGL/UI from `MeshTemps-RootNode` without any mesh or
Wi‑Fi activity. It listens on UART for serialized mesh frames from
`MeshTemps-BridgeNode` and renders them on the Waveshare LCD.

## Serial link
- Baud: `BRIDGE_GUI_BAUD` (default 921600)
- Pins (UART0 header): `BRIDGE_GUI_RX_PIN` (default 44 / U0RXD) <- Bridge TX,
  `BRIDGE_GUI_TX_PIN` (default 43 / U0TXD) -> Bridge RX
- Messages: newline-delimited JSON envelopes defined in `serial_protocol.h`
- Console/debug goes over the native USB port via the Arduino USB CDC setting.

Flash this sketch to the ESP32-S3 driving the display.

Time and timezone are injected by the bridge through `time_sync` envelopes, so
the GUI never brings up Wi‑Fi or NTP on its own.
