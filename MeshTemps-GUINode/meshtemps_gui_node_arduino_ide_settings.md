# MeshTemps GUI Node — Arduino IDE Settings for Waveshare ESP32-S3-Touch-LCD-7

## Purpose

This file records the Arduino IDE settings that successfully built, uploaded, booted, and provided serial-console access for the MeshTemps GUI node on the Waveshare ESP32-S3-Touch-LCD-7.

This is based on the working configuration verified on the bench after resolving the serial-console confusion.

## Hardware

```text
Board: Waveshare ESP32-S3-Touch-LCD-7
Board revision: Rev 1.2
Project: MeshTemps-GUINode
Arduino IDE: 2.3.9
Serial console: USB
Board hardware UART switch position: UART2
```

## Critical findings

The important working combination is:

```text
USB CDC On Boot: Enabled
Serial console port: USB
Hardware UART switch: UART2
PSRAM: Enabled
```

The serial console is **not** on the board's UART switch path. The console is on the USB CDC serial interface.

The board's hardware UART switch must be set to:

```text
UART2
```

This matters because the MeshTemps GUI node uses a UART link to the bridge/root-side hardware. The PC operator console should remain on USB CDC, not on the board's switched UART path.

## Working Arduino IDE board selection

In Arduino IDE:

```text
Tools > Board > Waveshare ESP32-S3-Touch-LCD-7
```

Observed working board line:

```text
Board: "Waveshare ESP32-S3-Touch-LCD-7"
```

## Working Arduino IDE Tools settings

Use these settings under **Tools**:

| Arduino IDE option | Working setting |
|---|---|
| Board | `Waveshare ESP32-S3-Touch-LCD-7` |
| Port | `/dev/ttyACM0` or the detected USB CDC port |
| USB CDC On Boot | `Enabled` |
| CPU Frequency | `240MHz (WiFi)` |
| Core Debug Level | `None` |
| USB DFU On Boot | `Disabled` |
| Erase All Flash Before Sketch Upload | `Disabled` normally |
| Events Run On | `Core 1` |
| Flash Mode | `QIO 80MHz` |
| Flash Size | `16MB (128Mb)` |
| Arduino Runs On | `Core 1` |
| USB Firmware MSC On Boot | `Disabled` |
| Partition Scheme | `16M Flash (3MB APP / 9.9MB FATFS)` |
| PSRAM | `Enabled` |
| Upload Mode | `UART0 / Hardware CDC` |
| Upload Speed | `921600` |
| USB Mode | `Hardware CDC and JTAG` |

## Serial Monitor settings

Use Arduino Serial Monitor with:

```text
Port: USB CDC port, typically /dev/ttyACM0
Baud: 115200
Line ending: New Line
```

The working setup showed serial diagnostic output such as:

```text
[CHART_DIAG] ...
```

and the console input field worked from the USB serial console.

## Hardware UART switch

Set the physical UART switch on the Waveshare board to:

```text
UART2
```

This is required for the working MeshTemps GUI-node setup.

Do not use the board switch position as the operator console path. The operator serial console is on USB.

## Why this matters

The MeshTemps GUI node has two different serial roles:

```text
1. USB CDC serial:
   - PC operator console
   - Arduino Serial Monitor
   - commands such as help
   - diagnostic logs such as [CHART_DIAG]

2. Hardware UART link:
   - GUI node to bridge/root-side communication
   - controlled by the board's hardware UART switch path
```

If USB CDC is disabled or the wrong port is monitored, the board may still print ESP-IDF / panel logs, but MeshTemps console commands such as:

```text
help
```

may not respond.

The correct working setup is:

```text
PC console = USB CDC
Board UART switch = UART2
```

## Upload and monitor workflow

1. Set the board switch to:

```text
UART2
```

2. In Arduino IDE, select:

```text
Board: Waveshare ESP32-S3-Touch-LCD-7
Port: /dev/ttyACM0 or the detected USB CDC port
```

3. Confirm these settings:

```text
USB CDC On Boot: Enabled
PSRAM: Enabled
USB Mode: Hardware CDC and JTAG
Upload Mode: UART0 / Hardware CDC
Upload Speed: 921600
```

4. Click **Verify** or **Upload**.

5. Open Serial Monitor.

6. Set Serial Monitor to:

```text
115200 baud
New Line
```

7. Type:

```text
help
```

Expected result:

```text
The MeshTemps command list should print.
```

## MESHTEMPS_CHART_DIAG note

The chart diagnostic flag is controlled in:

```text
MeshTemps-GUINode/MeshTemps-GUINode.ino
```

Look for:

```cpp
#ifndef MESHTEMPS_CHART_DIAG
#define MESHTEMPS_CHART_DIAG 0
#endif
```

For chart diagnostic testing only:

```cpp
#ifndef MESHTEMPS_CHART_DIAG
#define MESHTEMPS_CHART_DIAG 1
#endif
```

After testing, return it to:

```cpp
#ifndef MESHTEMPS_CHART_DIAG
#define MESHTEMPS_CHART_DIAG 0
#endif
```

Do not leave chart diagnostics enabled during normal use unless actively collecting chart-freeze data. It produces substantial serial output and can affect timing.

## Confirming the serial path on Linux

Before plugging in the board:

```bash
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

Plug in the board, then run again:

```bash
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

The USB CDC console usually appears as:

```text
/dev/ttyACM0
```

For live kernel messages while plugging/unplugging:

```bash
dmesg -w
```

## Troubleshooting

### ESP-IDF panel logs appear, but MeshTemps commands do not respond

Check:

```text
USB CDC On Boot: Enabled
Serial Monitor port: USB CDC /dev/ttyACM0
Hardware switch: UART2
Serial Monitor baud: 115200
Line ending: New Line
```

Seeing ESP-IDF panel logs does not necessarily prove that the MeshTemps `Serial` console is routed correctly. The working command console is on USB CDC.

### No response to `help`

Check these in order:

1. Confirm the Serial Monitor is attached to the USB CDC port.
2. Confirm the board switch is on UART2.
3. Confirm USB CDC On Boot is Enabled.
4. Confirm the sketch was uploaded after changing settings.
5. Press reset and watch for MeshTemps boot messages.
6. Try closing and reopening Serial Monitor.

### Upload problems at 921600

If upload fails at:

```text
Upload Speed: 921600
```

try:

```text
460800
```

or:

```text
115200
```

Do not change the rest of the working settings unless diagnosing a specific upload issue.

### Board boots but display or LVGL behaves strangely

Check:

```text
PSRAM: Enabled
Flash Size: 16MB (128Mb)
Partition Scheme: 16M Flash (3MB APP / 9.9MB FATFS)
```

PSRAM must be enabled for this GUI/LVGL workload.

### Serial output looks garbled

Check:

```text
Serial Monitor baud: 115200
Line ending: New Line
Correct USB CDC port selected
```

Also confirm another terminal program is not already holding the same port.

## Known-good settings summary

Use this checklist before building or uploading MeshTemps-GUINode:

```text
Board: Waveshare ESP32-S3-Touch-LCD-7
Port: /dev/ttyACM0
USB CDC On Boot: Enabled
CPU Frequency: 240MHz (WiFi)
Core Debug Level: None
USB DFU On Boot: Disabled
Erase All Flash Before Sketch Upload: Disabled
Events Run On: Core 1
Flash Mode: QIO 80MHz
Flash Size: 16MB (128Mb)
Arduino Runs On: Core 1
USB Firmware MSC On Boot: Disabled
Partition Scheme: 16M Flash (3MB APP / 9.9MB FATFS)
PSRAM: Enabled
Upload Mode: UART0 / Hardware CDC
Upload Speed: 921600
USB Mode: Hardware CDC and JTAG
Hardware UART switch: UART2
Serial console: USB CDC
Serial Monitor baud: 115200
Serial Monitor line ending: New Line
```
