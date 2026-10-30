#!/usr/bin/env python3
"""Viewport session-tick proof (node editor OFF).

The viewport free-runs its frame counter, so tools/live_proof.py g3's
frame-0 contract cannot run against it. This script is the relative-frame
equivalent: pause the visual host, record a clip through `step`-released
frames, and verify samples land plus playback applies -- which only
happens if the session actually ticks on the visual host.

Usage: vp_tick_check.py <socket-path>
Exit 0 when every check passes, 1 otherwise.
"""

import json
import socket
import sys
import time

PASS = []
FAIL = []


def check(name, cond, extra=""):
    (PASS if cond else FAIL).append(name)
    print(f"  {'PASS' if cond else 'FAIL'} {name} {extra if not cond else ''}")


class Client:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(10)
        self.s.connect(path)
        self.buf = b""
        self.welcome = self._read_line()

    def _read_line(self):
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("socket closed by peer")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def cmd(self, **kw):
        self.s.sendall((json.dumps(kw) + "\n").encode())
        return self._read_line()

    def detail(self, **kw):
        return json.loads(self.cmd(**kw)["detail"])

    def step_and_settle(self, ticks, settle_s=3.0):
        r = self.cmd(cmd="step", ticks=ticks)
        ok = r.get("ok") is True
        # Steps only queue frames; the render loop consumes them
        # asynchronously, so settle before the next frame-sensitive command.
        time.sleep(settle_s)
        return ok


def main():
    c = Client(sys.argv[1])

    r = c.cmd(cmd="pause", id=1)
    check("paused", r.get("ok") is True, str(r))

    base = c.detail(cmd="list_objects", id=2)
    base_names = [o["name"] for o in base["objects"]]
    r = c.cmd(cmd="spawn_cube", id=3, x=1, y=1, z=1, size=1.0)
    check("spawn cube", r.get("ok") is True, str(r))
    after = c.detail(cmd="list_objects", id=4)
    new = [o for o in after["objects"] if o["name"] not in base_names]
    check("one new object", len(new) == 1, str(new))
    if len(new) != 1:
        print(f"RESULT {len(PASS)} pass {len(FAIL)} fail")
        sys.exit(1)
    oid = new[0]["id"]
    name = new[0]["name"]

    # Wide clip: the viewport frame counter free-ran before we paused, so
    # anchor at 0 with a length that covers wherever it stopped.
    r = c.cmd(cmd="clip_add", id=5, name="tickproof", frame=0,
              length=1000000000)
    check("clip_add", r.get("ok") is True, str(r))
    clip_id = int(r["detail"].rsplit(" ", 1)[1])

    r = c.cmd(cmd="clip_record", id=6, clip=clip_id, oid=oid,
              key="position.x")
    check("record armed", r.get("ok") is True, str(r))
    check("step 2 pre-edit", c.step_and_settle(2), "")
    r = c.cmd(cmd="set_property", id=9, object=name, key="position",
              x=0.75, y=1, z=1)
    check("mid-record edit", r.get("ok") is True, str(r))
    check("step 2 post-edit", c.step_and_settle(2), "")
    r = c.cmd(cmd="clip_record_stop", id=12)
    check("record stop disarms",
          r.get("ok") is True and r.get("detail") == "record disarmed",
          str(r))

    info = c.detail(cmd="clips_info", id=13)
    matching = [k for k in info
                if k["name"] == "tickproof" and
                str(oid) + ":position.x" in k["tracks"]]
    check("clip has the track", len(matching) == 1, str(info))
    if matching:
        track = matching[0]["tracks"][str(oid) + ":position.x"]
        check("4 samples", track["count"] == 4, str(track))
        check("head 1.0 / tail 0.75",
              track["samples"][0] == 1.0 and
              track["samples"][-1] == 0.75, str(track["samples"]))

    # Playback: step-hold at the current (large) frame holds the last
    # sample at or below it, i.e. the 0.75 tail.
    r = c.cmd(cmd="set_property", id=14, object=name, key="position",
              x=5.0, y=1, z=1)
    check("deface for playback", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_play", id=15, clip=clip_id)
    check("clip_play", r.get("ok") is True, str(r))
    check("step 1 replay", c.step_and_settle(1), "")
    px = c.detail(cmd="get_object", id=17, oid=oid)["properties"][
        "position"][0]
    check("replay holds 0.75", abs(px - 0.75) < 1e-9, str(px))
    r = c.cmd(cmd="clip_stop", id=18)
    check("clip_stop", r.get("ok") is True, str(r))

    print(f"RESULT {len(PASS)} pass {len(FAIL)} fail")
    sys.exit(1 if FAIL else 0)


main()
