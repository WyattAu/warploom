#!/usr/bin/env python3
"""Live protocol proofs against running viewport control sockets.

Proofs (run against hardware per docs/roadmap.md verification discipline):
  w1  -- replay scrubber: checkpoint, mutate, scrub_info, negative
         rejection, time warp back to byte-equal baseline state.
  g1  -- document save/load round-trip: wire state equality + disk
         sha256 byte-compare.
  w2  -- record/replay: capture on instance A (socket A), load into a
         FRESH instance B (socket B); wire state must be byte-identical,
         the checkpoint timeline must hydrate, and B must warp back to
         the opening checkpoint. Exit proof for W2.
  g3  -- timeline clips (protocol v1.8): record a property track through
         step ticks, play it back step-held after a clip_move, verify
         clips_info shape and that scrub warps disarm record/playback.
         Exit proof for G3 core.

Usage:
  tools/live_proof.py w1 --sock /tmp/omnicpp_hw.sock
  tools/live_proof.py g1 --sock /tmp/omnicpp_hw.sock
  tools/live_proof.py w2 --sock-a /tmp/omnicpp_w2a.sock --sock-b /tmp/omnicpp_w2b.sock
  tools/live_proof.py g3 --sock /tmp/omnicpp_hw.sock  # FRESH host: the
                        # g3 frame contract needs a step-untouched counter
"""

import argparse
import hashlib
import json
import os
import socket
import sys

PASS = []
FAIL = []


def check(name, cond, extra=""):
    if cond:
        PASS.append(name)
        print(f"  PASS {name}")
    else:
        FAIL.append(name)
        print(f"  FAIL {name} {extra}")


class Client:
    """One JSONL control-socket client (welcome precedes all replies)."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(5)
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


def proof_w1(c):
    print("W1: replay scrubber over the wire")
    check("welcome event", c.welcome.get("event") == "welcome")
    base = c.detail(cmd="list_objects", id=1)
    n0 = base["count"]

    r = c.cmd(cmd="scrub_start", id=2, frame=7)
    check("scrub_start@7", r.get("ok") is True, str(r))

    r = c.cmd(cmd="spawn_cube", id=3, x=1, y=2, z=3, size=0.5)
    check("spawn_cube", r.get("ok") is True, str(r))
    after = c.detail(cmd="list_objects", id=4)
    check("spawn visible", after["count"] == n0 + 1, str(after["count"]))
    new = [o["name"] for o in after["objects"]
           if o["name"] not in [q["name"] for q in base["objects"]]]
    check("one new object", len(new) == 1, str(new))
    target = new[0]

    r = c.cmd(cmd="set_property", id=5, object=target, key="position",
              x=9, y=8, z=7)
    check("set_property", r.get("ok") is True, str(r))

    info = c.detail(cmd="scrub_info", id=6)
    check("ckpt 7 listed", 7 in info.get("checkpoints", []), str(info))

    r = c.cmd(cmd="scrub_to", id=7, frame=999)
    check("unknown frame rejected", r.get("ok") is False, str(r))

    r = c.cmd(cmd="scrub_to", id=8, frame=7)
    check("scrub_to@7", r.get("ok") is True, str(r))
    post = c.detail(cmd="list_objects", id=9)
    check("post-warp == baseline",
          post["objects"] == base["objects"] and post["count"] == n0,
          str(post["count"]))


def proof_g1(c):
    print("G1: document save/load round-trip")
    path_a = "/tmp/live_proof_g1_a.json"
    path_b = "/tmp/live_proof_g1_b.json"
    for p in (path_a, path_b):
        if os.path.exists(p):
            os.remove(p)

    r = c.cmd(cmd="spawn_cube", id=20, x=4, y=5, z=6, size=0.8)
    check("spawn", r.get("ok") is True, str(r))
    r = c.cmd(cmd="save_document", id=21, path=path_a)
    check("save A", r.get("ok") is True, str(r))
    saved = c.detail(cmd="list_objects", id=22)

    r = c.cmd(cmd="spawn_cube", id=23, x=7, y=7, z=7, size=0.3)
    check("mutate past save", r.get("ok") is True, str(r))
    r = c.cmd(cmd="load_document", id=24, path="/tmp/live_proof_missing.json")
    check("missing file rejected", r.get("ok") is False, str(r))

    r = c.cmd(cmd="load_document", id=25, path=path_a)
    check("load A", r.get("ok") is True, str(r))
    check("wire state == saved",
          c.detail(cmd="list_objects", id=26) == saved)

    r = c.cmd(cmd="save_document", id=27, path=path_b)
    check("save B", r.get("ok") is True, str(r))
    h = [hashlib.sha256(open(p, "rb").read()).hexdigest()
         for p in (path_a, path_b)]
    check("sha256 round-trip equal", h[0] == h[1], str(h))
    for p in (path_a, path_b):
        if os.path.exists(p):
            os.remove(p)


def proof_w2(a, b):
    print("W2: record on A -> load into fresh B -> byte-identical")
    path = "/tmp/live_proof_w2.replay"
    if os.path.exists(path):
        os.remove(path)

    # A: capture a session with a mid-capture time warp.
    r = a.cmd(cmd="start_capture", id=30, frame=0)
    check("start_capture", r.get("ok") is True, str(r))
    st = a.detail(cmd="capture_status", id=31)
    check("status recording", st.get("recording") is True, str(st))
    check("opening ckpt embedded", st.get("checkpoints") == 1, str(st))

    r = a.cmd(cmd="scrub_start", id=32, frame=5)
    check("scrub_start@5 (recorded)", r.get("ok") is True, str(r))
    r = a.cmd(cmd="spawn_cube", id=33, x=1, y=0, z=0, size=1.0)
    check("spawn one", r.get("ok") is True, str(r))
    r = a.cmd(cmd="step", id=34, ticks=4)
    check("step 4", r.get("ok") is True, str(r))
    r = a.cmd(cmd="spawn_cube", id=35, x=2, y=0, z=0, size=1.0)
    check("spawn two", r.get("ok") is True, str(r))

    r = a.cmd(cmd="stop_capture", id=36, path=path)
    check("stop_capture", r.get("ok") is True, str(r))
    st = a.detail(cmd="capture_status", id=37)
    check("status stopped", st.get("recording") is False, str(st))
    check("file exists", os.path.isfile(path))
    with open(path, "rb") as f:
        raw = f.read()
    check("header line", raw.startswith(b'{"record":"header"'), raw[:60])
    check("end record", b'"record":"end"' in raw)
    check("no temp residue", not os.path.exists(path + ".tmp." +
                                               str(os.getpid())))

    state_a = a.detail(cmd="list_objects", id=38)

    # B: load the replay into the fresh instance.
    r = b.cmd(cmd="load_replay", id=40, path=path)
    check("load_replay on B", r.get("ok") is True, str(r))
    state_b = b.detail(cmd="list_objects", id=41)
    check("B wire state == A", state_b == state_a,
          f"{state_b['count']} vs {state_a['count']}")
    info_b = b.detail(cmd="scrub_info", id=42)
    # B's timeline is the FILE's checkpoint set (hydration), which is
    # {start 0, scrub_start target 5, stop 0}: the stop checkpoint replaces
    # the start one (same frame). A's own ring only holds what IT captured
    # ({5}) — file set ⊇ A's ring is the invariant.
    check("B timeline hydrates file ckpts",
          set(info_b["checkpoints"]) >= {0, 5}, str(info_b))
    info_a = a.detail(cmd="scrub_info", id=43)
    check("A ring holds its capture targets",
          set(info_a["checkpoints"]) >= {5}, str(info_a))

    # B warps back to the opening checkpoint.
    r = b.cmd(cmd="scrub_to", id=44, frame=0)
    check("B warp to opening ckpt", r.get("ok") is True, str(r))
    base_b = b.detail(cmd="list_objects", id=45)
    check("B post-warp smaller than loaded",
          base_b["count"] < state_b["count"], str(base_b["count"]))

    os.remove(path)


def proof_g3(c):
    print("G3: timeline clips over the wire")
    # Frame contract: the host frame advances ONLY on `step` ticks, so this
    # proof must run on a connection whose frame counter is known. In `all`
    # mode main() runs it on sock-b AFTER w2 (w2's instance B never gets a
    # wire `step` — its load_replay re-simulates ticks internally — so its
    # frame is 0); standalone, run it against a fresh host.
    base = c.detail(cmd="list_objects", id=49)
    base_names = [o["name"] for o in base["objects"]]
    r = c.cmd(cmd="spawn_cube", id=50, x=1, y=1, z=1, size=1.0)
    check("spawn cube", r.get("ok") is True, str(r))
    after = c.detail(cmd="list_objects", id=50)
    new = [o for o in after["objects"] if o["name"] not in base_names]
    check("one new object", len(new) == 1, str(new))
    oid = new[0]["id"]
    name = new[0]["name"]

    # Clip spanning [0, 6); frame is 0, so the record below captures
    # frames 0..3.
    r = c.cmd(cmd="clip_add", id=51, name="proof", frame=0, length=6)
    check("clip_add", r.get("ok") is True, str(r))
    clip_id = int(r["detail"].rsplit(" ", 1)[1])  # "added clip <N>"

    # Arm a record track on position.x (vec3 axis form) and tick 2 step
    # frames (0..1); the mid-recording edit must land in later samples.
    r = c.cmd(cmd="clip_record", id=52, clip=clip_id, oid=oid,
              key="position.x")
    check("clip_record armed", r.get("ok") is True and
          "recording clip %d" % clip_id in r.get("detail", ""), str(r))
    r = c.cmd(cmd="step", id=53, ticks=2)
    check("step 2 (pre-edit)", r.get("ok") is True, str(r))
    r = c.cmd(cmd="set_property", id=54, object=name, key="position",
              x=0.75, y=1, z=1)
    check("set mid-recording", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=55, ticks=2)
    check("step 2 (post-edit)", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_record_stop", id=56)
    check("record stop", r.get("ok") is True and
          r.get("detail") == "record disarmed", str(r))

    # Playback armed from frame 6 while the host frame is 4 is silence:
    # ticks 4..5 are below play_started_, so nothing applies.
    r = c.cmd(cmd="clip_play", id=57, clip=clip_id, frame=6)
    check("clip_play@6", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=58, ticks=2)
    check("step 2 (play silent)", r.get("ok") is True, str(r))

    # Re-arming record past the clip end auto-disarms on the first tick
    # (frame 6 >= 0 + 6): no samples are added and stop reports unarmed.
    r = c.cmd(cmd="clip_record", id=59, clip=clip_id, oid=oid,
              key="position.x")
    check("record re-arm past end", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=60, ticks=2)
    check("step 2 (past end)", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_record_stop", id=61)
    check("record auto-disarmed at clip end",
          r.get("detail") == "record was not armed", str(r))

    info = c.detail(cmd="clips_info", id=62)  # already parsed JSON
    matching = [k for k in info
                if k["name"] == "proof" and
                str(oid) + ":position.x" in k["tracks"]]
    check("clips_info has proof clip", len(matching) == 1, str(info))
    track = matching[0]["tracks"][str(oid) + ":position.x"]
    check("4 samples (end ticks excluded)", track["count"] == 4,
          str(track))
    check("head 1.0 / tail 0.75",
          track["samples"][0] == 1.0 and
          track["samples"][-1] == 0.75, str(track["samples"]))

    # Move the clip to [8, 14) and replay from its (new) start. Ticks run
    # 8..10: offsets 0,1,2 step-hold 1.0, 1.0, then the edit value 0.75.
    r = c.cmd(cmd="clip_move", id=63, clip=clip_id, frame=8)
    check("clip_move@8", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_play", id=64, clip=clip_id)
    check("clip_play from start", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=65, ticks=3)
    check("step 3 (replay)", r.get("ok") is True, str(r))

    after = c.detail(cmd="get_object", oid=oid)
    px = after["properties"]["position"][0]
    check("step-held 0.75", abs(px - 0.75) < 1e-9, str(px))

    info = c.detail(cmd="clips_info", id=66)
    moved = [k for k in info
             if k["name"] == "proof" and
             str(oid) + ":position.x" in k["tracks"]]
    check("moved start", len(moved) == 1 and
          moved[0]["start_frame"] == 8, str(moved))

    # Scrub warps disarm anything armed: re-arm both, warp, verify.
    r = c.cmd(cmd="clip_record", id=67, clip=clip_id, oid=oid,
              key="position.x")
    check("record re-arm", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_play", id=68, clip=clip_id)
    check("play re-arm", r.get("ok") is True, str(r))
    r = c.cmd(cmd="scrub_start", id=69, frame=20)
    check("scrub_start", r.get("ok") is True, str(r))
    r = c.cmd(cmd="scrub_to", id=70, frame=20)
    check("scrub_to", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_record_stop", id=71)
    check("record disarmed by warp",
          r.get("detail") == "record was not armed", str(r))
    r = c.cmd(cmd="clip_stop", id=72)
    check("playback disarmed by warp",
          r.get("detail") == "playback was not armed", str(r))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("proof", choices=["w1", "g1", "w2", "g3", "all"])
    parser.add_argument("--sock", default="/tmp/omnicpp_hw.sock")
    parser.add_argument("--sock-a", default="/tmp/omnicpp_w2a.sock")
    parser.add_argument("--sock-b", default="/tmp/omnicpp_w2b.sock")
    args = parser.parse_args()

    if args.proof in ("w1", "g1", "all"):
        c = Client(args.sock)
        if args.proof in ("w1", "all"):
            proof_w1(c)
        if args.proof in ("g1", "all"):
            proof_g1(c)
    if args.proof in ("w2", "all"):
        proof_w2(Client(args.sock_a), Client(args.sock_b))
    if args.proof in ("g3", "all"):
        # In `all` mode g3 runs on sock-b after w2: w2's instance B never
        # receives a wire `step` (its replay re-simulates internally), so
        # its host frame counter is still 0 — see proof_g3's contract note.
        proof_g3(Client(args.sock_b if args.proof == "all" else args.sock))

    print()
    print(f"LIVE PROOF: {len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        print("FAILED:", FAIL)
        sys.exit(1)
    print("LIVE_PROOF_OK")


if __name__ == "__main__":
    main()
