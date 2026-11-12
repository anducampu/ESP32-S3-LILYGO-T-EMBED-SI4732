#!/usr/bin/env python3
"""
webui_bridge.py -- PC-side server for the T-Embed SI4732 web UI.

Serves web/ and exposes the HTTP API the ESP32 will serve once the UI is ported,
so the page itself does not change between the two:

    GET  /api/bands   band / step / bandwidth table (web/bands.json)
    GET  /api/state   current receiver state as JSON
    POST /api/cmd     {"c": <command>, "v": <value>}, commands listed in CMDS_OK
    GET  /events      Server-Sent Events:
                        event "state"  full state, on every change
                        event "rds"    {"e": epoch, "reset": bool, "g": [groups]}
                                       raw RDS groups "AAAABBBBCCCCDDDDEE"; the page
                                       decodes them (web/rds.js). "reset" opens a
                                       new station and carries all groups so far.

Behind it, the radio is driven over CAT on the USB port, so the device must have
**CAT USB** switched on in its menu: Kenwood TS-480 for frequency / mode / volume /
mute, plus this project's ZZ extensions (ZZRS ZZRG ZZBD ZZST ZZBW ZZAG ZZSM ZZSK,
see CatControl.h) for everything else.

Usage:
    python3 tools/webui_bridge.py                 # http://localhost:8073
    python3 tools/webui_bridge.py --host 0.0.0.0  # reachable from a phone on the LAN
    python3 tools/webui_bridge.py --port-dev /dev/ttyACM0
"""

import argparse
import json
import os
import queue
import threading
import time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

import serial
import serial.tools.list_ports

USB_VID, USB_PID = 0x303A, 0x1001
WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")
POLL_S = 0.2
RDS_KEEP = 4000          # groups kept per station (~6 min) for late-joining pages

with open(os.path.join(WEB_DIR, "bands.json")) as _fh:
    BT = json.load(_fh)
BANDS = BT["bands"]

KW_TO_MODE = {"1": "LSB", "2": "USB", "3": "USB", "4": "FM", "5": "AM"}
MODE_TO_KW = {"LSB": "1", "USB": "2", "FM": "4", "AM": "5"}
CMDS_OK = ("freq", "band", "mode", "vol", "mute", "step", "bw", "agc", "sm", "seek")


# --------------------------------------------------------------------------
# Shared state
# --------------------------------------------------------------------------
class State:
    def __init__(self):
        self.cond = threading.Condition()
        self.ver = 0
        self.d = {"conn": False, "port": None, "err": None}
        # RDS for the current station: groups since the epoch began. base is the
        # absolute index of rds[0], so readers keep a cursor across trimming.
        self.rds_epoch = None
        self.rds = []
        self.rds_base = 0

    def update(self, **kw):
        with self.cond:
            if any(self.d.get(k) != v for k, v in kw.items()):
                self.d.update(kw)
                self.ver += 1
                self.cond.notify_all()

    def rds_add(self, epoch, groups):
        with self.cond:
            if epoch != self.rds_epoch:
                self.rds_epoch, self.rds, self.rds_base = epoch, [], 0
            elif not groups:
                return
            self.rds.extend(groups)
            if len(self.rds) > RDS_KEEP:
                cut = len(self.rds) - RDS_KEEP
                del self.rds[:cut]
                self.rds_base += cut
            self.ver += 1
            self.cond.notify_all()

    def snapshot(self):
        with self.cond:
            return dict(self.d, ver=self.ver)

    def wait_change(self, since, timeout):
        with self.cond:
            self.cond.wait_for(lambda: self.ver != since, timeout=timeout)
            return self.ver


STATE = State()
CMDS = queue.Queue()


# --------------------------------------------------------------------------
# Serial worker: the only thread that touches the port
# --------------------------------------------------------------------------
def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == USB_VID and p.pid == USB_PID:
            return p.device
    return None


def parse_kv(s):
    out = {}
    for tok in s.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


class Radio(threading.Thread):
    def __init__(self, dev=None):
        super().__init__(daemon=True)
        self.dev_arg = dev
        self.ser = None
        self.buf = b""

    def open(self):
        dev = self.dev_arg or find_port()
        if not dev:
            raise serial.SerialException("no device with USB ID 303a:1001")
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = dev, 115200, 0.05
        # RTS asserted with DTR released is the USB-Serial-JTAG reset sequence;
        # keep RTS low so opening the port never reboots the radio.
        s.rts, s.dtr = False, True
        s.open()
        time.sleep(0.2)
        s.reset_input_buffer()
        self.ser, self.buf = s, b""
        return dev

    def transact(self, cmd, expect, timeout=0.6):
        """Write cmd, then collect ';'-terminated frames until every prefix in
        `expect` has been seen. Returns {prefix: payload}."""
        self.ser.write(cmd.encode())
        got, end = {}, time.monotonic() + timeout
        want = list(expect)
        while want and time.monotonic() < end:
            self.buf += self.ser.read(1024)
            while b";" in self.buf:
                frame, self.buf = self.buf.split(b";", 1)
                f = frame.decode("ascii", "replace").strip()
                # Longest prefix first: "ZZRS" must not be taken for something shorter.
                for p in sorted(want, key=len, reverse=True):
                    if f.startswith(p):
                        got[p] = f[len(p):]
                        want.remove(p)
                        break
        return got

    def poll(self):
        r = self.transact("IF;SM0;AG0;MU;ZZRS;ZZRG;", ["IF", "SM0", "AG0", "MU", "ZZRS", "ZZRG"])
        if "IF" not in r:
            raise TimeoutError("no IF; answer -- is CAT USB switched on?")
        p = r["IF"]
        if len(p) < 35:
            return
        freq = int(p[0:11])
        mode = KW_TO_MODE.get(p[27], "?")
        upd = {"freq": freq, "mode": mode, "err": None}
        if "AG0" in r and r["AG0"].isdigit():
            ag = int(r["AG0"])
            upd.update(vol=(ag * 63 + 127) // 255, volPct=round(ag * 100 / 255))
        if "MU" in r:
            upd["mute"] = r["MU"] == "1"
        if "ZZRS" in r:
            k = parse_kv(r["ZZRS"])
            try:
                upd.update(
                    band=int(k["b"]), stepIdx=int(k["st"]), bwIdx=int(k["bw"]),
                    agc=int(k["agc"]), sm=int(k["sm"]), bfo=int(k["bfo"]),
                    dbuv=int(k["rssi"]), snr=int(k["snr"]), pilot=k["pil"] == "1",
                    blend=int(k["bl"]), multipath=int(k["mp"]), freqOff=int(k["off"]),
                    seeking=k["sk"] == "1", ssbLoaded=k["ssb"] == "1",
                )
            except (KeyError, ValueError):
                upd["err"] = "old firmware: no ZZRS -- flash the current sketch"
        elif "SM0" in r and r["SM0"].isdigit():
            upd["dbuv"] = int(r["SM0"]) * 4 - 20     # pre-ZZ firmware fallback
        STATE.update(**upd)
        if "ZZRG" in r:
            parts = r["ZZRG"].split()
            k = parse_kv(" ".join(parts[:3]))
            groups = [g for g in parts[3:] if len(g) == 18]
            if "e" in k:
                STATE.rds_add(int(k["e"]), groups)

    def apply(self, c, v):
        w = self.ser.write
        if c == "freq":
            w(f"FA{int(v):011d};".encode())
        elif c == "band":
            w(f"ZZBD{int(v):02d};".encode())
        elif c == "mode":
            w(f"MD{MODE_TO_KW[v]};".encode())
        elif c == "vol":
            vol = max(0, min(63, int(v)))
            w(f"AG0{(vol * 255 + 31) // 63:03d};".encode())
        elif c == "mute":
            w(f"MU{1 if v else 0};".encode())
        elif c == "step":
            w(f"ZZST{int(v)};".encode())
        elif c == "bw":
            w(f"ZZBW{int(v)};".encode())
        elif c == "agc":
            w(f"ZZAG{int(v):02d};".encode())
        elif c == "sm":
            w(f"ZZSM{int(v):02d};".encode())
        elif c == "seek":
            w({"up": b"ZZSKU;", "down": b"ZZSKD;"}.get(v, b"ZZSK0;"))

    def run(self):
        while True:
            try:
                dev = self.open()
                STATE.update(conn=True, port=dev, err=None)
                next_poll = 0.0
                while True:
                    try:
                        c, v = CMDS.get(timeout=max(0.0, next_poll - time.monotonic()))
                        self.apply(c, v)
                        # Coalesce a burst (a fast wheel spin) before reading back.
                        while not CMDS.empty():
                            self.apply(*CMDS.get_nowait())
                        next_poll = 0.0
                        continue
                    except queue.Empty:
                        pass
                    self.poll()
                    next_poll = time.monotonic() + POLL_S
            except (serial.SerialException, OSError, TimeoutError) as e:
                STATE.update(conn=False, err=str(e))
                try:
                    if self.ser:
                        self.ser.close()
                except Exception:
                    pass
                self.ser = None
                time.sleep(2)


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------
MIME = {".html": "text/html; charset=utf-8", ".css": "text/css", ".js": "text/javascript",
        ".svg": "image/svg+xml", ".json": "application/json"}


def validate(c, v):
    if c not in CMDS_OK:
        raise ValueError(f"unknown command {c!r}")
    if c == "mode" and v not in MODE_TO_KW:
        raise ValueError("mode must be AM, LSB, USB or FM")
    if c == "seek" and v not in ("up", "down", "stop"):
        raise ValueError("seek must be up, down or stop")
    lim = {"band": len(BANDS) - 1, "step": 9, "bw": 9, "agc": 35, "sm": 32, "vol": 63,
           "freq": 200_000_000}
    if c in lim and not (0 <= int(v) <= lim[c]):
        raise ValueError(f"{c} out of range")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/bands":
            # Re-read per request, so editing bands.json needs no bridge restart.
            with open(os.path.join(WEB_DIR, "bands.json")) as fh:
                return self._json(json.load(fh))
        if path == "/api/state":
            return self._json(STATE.snapshot())
        if path == "/api/rds":
            with STATE.cond:
                return self._json({"e": STATE.rds_epoch, "g": list(STATE.rds)})
        if path == "/events":
            return self._events()
        if path == "/":
            path = "/index.html"
        root = os.path.normpath(WEB_DIR)
        f = os.path.normpath(os.path.join(root, path.lstrip("/")))
        if not f.startswith(root + os.sep) or not os.path.isfile(f):
            return self._json({"error": "not found"}, 404)
        with open(f, "rb") as fh:
            body = fh.read()
        self.send_response(200)
        self.send_header("Content-Type", MIME.get(os.path.splitext(f)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send(self, event, obj):
        self.wfile.write(f"event: {event}\ndata: {json.dumps(obj, separators=(',', ':'))}\n\n".encode())

    def _events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        ver, state_sent, rds_epoch, rds_pos = -1, None, object(), 0
        try:
            while True:
                new = STATE.wait_change(ver, timeout=15)
                if new == ver:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    continue
                ver = new
                with STATE.cond:
                    snap = dict(STATE.d)
                    ep, base, groups = STATE.rds_epoch, STATE.rds_base, STATE.rds
                    if ep != rds_epoch:
                        out, reset = list(groups), True
                    else:
                        out, reset = groups[max(0, rds_pos - base):], False
                    rds_epoch, rds_pos = ep, base + len(groups)
                if snap != state_sent:
                    self._send("state", snap)
                    state_sent = snap
                if reset or out:
                    self._send("rds", {"e": ep, "reset": reset, "g": out})
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        if self.path != "/api/cmd":
            return self._json({"error": "not found"}, 404)
        try:
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n) or b"{}")
            c, v = req["c"], req.get("v")
            validate(c, v)
        except (ValueError, KeyError, TypeError, json.JSONDecodeError) as e:
            return self._json({"ok": False, "error": str(e)}, 400)
        if not STATE.d["conn"]:
            return self._json({"ok": False, "error": "radio not connected"}, 503)
        CMDS.put((c, v))
        self._json({"ok": True})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--http-port", type=int, default=8073)
    ap.add_argument("--port-dev", help="serial device (default: find USB ID 303a:1001)")
    a = ap.parse_args()
    Radio(a.port_dev).start()
    srv = ThreadingHTTPServer((a.host, a.http_port), Handler)
    srv.daemon_threads = True
    shown = "localhost" if a.host in ("127.0.0.1", "0.0.0.0") else a.host
    print(f"T-Embed web UI: http://{shown}:{a.http_port}   (Ctrl+C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
