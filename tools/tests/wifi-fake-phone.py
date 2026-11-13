#!/usr/bin/env python3
"""Fake CarLife phone for the Wi-Fi receiver test: serves the six CarLife ports on 127.0.0.1, answers the handshake the
way the phone does over USB, sends video (SPS/PPS/IDR from a sample), one audio format and PCM record, waits for a key on
the touch port, then closes. Writes what it saw to the log file given as argv[1]; argv[2] is the H.264 sample."""
import socket, struct, sys, threading, time
log = open(sys.argv[1], "w", buffering=1)
h264 = open(sys.argv[2], "rb").read()
ports = {"cmd": 7240, "video": 8240, "audio": 9240, "tts": 9241, "vr": 9242, "touch": 9340}
listeners = {}
for name, port in ports.items():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port)); s.listen(1); listeners[name] = s
log.write("ready\n")
conn = {}
for name in ["cmd", "video", "audio", "tts", "vr", "touch"]:
    listeners[name].settimeout(15)
    c, addr = listeners[name].accept()
    conn[name] = c
    log.write("accepted %s from local port %d\n" % (name, addr[1]))

def cmd(type_, body=b""):
    return struct.pack(">HHI", len(body), 0, type_) + body
def media(type_, body):
    return struct.pack(">III", len(body), 0, type_) + body
def read_exact(c, n):
    d = b""
    while len(d) < n:
        x = c.recv(n - len(d))
        if not x: raise EOFError
        d += x
    return d
def read_cmd(c):
    h = read_exact(c, 8); n, _, t = struct.unpack(">HHI", h)
    return t, read_exact(c, n)

c = conn["cmd"]; c.settimeout(10)
t, b = read_cmd(c); log.write("hu cmd 0x%08x %s\n" % (t, b.hex()))
c.sendall(cmd(0x00010002, b"\x08\x01"))                       # VERSION_MATCH: accepted
seen = []
while 0x00018028 not in seen:
    t, b = read_cmd(c); seen.append(t); log.write("hu cmd 0x%08x %s\n" % (t, b.hex()))
c.sendall(cmd(0x00010008, b"\x08\x80\x0a\x10\xd0\x05\x18\x1e"))  # VIDEO_ENCODER_INIT_DONE 1280x720@30
while 0x0001801d not in seen:
    t, b = read_cmd(c); seen.append(t); log.write("hu cmd 0x%08x %s\n" % (t, b.hex()))
v = conn["video"]
# send the first record in two pieces to check reassembly across reads
rec = media(0x00020001, h264)
v.sendall(rec[:5]); time.sleep(0.05); v.sendall(rec[5:])
idr = h264[h264.index(b"\x00\x00\x01\x65") - 1:] if b"\x00\x00\x00\x01\x65" in h264 else h264
for i in range(5):
    v.sendall(media(0x00020001, h264)); time.sleep(0.03)
a = conn["audio"]
a.sendall(media(0x00030001, b"\x08\xc4\xd8\x02\x10\x02\x18\x10"))  # 44100 Hz, 2 channels, 16 bits
pcm = media(0x00030006, b"\x01\x02" * 1024)
a.sendall(pcm[:7]); time.sleep(0.02); a.sendall(pcm[7:])
conn["tts"].sendall(media(0x00040001, b"\x00" * 16))               # must be read and ignored
tc = conn["touch"]; tc.settimeout(8)
try:
    h = read_exact(tc, 8); n, _, t = struct.unpack(">HHI", h); body = read_exact(tc, n)
    log.write("touch 0x%08x %s\n" % (t, body.hex()))
except Exception as e:
    log.write("touch none (%s)\n" % e)
time.sleep(0.3)
c.close()
log.write("closed cmd\n")
time.sleep(1)
