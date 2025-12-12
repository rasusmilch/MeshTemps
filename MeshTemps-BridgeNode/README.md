# MeshTemps Bridge Node

This sketch is the headless mesh/Wi‑Fi root from `MeshTemps-RootNode` with the
LVGL display removed. It receives temps payloads from the leaf nodes, responds
to `root_probe`, and forwards each temps frame to the GUI node over UART.

## Serial link
- Baud: `BRIDGE_GUI_BAUD` (default 115200)
- Pins: `BRIDGE_GUI_TX_PIN` (default 17) -> GUI RX, `BRIDGE_GUI_RX_PIN` (default 18) <- GUI TX
- Format: newline-delimited JSON envelopes from `serial_protocol.h`

UART0 stays on the board's CH434 USB-UART for flashing and PC console access.
The dedicated GUI link lives on GPIO17/18 using `Serial1`, keeping the host
connection separate from the bridge/GUI traffic.

`bridge_status` envelopes now include the current node list plus `lastSeenMs`
per node. If you see `connections` briefly flip between 0 and 1 while temps
still stream, the mesh library is expiring an idle link and re-establishing it
on the next payload; the `lastSeenMs` field shows that the leaf is still
talking even if the mesh connection list bounces.

Flash this sketch to the ESP32-S3 that stays on Wi‑Fi/mesh duty.

## Console controls
- `wifi status|scan|connect|ssid|password|clear`
- `time now|sync` (forces NTP and pushes a `time_sync` envelope to the GUI)
- `debug on|off` to mirror mesh/Wi‑Fi/NTP events to the PC console
- `passthru on|off` to mirror the GUI link (GPIO17/18) to the PC console for
  bridge-link debugging
