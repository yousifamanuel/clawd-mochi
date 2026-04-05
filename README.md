<!-- LOGO -->
<p align="center">
  <img src="pics/clawd_mochi_banner.png" alt="Clawd Mochi Logo" width="700"/>
</p>

# Clawd Mochi 🦀🤖

A physical desk companion inspired by **Clawd** — the pixel-crab mascot of Claude Code by Anthropic. An ESP32-C3 drives a 1.54" color TFT display and shows you what Claude is doing at a glance.

**Built for the Seeed Studio XIAO ESP32-C3 · Cost: ~$8-10 · Build time: ~1 hour · Skill level: Beginner**

---

> This is an independent fan project. It is not affiliated with, sponsored by, or endorsed by Anthropic. "Claude" and "Clawd" are trademarks of Anthropic.

---

<p align="center">
  <img src="pics/clawd_mochi_3_4.jpeg" alt="Assembled Clawd Mochi on a desk" width="500"/>
  &nbsp;
  <img src="pics/clawd_mochi_claude_code.jpeg" alt="Claude Code view" width="500"/>
</p>

## What it does

Clawd Mochi sits next to your monitor, plugged in via USB, and acts as a **status indicator for Claude Code**. Using Claude Code hooks, it detects what Claude is doing and shows a matching facial expression:

| State | Expression | When |
|---|---|---|
| **Working** | Scanning eyes (L/R wiggle) | Claude is running tools or generating |
| **Attention** | Happy squish eyes (> <) with blink | Claude finished — needs your input |
| **Inactive** | Sleepy half-closed eyes + dim | No activity for a while |

### How it works

```
Claude Code ──hooks──▶ clawd_hook.sh ──socket──▶ clawd_daemon.py ──serial──▶ ESP32
```

1. **Claude Code hooks** fire on tool use, prompt submit, and response completion
2. A lightweight **hook script** sends the state to a background daemon via Unix socket
3. The **Python daemon** forwards commands to the ESP32 over USB serial
4. The **ESP32 firmware** animates the matching facial expression on the display

## Parts list

| Part | ~Cost |
|---|---|
| Seeed Studio XIAO ESP32-C3 | $4.99 |
| ST7789 1.54" TFT (240x240, SPI) | $3.00 |
| 8x jumper wires (8-10 cm) | $0.50 |
| 3D printed case | ~$1.00 |

**Total: ~$8-10**

## Wiring

| Display Pin | XIAO Pin | GPIO | Purpose |
|---|---|---|---|
| VCC | 3V3 | — | Power (3.3V ONLY) |
| GND | GND | — | Ground |
| SDA | D10 | GPIO 10 | MOSI (hardware SPI) |
| SCL | D8 | GPIO 8 | SCK (hardware SPI) |
| RES | D0 | GPIO 2 | Reset |
| DC | D3 | GPIO 5 | Data/Command |
| CS | D2 | GPIO 4 | Chip select |
| BL | D1 | GPIO 3 | Backlight (PWM) |

> **Note:** The XIAO ESP32-C3 does not expose GPIO 1 on its pin headers. DC is mapped to GPIO 5 (D3) instead.

## Setup

### 1. Flash the ESP32 firmware

**Requirements:** Arduino IDE 2.x

1. Add ESP32 board support:
   - File → Preferences → Additional Board URLs: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board Manager → Install "esp32 by Espressif Systems"

2. Install libraries (Library Manager):
   - Adafruit GFX Library
   - Adafruit ST7735 and ST7789 Library

3. Open `firmware/clawd_status/clawd_status.ino`

4. Board settings:
   - Board: **XIAO_ESP32C3** (Seeed Studio XIAO ESP32C3)
   - USB CDC On Boot: **Enabled**
   - CPU Frequency: **160 MHz**
   - Upload Speed: **921600**

5. Upload!

### 2. Install the daemon and hooks

```bash
# Clone and run the installer
cd hooks
./install.sh
```

This will:
- Copy the daemon and hook script to `~/.clawd/`
- Install `pyserial` if needed
- Create default config at `~/.clawd/config.json`
- Add hooks to `~/.claude/settings.json`
- Start the daemon via launchd (macOS)

### 3. Plug in and go

Connect the ESP32 via USB-C. The daemon auto-detects the serial port. Start using Claude Code — the expressions will change automatically!

## Configuration

Edit `~/.clawd/config.json`:

```json
{
  "serial_port": "auto",
  "baud_rate": 115200,
  "idle_timeout": 300,
  "brightness": 255,
  "speed": 2
}
```

| Setting | Description | Default |
|---|---|---|
| `serial_port` | `"auto"` or explicit path like `"/dev/cu.usbmodem1101"` | `"auto"` |
| `baud_rate` | Serial baud rate | `115200` |
| `idle_timeout` | Seconds before switching to inactive | `300` (5 min) |
| `brightness` | Display brightness (0-255) | `255` |
| `speed` | Animation speed (1=slow, 2=normal, 3=fast) | `2` |

After editing, send `reload` to the daemon:
```bash
echo "reload" | python3 -c "import socket,os; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect(os.path.expanduser('~/.clawd/clawd.sock')); s.sendall(b'reload\n'); s.close()"
```

## Serial protocol

You can test the ESP32 directly via any serial monitor at 115200 baud:

```
PING              → PONG
STATE:working     → OK:STATE:working
STATE:attention   → OK:STATE:attention
STATE:inactive    → OK:STATE:inactive
SET:brightness:128 → OK:SET:brightness:128
SET:speed:3       → OK:SET:speed:3
SET:bgcolor:DA1100 → OK:SET:bgcolor:DA1100
```

## Uninstall

```bash
cd hooks
./install.sh --uninstall
```

## Project structure

```
clawd-mochi/
├── firmware/clawd_status/    # ESP32 Arduino firmware
│   └── clawd_status.ino
├── daemon/                   # Host-side Python daemon
│   ├── clawd_daemon.py
│   ├── requirements.txt
│   └── com.clawd.status-daemon.plist
├── hooks/                    # Claude Code hook script + installer
│   ├── clawd_hook.sh
│   └── install.sh
├── clawd_mochi.ino           # Original WiFi version (preserved)
├── models/                   # 3D printable case files
└── pics/                     # Project photos
```

## Original version

The original `clawd_mochi.ino` in the root is the standalone WiFi-controlled version with a web interface, terminal, drawing canvas, and multiple expression views. It still works independently — just flash it instead of the status indicator firmware.

## 3D printing

The same case from [MakerWorld](https://makerworld.com/en/models/2559505-clawd-mochi-physical-claude-code-mascot#profileId-2820000) works for both versions. Print in PLA/PETG, ~30g of filament.

## License

- **Code:** MIT License
- **3D models and media assets:** CC BY-NC-SA 4.0
