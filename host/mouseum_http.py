#!/usr/bin/env python3
"""
mouseum_http.py — Python host driver for the mouseum-esp32 firmware.

The firmware exposes a JSON REST API over Wi-Fi.  By default the ESP32 runs
in Soft-AP mode at 192.168.4.1 — join the `mouseum` SSID first.

Usage:
    python mouseum_http.py status
    python mouseum_http.py move 50 20
    python mouseum_http.py human 300 100
    python mouseum_http.py click left
    python mouseum_http.py scroll -3
    python mouseum_http.py autowalk
    python mouseum_http.py demo            # scripted sequence

Library use:
    from mouseum_http import Mouseum
    m = Mouseum("http://192.168.4.1")
    m.move_human(300, 100); m.click()
"""
import argparse
import sys
import time

try:
    import requests
except ImportError:
    sys.stderr.write("This script requires `requests`.  pip install requests\n")
    raise


class Mouseum:
    def __init__(self, base="http://192.168.4.1", timeout=30):
        self.base = base.rstrip("/") + "/api/v1"
        self.timeout = timeout

    # --- low level
    def _post(self, path, body=None):
        r = requests.post(self.base + path,
                          json=body if body is not None else {},
                          timeout=self.timeout)
        r.raise_for_status()
        return r.json() if r.content else {}

    def _get(self, path):
        r = requests.get(self.base + path, timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    # --- high level
    def status(self):                  return self._get("/status")
    def help(self):                    return self._get("/help")
    def move(self, dx, dy):            return self._post("/move", {"dx": dx, "dy": dy})
    def move_human(self, dx, dy):      return self._post("/move_human", {"dx": dx, "dy": dy})
    def click(self, button="left"):    return self._post("/click", {"button": button})
    def buttons_down(self, mask):      return self._post("/buttons/down", {"mask": mask})
    def buttons_up(self, mask):        return self._post("/buttons/up", {"mask": mask})
    def release_all(self):             return self._post("/buttons/release_all")
    def wheel(self, delta):            return self._post("/wheel", {"delta": delta})
    def toggle_autowalk(self):         return self._post("/autowalk/toggle")


def _demo(m):
    print("status:", m.status())
    for dx, dy in [(300, 80), (-300, -80), (200, 200), (-200, -200)]:
        print("human", dx, dy)
        m.move_human(dx, dy)
        time.sleep(0.3)
    print("click left"); m.click("left")
    print("scroll");     m.wheel(-3); time.sleep(0.2); m.wheel(3)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://192.168.4.1",
                    help="firmware base URL (default: %(default)s)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    sub.add_parser("help")
    sub.add_parser("autowalk")
    sub.add_parser("release_all")
    sub.add_parser("demo")

    p = sub.add_parser("move");      p.add_argument("dx", type=int); p.add_argument("dy", type=int)
    p = sub.add_parser("human");     p.add_argument("dx", type=int); p.add_argument("dy", type=int)
    p = sub.add_parser("click");     p.add_argument("button", nargs="?", default="left",
                                                    choices=["left","right","middle"])
    p = sub.add_parser("scroll");    p.add_argument("delta", type=int)
    p = sub.add_parser("down");      p.add_argument("mask", type=int)
    p = sub.add_parser("up");        p.add_argument("mask", type=int)

    args = ap.parse_args(argv)
    m = Mouseum(args.base)

    if args.cmd == "status":      print(m.status())
    elif args.cmd == "help":      print(m.help())
    elif args.cmd == "autowalk":  print(m.toggle_autowalk())
    elif args.cmd == "release_all": print(m.release_all())
    elif args.cmd == "move":      print(m.move(args.dx, args.dy))
    elif args.cmd == "human":     print(m.move_human(args.dx, args.dy))
    elif args.cmd == "click":     print(m.click(args.button))
    elif args.cmd == "scroll":    print(m.wheel(args.delta))
    elif args.cmd == "down":      print(m.buttons_down(args.mask))
    elif args.cmd == "up":        print(m.buttons_up(args.mask))
    elif args.cmd == "demo":      _demo(m)


if __name__ == "__main__":
    main(sys.argv[1:])
