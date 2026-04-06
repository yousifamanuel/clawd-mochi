#!/usr/bin/env python3
"""CLI to send commands to Clawd Mochi via serial (USB).

Usage:
  mochi text Hello world     # type "Hello world" on the terminal screen
  mochi eyes                 # show normal eyes (w)
  mochi squish               # show squish eyes (s)
  mochi terminal             # switch to terminal view (d)
  mochi logo                 # show logo animation
  mochi quit                 # exit terminal mode (q)
  mochi bg #ff0000           # set background colour
  mochi speed 2              # set speed 1/2/3
  mochi status Thinking...   # show text below eyes (size 2)
  mochi status -s3 Klar!     # show with font size 3
  mochi status               # clear status text
  mochi img photo.jpg         # display image on screen
  mochi raw "thello"         # send raw serial command
  mochi ports                # list available serial ports
"""

import sys
import time
import serial
import serial.tools.list_ports


DEFAULT_BAUD = 115200
DISP_W = 240
DISP_H = 240


def find_port():
    """Find the first likely ESP32 serial port."""
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or "").lower()
        if any(k in desc for k in ("cp210", "ch340", "ch910", "usb serial", "usb-serial", "esp32", "jtag")):
            return p.device
    # fallback: first port that isn't COM1
    for p in ports:
        if p.device.upper() != "COM1":
            return p.device
    return None


def send_cmd(port, cmd, baud=DEFAULT_BAUD):
    """Open serial port, send command, read response."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.05)
    ser.write((cmd + "\n").encode())
    ser.flush()
    time.sleep(0.3)
    resp = ""
    while ser.in_waiting:
        resp += ser.read(ser.in_waiting).decode(errors="replace")
    ser.close()
    return resp.strip()


def send_image(port, image_path, tint=None, baud=DEFAULT_BAUD):
    """Read image, resize to display size, convert to RGB565, send over serial."""
    from PIL import Image
    img = Image.open(image_path).convert("RGB")
    img = img.resize((DISP_W, DISP_H), Image.LANCZOS)

    # Parse tint colour
    tr, tg, tb = 0, 0, 0
    if tint:
        tint = tint.lstrip("#")
        tr, tg, tb = int(tint[0:2], 16), int(tint[2:4], 16), int(tint[4:6], 16)

    # Convert to RGB565 little-endian (ESP32 native byte order)
    pixels = img.load()
    data = bytearray(DISP_W * DISP_H * 2)
    idx = 0
    for y in range(DISP_H):
        for x in range(DISP_W):
            r, g, b = pixels[x, y]
            if tint:
                # Blend: dark pixels → tint colour, bright pixels → white
                brightness = (r + g + b) / (3 * 255)
                r = int(tr + (255 - tr) * brightness)
                g = int(tg + (255 - tg) * brightness)
                b = int(tb + (255 - tb) * brightness)
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data[idx] = rgb565 & 0xFF
            data[idx + 1] = (rgb565 >> 8) & 0xFF
            idx += 2

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.05)

    # Send img command and wait for "ready"
    ser.write(b"img\n")
    ser.flush()
    time.sleep(0.5)
    resp = ""
    t0 = time.time()
    while time.time() - t0 < 3:
        if ser.in_waiting:
            resp += ser.read(ser.in_waiting).decode(errors="replace")
            if "ready" in resp:
                break
        time.sleep(0.05)
    if "ready" not in resp:
        print(f"Unexpected response: '{resp}'", file=sys.stderr)
        ser.close()
        sys.exit(1)

    # Send pixel data in chunks with pacing
    chunk = 128
    sent = 0
    total = len(data)
    while sent < total:
        n = min(chunk, total - sent)
        ser.write(data[sent:sent + n])
        sent += n
        time.sleep(0.002)
    ser.flush()

    # Wait for done
    resp = ""
    t0 = time.time()
    while time.time() - t0 < 20:
        if ser.in_waiting:
            resp += ser.read(ser.in_waiting).decode(errors="replace")
            if "done" in resp:
                break
        time.sleep(0.1)
    print(resp.strip() if resp else "transfer complete")
    ser.close()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)

    subcmd = sys.argv[1].lower()

    if subcmd == "ports":
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device:10s}  {p.description}")
        sys.exit(0)

    port = find_port()
    if not port:
        print("Error: no serial port found. Run 'mochi ports' to list.", file=sys.stderr)
        sys.exit(1)

    if subcmd == "text":
        text = " ".join(sys.argv[2:])
        cmd = "t" + text
    elif subcmd == "eyes":
        cmd = "w"
    elif subcmd == "squish":
        cmd = "s"
    elif subcmd == "terminal":
        cmd = "d"
    elif subcmd == "logo":
        cmd = "logo"
    elif subcmd == "quit":
        cmd = "q"
    elif subcmd == "bg":
        colour = sys.argv[2] if len(sys.argv) > 2 else "#000000"
        cmd = "bg" + colour
    elif subcmd == "speed":
        level = sys.argv[2] if len(sys.argv) > 2 else "2"
        cmd = "speed" + level
    elif subcmd == "status":
        size = "2"
        color = ""
        args = sys.argv[2:]
        if len(args) >= 2 and args[0].startswith("-s"):
            size = args[0][2:]
            args = args[1:]
        if len(args) >= 2 and args[0].startswith("-c"):
            color = " -c" + args[0][2:]
            args = args[1:]
        text = " ".join(args) if args else ""
        cmd = "status" + size + color + " " + text
    elif subcmd == "img":
        if len(sys.argv) < 3:
            print("Usage: mochi img [-t #RRGGBB] <image_file>", file=sys.stderr)
            sys.exit(1)
        args = sys.argv[2:]
        tint = None
        if args[0] == "-t" and len(args) >= 3:
            tint = args[1]
            args = args[2:]
        send_image(port, args[0], tint=tint)
        sys.exit(0)
    elif subcmd == "canvas":
        cmd = "canvas"
    elif subcmd == "line":
        # line x1 y1 x2 y2 #color
        cmd = "line " + ",".join(sys.argv[2:])
    elif subcmd == "raw":
        cmd = " ".join(sys.argv[2:])
    else:
        # treat everything as text if unknown command
        text = " ".join(sys.argv[1:])
        cmd = "t" + text

    resp = send_cmd(port, cmd)
    if resp:
        print(resp)


if __name__ == "__main__":
    main()
