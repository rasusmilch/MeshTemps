# MeshTemps Bridge Node

This sketch is the headless mesh/Wi‑Fi root from `MeshTemps-RootNode` with the
LVGL display removed. It receives temps payloads from the leaf nodes, responds
to `root_probe`, and forwards each temps frame to the GUI node over UART.

## Serial link
- Baud: `BRIDGE_GUI_BAUD` (default 921600)
- Pins: `BRIDGE_GUI_TX_PIN` (default 17) -> GUI RX, `BRIDGE_GUI_RX_PIN` (default 18) <- GUI TX
- Format: newline-delimited JSON envelopes from `serial_protocol.h`

UART0 stays on the board's CH434 USB-UART for flashing and PC console access.
The dedicated GUI link lives on GPIO17/18 using `Serial1`, keeping the host
connection separate from the bridge/GUI traffic.

Flash this sketch to the ESP32-S3 that stays on Wi‑Fi/mesh duty.

## Console controls
- `wifi status|scan|connect|ssid|password|clear`
- `time now|sync` (forces NTP and pushes a `time_sync` envelope to the GUI)
