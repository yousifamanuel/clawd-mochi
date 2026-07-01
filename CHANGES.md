# Changes from original Clawd Mochi

## Overview

These changes add serial USB control, status text display, image transfer, and
other improvements to the original Clawd Mochi firmware. The `main` branch
targets a 280x240 (1.69") display; the `orig-240x240` branch keeps the
original 240x240 (1.54") display size.

---

## 1. Orange colour

```c
C_ORANGE = tft.color565(219, 99, 0);   // was (218, 17, 0)
```

Default background set to orange in `initColours()`:
```c
animBgColor = C_ORANGE;
drawBgColor = C_ORANGE;
```

## 2. Status text below eyes

Centered text shown below the eyes in any eye view. Automatically wraps to
multiple lines if the text is wider than the screen.

### Global variables
```c
String   statusText   = "";  // text shown below eyes
uint8_t  statusSize   = 2;   // font size (1-4)
uint16_t statusColor  = 0;   // 0 = black
```

`drawStatusText()` is called at the end of both `drawNormalEyes()` and
`drawSquishEyes()`.

## 3. Serial CLI commands

A serial command handler (`handleSerial()`) is added to `loop()` alongside the
existing `server.handleClient()`. Commands are sent over USB serial at
115200 baud, terminated by newline.

### Single-character commands
| Cmd | Action |
|-----|--------|
| `w` | Normal eyes with animation |
| `s` | Squish eyes with animation |
| `d` | Terminal / code view |
| `a` | Logo reveal animation |
| `q` | Exit terminal mode |

### Multi-character commands
| Command | Description |
|---------|-------------|
| `t<text>` | Type text in terminal (auto-enters terminal mode) |
| `bg#RRGGBB` | Set background colour, redraw current view |
| `speed1` / `speed2` / `speed3` | Set animation speed |
| `logo` | Show logo animation |
| `canvas` | Switch to draw mode |
| `line x1,y1,x2,y2,#RRGGBB` | Draw a 2px line on canvas |
| `img` | Receive raw RGB565 image (see below) |
| `status[N] [-c#RRGGBB] <text>` | Show text below eyes |

### Status command format
- `status2 Hello` -- size 2, default colour
- `status3 -c#ff0000 Error!` -- size 3, red
- `status` -- clear text

### Image transfer protocol
1. Send `img\n`
2. ESP32 responds `ready\n`
3. Send `DISP_W * DISP_H * 2` bytes of raw RGB565 (little-endian)
4. ESP32 responds `done <bytes>\n`

## 4. Startup behaviour

WiFi info screen is shown for 10 seconds, then automatically switches to
normal eyes (previously it stayed on the WiFi screen until a web request).

## 5. Display size (main branch only)

The `main` branch targets a 280x240 display:

| Setting | Original | Main branch |
|---------|----------|-------------|
| `DISP_W` | 240 | 280 |
| `tft.init()` | `(240, 240)` | `(240, 280)` |
| `LOGO_CX` | 120 | 140 |
| `TERM_COLS` | 15 | 18 |
| Canvas | 240x240 | 280x240 |
| Logo coords | Original | All X +20 |

The `orig-240x240` branch keeps all original display dimensions.

---

## Python CLI (mochi.py)

A companion CLI tool for sending commands via USB serial.

### Requirements
```
pip install pyserial Pillow
```

### Usage
```
mochi.py eyes                        # normal eyes
mochi.py squish                      # squish eyes
mochi.py logo                        # logo animation
mochi.py terminal                    # terminal view
mochi.py text Hello world            # type in terminal
mochi.py quit                        # exit terminal
mochi.py status Hello                # status text (size 2)
mochi.py status -s3 -c#ff0000 Error  # size 3, red
mochi.py status                      # clear status
mochi.py bg #ff0000                  # set background
mochi.py speed 2                     # animation speed
mochi.py canvas                      # draw mode
mochi.py line 10 10 100 100 #fff     # draw line
mochi.py img photo.jpg               # display image
mochi.py img -t "#db6300" logo.png   # display with tint
mochi.py raw "status3 test"          # raw serial command
mochi.py ports                       # list serial ports
```

### Serial port notes
- Auto-detects ESP32 serial port (CP210x, CH340, USB Serial, etc.)
- Opens port with DTR/RTS disabled to prevent ESP32 reset
- Image transfer uses 128-byte chunks with 2ms pacing
- RGB565 is sent in little-endian byte order
- Edit `DISP_W` / `DISP_H` at the top of the script to match your display

---

## Claude Code hooks

Example hooks for `~/.claude/settings.json` to show Mochi status while
Claude is working:

```json
{
  "hooks": {
    "UserPromptSubmit": [{
      "hooks": [{
        "type": "command",
        "command": "DIR=$(basename \"$PWD\") && python3 \"<path>/mochi.py\" squish && python3 \"<path>/mochi.py\" status -s2 -c#dddddd \"$DIR\"",
        "timeout": 10,
        "async": true
      }]
    }],
    "Stop": [{
      "hooks": [{
        "type": "command",
        "command": "DIR=$(basename \"$PWD\") && python3 \"<path>/mochi.py\" eyes && python3 \"<path>/mochi.py\" status -s2 -c#dddddd \"$DIR\"",
        "timeout": 10,
        "async": true
      }]
    }]
  }
}
```

Replace `<path>` with the actual path to `mochi.py`.
