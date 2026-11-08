#!/usr/bin/env python3
"""Record the microphone of a board running rcplayer (the LCDWIKI 2.8" + Teensy Audio Shield),
save it as a WAV, print what the board measured, and play it back on the board.

  tools/rec.py [--seconds 5] [--out rec.wav] [--no-play] [--port P]

The board records into PSRAM ("rec N"), measures it (DC, RMS, peak, clipping, noise floor),
sends it as base64 16-bit mono ("wav"), and replays it on its speaker ("play").
The port is opened like tools/snap.py: raw, without touching the modem lines.
"""
import argparse, base64, glob, os, struct, sys, termios, time, wave

ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=float, default=5)
ap.add_argument("--out", default="rec.wav")
ap.add_argument("--no-play", action="store_true")
ap.add_argument("--port", default=None)
a = ap.parse_args()
port = a.port or (sorted(glob.glob("/dev/cu.usbmodem*")) or [None])[0]
if not port: sys.exit("no /dev/cu.usbmodem* port")

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[0] = t[1] = 0
t[2] = (t[2] & ~(termios.HUPCL | termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)) \
    | termios.CS8 | termios.CREAD | termios.CLOCAL
t[3] &= ~(termios.ICANON | termios.ECHO | termios.ISIG | termios.IEXTEN)
termios.tcsetattr(fd, termios.TCSANOW, t)
buf = b""

def read():
    try: return os.read(fd, 65536)
    except BlockingIOError: return b""

def lines(timeout):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        if b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode(errors="replace").strip()
            continue
        c = read()
        if c: buf += c
        else: time.sleep(0.005)

while read(): pass
for i in (3, 2, 1):
    print(f"recording in {i}..."); time.sleep(1)
print(f">>> SPEAK NOW ({a.seconds:g} s) <<<")
os.write(fd, f"rec {a.seconds}\n".encode())
for line in lines(a.seconds + 10):
    if line.startswith("rec:"):
        if line == "rec: done": print(">>> stop <<<")
        elif not line.startswith("rec: recording"): print(line)
        if "noise floor" in line or "failed" in line: break

os.write(fd, b"wav\n")
rate = n = None; data = bytearray()
for line in lines(120):
    if line.startswith("WAV BEGIN"):
        _, _, rate, n = line.split(); rate, n = int(rate), int(n)
    elif line == "WAV END": break
    elif rate is not None and line and not line.startswith(("I (", "W (", "E (")):
        try: data += base64.b64decode(line)
        except Exception: pass
if rate is None: sys.exit("no recording came back")
with wave.open(a.out, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(data[: n * 2]))
print(f"saved {a.out}: {len(data) // 2} of {n} samples at {rate} Hz")

if not a.no_play:
    print("playing it back on the board...")
    os.write(fd, b"play\n")
    for line in lines(a.seconds + 5):
        if line == "play: done": break
