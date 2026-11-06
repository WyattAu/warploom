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
  t5c -- render-channel tracks (protocol v1.16): arm a track on the
         reserved render_settings object through the wire, drive the
         channel, play it back, and read the driven mask through
         get_render_config. On a visual host the reply also carries the
         values the host actually applied, so the track -> renderer
         mapping is observed rather than inferred. Any host: an armed
         track pins the reserved object against destruction.
  cnc -- concurrent record+play (T5a over the wire): arm playback on a
         source clip AND a record arm on a destination clip for the same
         track, step, and verify the destination captures the PLAYED
         tail (never the stale sentinel), the duplicate arm is rejected
         (T5d), and stopping playback does not stop the recording.
         Frame-relative like g3; the head is unreplayable live, so only
         the tail half of the T5a contract is wire-observable here.

Usage:
  tools/live_proof.py w1 --sock /tmp/omnicpp_hw.sock
  tools/live_proof.py g1 --sock /tmp/omnicpp_hw.sock
  tools/live_proof.py w2 --sock-a /tmp/omnicpp_w2a.sock --sock-b /tmp/omnicpp_w2b.sock
  tools/live_proof.py g3 --sock /tmp/omnicpp_hw.sock  # any host state:
                        # g3 pauses first and places every clip relatively
"""

import argparse
import hashlib
import json
import os
import socket
import sys
import time

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
    # Frame contract: a live viewport ticks its session on EVERY rendered
    # frame while unpaused, so the frame counter races ahead of wire `step`
    # ticks and a clip spanning [0, 6) expires within a tenth of a second of
    # arming — the record auto-disarms and the clip keeps no tracks (seen
    # standalone on a fresh host: "record was not armed" + "no tracks").
    # `pause` freezes the counter; wire `step` then releases frames one at a
    # time and the recorded frames are exactly the stepped ones. The proof
    # pauses first and resumes at the end, so it runs on ANY host state —
    # the old comment's claim that B's frame "is 0" after w2 was wrong
    # (B free-runs the whole time w2 runs) and standalone needed a fresh
    # host only by luck of timing.
    r = c.cmd(cmd="pause", id=48)
    check("pause freezes the frame", r.get("ok") is True, str(r))
    base = c.detail(cmd="list_objects", id=49)
    base_names = [o["name"] for o in base["objects"]]
    r = c.cmd(cmd="spawn_cube", id=50, x=1, y=1, z=1, size=1.0)
    check("spawn cube", r.get("ok") is True, str(r))
    after = c.detail(cmd="list_objects", id=50)
    new = [o for o in after["objects"] if o["name"] not in base_names]
    check("one new object", len(new) == 1, str(new))
    oid = new[0]["id"]
    name = new[0]["name"]

    # Frame contract, second attempt. A viewport ticks its session on every
    # rendered frame, so by the time this proof runs the counter is thousands
    # past zero and a clip at [0, 6) would be born expired (seen standalone:
    # "record was not armed" + "no tracks"). `pause` freezes the counter so
    # each wire `step` releases exactly the requested frames — but the
    # ABSOLUTE frame is still unknown, and clips_info cannot reveal it (its
    # samples array carries head/tail VALUES, not offsets; a first attempt at
    # a probe clip misread the value 1.0 as frame 1). So the clip spans
    # [0, 1e9) — wherever the frozen frame sits, the record below lands
    # inside — and every assertion is frame-relative: counts and values,
    # never absolute positions. (t5c already uses this shape for the same
    # reason.) The deterministic absolute-offset semantics stay pinned by
    # the headless TimelineSession unit tests; this proof pins the wire path.
    r = c.cmd(cmd="clip_add", id=51, name="proof", frame=0, length=1000000000)
    check("clip_add", r.get("ok") is True, str(r))
    clip_id = int(r["detail"].rsplit(" ", 1)[1])  # "added clip <N>"

    # Arm a record track on position.x (vec3 axis form) and tick 2 step
    # frames; the mid-recording edit must land in later samples.
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
    # A wire `step` only QUEUES released frames — the reply lands when the
    # command is received, not when the frames tick (on a loaded GPU a
    # frame can take seconds). Poll the sample count until the 4 queued
    # ticks land; time out rather than assert a half-ticked state. No extra
    # steps are queued here, so count == 4 is exact, not an overshoot.
    # Match by track key, not just clip name: a host re-used across runs
    # holds several clips named "proof" (one per run, distinct cubes), and
    # have[0] would be a frozen clip from an earlier run whose count never
    # moves — exactly the false "count=0" timeout seen when this matched
    # on name alone.
    pkey = str(oid) + ":position.x"
    n = 0
    for _ in range(60):
        info = c.detail(cmd="clips_info", id=80)
        have = [k for k in info
                if k["name"] == "proof" and pkey in k["tracks"]]
        n = have[0]["tracks"][pkey].get("count", 0) if have else 0
        if n >= 4:
            break
        time.sleep(2)
    check("4 ticks landed", n >= 4, "count=%d" % n)
    r = c.cmd(cmd="clip_record_stop", id=56)
    check("record stop", r.get("ok") is True and
          r.get("detail") == "record disarmed", str(r))

    # Replay from the clip start at a frame far past the recorded samples:
    # evaluate clamps to the tail, so stepped ticks must restore 0.75 over
    # a sentinel the ticks could not have produced (999) — which also
    # proves the ticks actually ran before the readback, closing the
    # queue-vs-tick race above in the other direction.
    r = c.cmd(cmd="set_property", id=84, object=name, key="position",
              x=999, y=1, z=1)
    check("sentinel 999", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_play", id=57, clip=clip_id)
    check("clip_play from start", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=58, ticks=2)
    check("step 2 (tail hold)", r.get("ok") is True, str(r))
    for _ in range(60):
        after = c.detail(cmd="get_object", oid=oid)
        px = after["properties"]["position"][0]
        if abs(px - 0.75) < 1e-9:
            break
        time.sleep(2)
    check("replay holds tail 0.75", abs(px - 0.75) < 1e-9, str(px))
    r = c.cmd(cmd="clip_stop", id=59)
    check("clip_stop", r.get("ok") is True, str(r))

    # A zero-length clip ends the moment it begins: arming then stepping
    # auto-disarms on the first tick at ANY host frame (frame_time >= 0 + 0
    # is always true), no samples are added, and stop reports unarmed.
    # This is the end-of-clip disarm contract without an absolute frame.
    r = c.cmd(cmd="clip_add", id=60, name="ends", frame=0, length=0)
    check("zero-length clip_add", r.get("ok") is True, str(r))
    ends_id = int(r["detail"].rsplit(" ", 1)[1])
    r = c.cmd(cmd="clip_record", id=61, clip=ends_id, oid=oid,
              key="position.x")
    check("ends record armed", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=74, ticks=1)
    check("step 1 (past end)", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_record_stop", id=75)
    check("record auto-disarmed at clip end",
          r.get("detail") == "record was not armed", str(r))
    r = c.cmd(cmd="clip_remove", id=76, clip=ends_id)
    check("ends clip removed", r.get("ok") is True, str(r))

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

    # Move the clip far into the future and replay from its (new) start:
    # every stepped tick is below play_started_, so nothing applies and the
    # cube sits untouched at 0.75. This is the original play-silence check,
    # freed from the absolute frame by placing the start beyond any host
    # counter (2^40; exact in JSON doubles, no u64 overflow with length).
    far = 1099511627776
    r = c.cmd(cmd="clip_move", id=63, clip=clip_id, frame=far)
    check("clip_move@far", r.get("ok") is True, str(r))
    # Sentinel again: the ticks must run (else this is vacuous) and apply
    # nothing, so 999 survives. Restored to 0.75 afterwards.
    r = c.cmd(cmd="set_property", id=85, object=name, key="position",
              x=999, y=1, z=1)
    check("sentinel 999 (far)", r.get("ok") is True, str(r))
    # A silent playback leaves no trace, so "nothing happened" is vacuous
    # unless the ticks are observed independently: a watch clip records the
    # untouched y channel over the same two steps. Its count reaching 2
    # proves the ticks ran; x still 999 proves playback applied nothing.
    r = c.cmd(cmd="clip_add", id=87, name="watch", frame=0, length=1000000000)
    check("watch clip_add", r.get("ok") is True, str(r))
    watch_id = int(r["detail"].rsplit(" ", 1)[1])
    r = c.cmd(cmd="clip_record", id=88, clip=watch_id, oid=oid,
              key="position.y")
    check("watch record armed", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_play", id=64, clip=clip_id)
    check("clip_play from far start", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=65, ticks=2)
    check("step 2 (play silent)", r.get("ok") is True, str(r))
    n = 0
    for _ in range(60):
        info = c.detail(cmd="clips_info", id=89)
        have = [k for k in info if k["name"] == "watch"]
        n = have[0]["tracks"].get(str(oid) + ":position.y", {}).get(
            "count", 0) if have else 0
        if n >= 2:
            break
        time.sleep(2)
    check("silence ticks landed", n >= 2, "count=%d" % n)
    after = c.detail(cmd="get_object", oid=oid)
    px = after["properties"]["position"][0]
    check("future play applies nothing", abs(px - 999) < 1e-9, str(px))
    r = c.cmd(cmd="clip_record_stop", id=90)
    check("watch record stop", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_remove", id=91, clip=watch_id)
    check("watch clip removed", r.get("ok") is True, str(r))
    r = c.cmd(cmd="set_property", id=86, object=name, key="position",
              x=0.75, y=1, z=1)
    check("restore 0.75", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_stop", id=66)
    check("clip_stop far", r.get("ok") is True, str(r))

    info = c.detail(cmd="clips_info", id=77)
    moved = [k for k in info
             if k["name"] == "proof" and
             str(oid) + ":position.x" in k["tracks"]]
    check("moved start", len(moved) == 1 and
          moved[0]["start_frame"] == far, str(moved))

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
    # Leave the host as found: resume the free run. Every check above ran
    # under pause, so the recorded/played frames are exactly the stepped
    # ones regardless of what the host was doing before.
    r = c.cmd(cmd="resume", id=73)
    check("resume restores free run", r.get("ok") is True, str(r))


def poll_count(c, pid, name, pkey, want, tries=60):
    """Poll clips_info until the track count reaches `want` (or tries run
    out). A wire `step` only QUEUES released frames — the reply lands when
    the command is received, not when the frames tick (on a loaded GPU a
    frame can take seconds) — so asserting a half-ticked state is a race;
    poll, time out, never overshoot (no extra steps are queued)."""
    import time
    n = 0
    for _ in range(tries):
        info = c.detail(cmd="clips_info", id=pid)
        have = [k for k in info
                if k["name"] == name and pkey in k["tracks"]]
        n = have[0]["tracks"][pkey].get("count", 0) if have else 0
        if n >= want:
            break
        time.sleep(2)
    return n


def proof_cnc(c):
    print("CNC: concurrent record+play over the wire")
    # The session contract is T5a (playback applies, THEN recording samples
    # the current value; per-arm disarm; T5d duplicate-arm rejection). What
    # no proof touches is arming play AND record over the SOCKET on a live
    # host — the queue-vs-tick races only exist there: a `step` reply lands
    # when the command is received, not when the frames tick, so every
    # count below is polled, never asserted once.
    #
    # Frame contract (same as g3): pause first, clips at [0, 1e9), every
    # assertion frame-relative. One honest limit, stated: the head of a
    # source clip is unreplayable live — playback maps frame_time minus
    # clip start onto stored offsets, and the live counter never revisits
    # the frames the source was recorded at (only a scrub warps back, and
    # a warp disarms everything). So the chain reproduces the TAIL, which
    # is exactly the wire-observable half of the T5a ordering contract:
    # at the tail, "record ran after playback" means the played value and
    # "record ran first" means the stale sentinel.
    r = c.cmd(cmd="pause", id=100)
    check("pause freezes the frame", r.get("ok") is True, str(r))
    base = c.detail(cmd="list_objects", id=101)
    base_names = [o["name"] for o in base["objects"]]
    r = c.cmd(cmd="spawn_cube", id=102, x=1, y=1, z=1, size=1.0)
    check("spawn cube", r.get("ok") is True, str(r))
    after = c.detail(cmd="list_objects", id=103)
    new = [o for o in after["objects"] if o["name"] not in base_names]
    check("one new object", len(new) == 1, str(new))
    oid = new[0]["id"]
    name = new[0]["name"]
    pkey = str(oid) + ":position.x"

    # Source clip: three stepped values 5/6/7 on position.x.
    r = c.cmd(cmd="clip_add", id=104, name="csrc", frame=0,
              length=1000000000)
    check("csrc clip_add", r.get("ok") is True, str(r))
    src_id = int(r["detail"].rsplit(" ", 1)[1])
    r = c.cmd(cmd="clip_record", id=105, clip=src_id, oid=oid,
              key="position.x")
    check("csrc record armed", r.get("ok") is True, str(r))
    for i, v in enumerate((5.0, 6.0, 7.0)):
        r = c.cmd(cmd="set_property", id=106 + i, object=name,
                  key="position", x=v, y=1, z=1)
        check("set %.1f" % v, r.get("ok") is True, str(r))
        r = c.cmd(cmd="step", id=110 + i, ticks=1)
        check("step %d" % (i + 1), r.get("ok") is True, str(r))
    n = poll_count(c, 120, "csrc", pkey, 3)
    check("3 source ticks landed", n == 3, "count=%d" % n)
    r = c.cmd(cmd="clip_record_stop", id=121)
    check("csrc record stop", r.get("ok") is True and
          r.get("detail") == "record disarmed", str(r))
    info = c.detail(cmd="clips_info", id=122)
    have = [k for k in info if k["name"] == "csrc" and pkey in k["tracks"]]
    check("csrc has 3 samples", len(have) == 1 and
          have[0]["tracks"][pkey].get("count", 0) == 3, str(have))
    tail = have[0]["tracks"][pkey]["samples"][-1] if have else None
    check("csrc tail is 7.0", tail == 7.0, str(tail))

    # Sentinel: the destination must capture the PLAYED tail, never this.
    # Read it back: if the 999 never landed, the ordering proof below is
    # vacuous (everything would be 7.0 regardless of tick order).
    r = c.cmd(cmd="set_property", id=123, object=name, key="position",
              x=999, y=1, z=1)
    check("sentinel 999", r.get("ok") is True, str(r))
    after = c.detail(cmd="get_object", oid=oid)
    check("sentinel read back", after["properties"]["position"][0] == 999,
          str(after["properties"]["position"][0]))
    r = c.cmd(cmd="clip_add", id=124, name="cdst", frame=0,
              length=1000000000)
    check("cdst clip_add", r.get("ok") is True, str(r))
    dst_id = int(r["detail"].rsplit(" ", 1)[1])
    r = c.cmd(cmd="clip_play", id=125, clip=src_id)
    check("play csrc", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_record", id=126, clip=dst_id, oid=oid,
              key="position.x")
    check("cdst record armed", r.get("ok") is True, str(r))
    # T5d over the wire: the same clip+track twice is rejected, so two
    # writers can never share a track even when play is armed alongside.
    r = c.cmd(cmd="clip_record", id=127, clip=dst_id, oid=oid,
              key="position.x")
    check("double arm rejected",
          r.get("ok") is False and "already armed" in r.get("error", ""),
          str(r))
    r = c.cmd(cmd="step", id=128, ticks=2)
    check("step 2 (chained)", r.get("ok") is True, str(r))
    n = poll_count(c, 129, "cdst", pkey, 2)
    check("2 chained ticks landed", n == 2, "count=%d" % n)
    info = c.detail(cmd="clips_info", id=130)
    have = [k for k in info if k["name"] == "cdst" and pkey in k["tracks"]]
    vals = have[0]["tracks"][pkey]["samples"] if have else []
    # Wire semantics (see clips_info in editor_session.cpp): `samples` is
    # ALWAYS the 2-element [head, tail] display pair and `count` is the
    # true length — the full list lives in the document, not on the wire.
    # With count == 2 that pair IS the whole track, exactly. A
    # sample-before-playback build would record the stale 999 first, so
    # head == 7.0 is the ordering proof and tail == 7.0 the hold proof.
    check("chained track is exactly [7,7]",
          len(vals) == 2 and vals[0] == 7.0 and vals[1] == 7.0, str(vals))

    # Per-arm disarm is independent: stopping playback must not stop the
    # recording, which keeps sampling the held tail.
    r = c.cmd(cmd="clip_stop", id=131)
    check("clip_stop ends playback",
          r.get("ok") is True and
          r.get("detail") == "playback stopped", str(r))
    r = c.cmd(cmd="step", id=132, ticks=1)
    check("step 1 (record alone)", r.get("ok") is True, str(r))
    n = poll_count(c, 133, "cdst", pkey, 3)
    check("record outlives playback", n == 3, "count=%d" % n)
    info = c.detail(cmd="clips_info", id=134)
    have = [k for k in info if k["name"] == "cdst" and pkey in k["tracks"]]
    vals = have[0]["tracks"][pkey]["samples"] if have else []
    # count == 3 (above) is the length proof; the wire pair pins both
    # visible ends at the held tail. The interior of a 3-sample track is
    # wire-invisible by design — but nothing can put a 999 there: the
    # sentinel predates the chain and no writer runs on the solo tick —
    # so head/tail/count pin everything observable. Full-sequence
    # visibility stays a session-level property (T5a unit test).
    check("held tail keeps sampling",
          len(vals) == 2 and vals[0] == 7.0 and vals[1] == 7.0, str(vals))
    r = c.cmd(cmd="clip_record_stop", id=135)
    check("cdst record stop", r.get("ok") is True and
          r.get("detail") == "record disarmed", str(r))
    r = c.cmd(cmd="clip_remove", id=136, clip=dst_id)
    check("cdst removed", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_remove", id=137, clip=src_id)
    check("csrc removed", r.get("ok") is True, str(r))
    r = c.cmd(cmd="resume", id=138)
    check("resume restores free run", r.get("ok") is True, str(r))


def poll_phys_y(c, pid, oid, timeout_s=120):
    """Poll get_physics_body until it answers (ticks may still be landing
    on a threaded host); returns (y, raw_detail)."""
    import time as _t
    y, raw = None, ""
    for _ in range(timeout_s // 2):
        r = c.cmd(cmd="get_physics_body", id=pid, oid=oid)
        if r.get("ok") is True:
            try:
                info = json.loads(r["detail"])
                y, raw = float(info["pos"][1]), r["detail"]
                break
            except (KeyError, ValueError, TypeError):
                pass
        _t.sleep(2)
    return y, raw


def proof_phys(c):
    print("PHYS: session physics over the wire")
    # D1 demo-story half at the session layer: the only sim the wire can
    # drive (the viewport's gd scene runs an app-local world; this leg pins
    # the capture-blessed session sim). Pause-first like g3/cnc; every
    # assertion frame-relative. Rest height is the body radius (0.5).
    r = c.cmd(cmd="pause", id=200)
    check("pause freezes the frame", r.get("ok") is True, str(r))
    base = c.detail(cmd="list_objects", id=201)
    base_names = [o["name"] for o in base["objects"]]
    # Checkpoint before anything exists: the re-run below scrubs here and
    # must reproduce the drop bit-identically (string-equal pos JSON).
    r = c.cmd(cmd="scrub_start", id=202, frame=1000007)
    check("scrub_start phys base", r.get("ok") is True, str(r))
    r = c.cmd(cmd="spawn_cube", id=203, x=0, y=5, z=0, size=1.0)
    check("spawn drop cube", r.get("ok") is True, str(r))
    after = c.detail(cmd="list_objects", id=204)
    new = [o for o in after["objects"] if o["name"] not in base_names]
    check("one new object", len(new) == 1, str(new))
    oid = new[0]["id"]
    # The query discriminates: no body yet is an error, never at-origin.
    r = c.cmd(cmd="get_physics_body", id=205, oid=oid)
    check("query before attach rejected",
          r.get("ok") is False and "no physics body" in r.get("error", ""),
          str(r))
    # One tick projects the spawn; attach needs the projection (same
    # requirement the session unit test pins).
    r = c.cmd(cmd="step", id=217, ticks=1)
    check("step 1 (project spawn)", r.get("ok") is True, str(r))
    r = c.cmd(cmd="spawn_physics_body", id=206, oid=oid)
    check("attach body", r.get("ok") is True and
          "spawned physics body" in r.get("detail", ""), str(r))
    # One body per object: the world has no removal, so a second attach
    # would orphan the first into an invisible collider.
    r = c.cmd(cmd="spawn_physics_body", id=207, oid=oid)
    check("double attach rejected",
          r.get("ok") is False and "already has a body" in r.get("error", ""),
          str(r))
    y, raw = poll_phys_y(c, 208, oid)
    check("attached at authored height",
          y is not None and abs(y - 5.0) < 1e-9, str(raw))
    r = c.cmd(cmd="step", id=209, ticks=10)
    check("step 10 (fall)", r.get("ok") is True, str(r))
    y, raw = poll_phys_y(c, 218, oid)
    check("ticks move the sim",
          y is not None and y < 4.9 and y >= 0.0, str(raw))
    r = c.cmd(cmd="step", id=210, ticks=600)
    check("step 600 (settle)", r.get("ok") is True, str(r))
    y1, raw1 = poll_phys_y(c, 219, oid)
    check("settled on the ground plane",
          y1 is not None and abs(y1 - 0.5) < 0.05, str(raw1))
    pos1 = json.loads(raw1)["pos"] if raw1 else None
    # Scrub back past the spawn: the cube is gone AND the body is gone
    # (scrub restores physics from the checkpoint — T4a over the wire).
    r = c.cmd(cmd="scrub_to", id=211, frame=1000007)
    check("scrub_to phys base", r.get("ok") is True, str(r))
    back = c.detail(cmd="list_objects", id=220)
    check("cube gone after warp",
          all(o["name"] not in [n for n in [new[0]["name"]]]
              for o in back["objects"]),
          str(back["count"]))
    r = c.cmd(cmd="get_physics_body", id=221, oid=oid)
    check("body gone after warp",
          r.get("ok") is False and "no physics body" in r.get("error", ""),
          str(r))
    # Identical second run: same tick sequence, must reproduce to the
    # string (shortest-round-trip floats, deterministic sim).
    r = c.cmd(cmd="spawn_cube", id=212, x=0, y=5, z=0, size=1.0)
    check("re-spawn drop cube", r.get("ok") is True, str(r))
    again = c.detail(cmd="list_objects", id=213)
    new2 = [o for o in again["objects"] if o["name"] not in base_names]
    check("one new object again", len(new2) == 1, str(new2))
    oid2 = new2[0]["id"]
    r = c.cmd(cmd="step", id=222, ticks=1)
    check("step 1 (project re-spawn)", r.get("ok") is True, str(r))
    r = c.cmd(cmd="spawn_physics_body", id=214, oid=oid2)
    check("re-attach body", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=223, ticks=10)
    check("step 10 (fall again)", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=215, ticks=600)
    check("step 600 (settle again)", r.get("ok") is True, str(r))
    y2, raw2 = poll_phys_y(c, 224, oid2)
    check("settled again", y2 is not None and abs(y2 - 0.5) < 0.05,
          str(raw2))
    pos2 = json.loads(raw2)["pos"] if raw2 else None
    check("drop replays bit-identically", pos1 == pos2 and pos1 is not None,
          "%s vs %s" % (pos1, pos2))
    r = c.cmd(cmd="resume", id=216)
    check("resume restores free run", r.get("ok") is True, str(r))


def proof_e1(c):
    """E1/E2 live protocol proofs: render mode + exposure.

    These commands are HOST-OWNED visuals. Both host classes guarantee: valid
    payloads reply ok, and no reply is ever a parse failure ("unknown
    command" is exactly how a missing wire-table entry presents). The viewport
    additionally validates payloads and reports the applied value; a headless
    host acknowledges without visuals. The proof detects the host class from
    the welcome snapshot (the viewport publishes render_mode) and asserts the
    stronger viewport contract when it can.
    """
    print("E1: render mode + exposure over the wire")
    is_visual_host = c.welcome.get("snapshot", {}).get("render_mode") is not None

    r = c.cmd(cmd="get_render_mode", id=601)
    check("get_render_mode ok", r.get("ok") is True, str(r))
    if is_visual_host:
        check("get_render_mode names mode",
              r.get("detail", "").startswith('{"mode"'), str(r))
    r = c.cmd(cmd="set_render_mode", id=602, mode="forward")
    check("set_render_mode forward ok", r.get("ok") is True, str(r))
    r = c.cmd(cmd="set_exposure", id=604, exposure=2.0)
    check("set_exposure ok", r.get("ok") is True, str(r))
    if is_visual_host:
        # Payload validation lives in the viewport host; a headless host
        # acknowledges the kind without looking at the payload.
        r = c.cmd(cmd="set_render_mode", id=603, mode="nonsense")
        check("bad mode rejected", r.get("ok") is False, str(r))
        check("bad mode named error",
              "set_render_mode" in r.get("error", ""), str(r))
        r = c.cmd(cmd="set_exposure", id=605)
        check("exposure without arg rejected", r.get("ok") is False, str(r))

    # R1b path tracing: a third render mode, same host-owned class. The
    # viewport builds the stack on the RENDER thread (the allocator has no
    # lock), so the reply is "scheduled" and the mode is polled until it lands.
    r = c.cmd(cmd="set_render_mode", id=620, mode="pt")
    check("set_render_mode pt ok", r.get("ok") is True, str(r))
    r = c.cmd(cmd="get_pt_samples", id=665)
    check("get_pt_samples ok", r.get("ok") is True, str(r))
    r = c.cmd(cmd="get_pt_mean", id=666)
    check("get_pt_mean ok", r.get("ok") is True, str(r))
    if is_visual_host:
        landed = False
        for _ in range(40):
            m = c.cmd(cmd="get_render_mode", id=621)
            if '"pt"' in m.get("detail", "").replace(" ", ""):
                landed = True
                break
            time.sleep(0.25)
        check("pt mode applied on the visual host", landed,
              "pt never became the reported render mode")
        r = c.cmd(cmd="set_render_mode", id=622, mode="forward")
        check("leave pt ok", r.get("ok") is True, str(r))
        back = False
        for _ in range(40):
            m = c.cmd(cmd="get_render_mode", id=623)
            if '"forward"' in m.get("detail", "").replace(" ", ""):
                back = True
                break
            time.sleep(0.25)
        check("back to forward after pt", back, "did not leave pt")
        r = c.cmd(cmd="set_render_mode", id=624, mode="pt")
        check("re-enter pt ok", r.get("ok") is True, str(r))
        r = c.cmd(cmd="set_render_mode", id=625, mode="forward")
        check("leave pt again ok", r.get("ok") is True, str(r))
        # R1 stepped PT: deterministic "accumulate K frames". Leaving PT
        # resets the accumulation (a fresh tracer starts at sample 0), so
        # two stepped runs must report bitwise-identical mean text — the
        # record/replay seed contract made observable over the wire.
        r = c.cmd(cmd="set_render_mode", id=626, mode="pt")
        check("enter pt for stepped run ok", r.get("ok") is True, str(r))
        stepped_landed = False
        for _ in range(40):
            m = c.cmd(cmd="get_render_mode", id=627)
            if '"pt"' in m.get("detail", "").replace(" ", ""):
                stepped_landed = True
                break
            time.sleep(0.25)
        check("pt landed for stepped run", stepped_landed,
              "pt never landed")
        r = c.cmd(cmd="set_pt_sample_target", id=628, samples=16)
        check("set_pt_sample_target ok", r.get("ok") is True, str(r))
        k1 = False
        for _ in range(120):
            m = c.cmd(cmd="get_pt_samples", id=629)
            if '"samples":16' in m.get("detail", "").replace(" ", ""):
                k1 = True
                break
            time.sleep(0.25)
        check("pt reaches 16 samples", k1, "target never reached")
        m1 = c.cmd(cmd="get_pt_mean", id=636).get("detail", "")
        check("pt mean reads back", '"mean":' in m1, m1)
        r = c.cmd(cmd="set_render_mode", id=637, mode="forward")
        check("leave pt for reset ok", r.get("ok") is True, str(r))
        r = c.cmd(cmd="set_render_mode", id=638, mode="pt")
        check("re-enter pt for repeat ok", r.get("ok") is True, str(r))
        stepped_landed2 = False
        for _ in range(40):
            m = c.cmd(cmd="get_render_mode", id=639)
            if '"pt"' in m.get("detail", "").replace(" ", ""):
                stepped_landed2 = True
                break
            time.sleep(0.25)
        check("pt re-landed for repeat", stepped_landed2,
              "pt never re-landed")
        r = c.cmd(cmd="set_pt_sample_target", id=661, samples=16)
        check("set_pt_sample_target again ok", r.get("ok") is True, str(r))
        k2 = False
        for _ in range(120):
            m = c.cmd(cmd="get_pt_samples", id=662)
            if '"samples":16' in m.get("detail", "").replace(" ", ""):
                k2 = True
                break
            time.sleep(0.25)
        check("pt reaches 16 samples again", k2, "target never reached")
        m2 = c.cmd(cmd="get_pt_mean", id=663).get("detail", "")
        check("stepped pt repeats exactly", m1 == m2 and '"mean":' in m2,
              f"{m1} vs {m2}")
        # T2 adaptive (v1.17): one variance snapshot from the 16-sample
        # accumulation, rank-selected so a 0.25 top fraction is EXACTLY a
        # quarter of the texels even when most variances tie at zero.
        r = c.cmd(cmd="set_adaptive", id=669, enabled=1, fraction=0.25)
        check("set_adaptive on ok", r.get("ok") is True, str(r))
        g = c.detail(cmd="get_adaptive", id=670)
        check("adaptive reports enabled",
              g.get("enabled") is True, str(g))
        check("adaptive selects exactly one quarter",
              g.get("selected", 0) > 0
              and g.get("selected", 0) * 4 == g.get("texels", -1),
              str(g))
        # T2 iterative re-mask (no protocol change): re-issuing set_adaptive
        # re-snapshots from the REFINED accumulation, so the second ranking
        # follows the current noise. The budget stays exact by construction.
        r = c.cmd(cmd="set_adaptive", id=677, enabled=1, fraction=0.25)
        check("adaptive re-mask on demand ok", r.get("ok") is True, str(r))
        g = c.detail(cmd="get_adaptive", id=678)
        check("adaptive re-mask stays enabled with exact budget",
              g.get("enabled") is True
              and g.get("selected", 0) > 0
              and g.get("selected", 0) * 4 == g.get("texels", -1),
              str(g))
        r = c.cmd(cmd="set_adaptive", id=671, enabled=1, fraction=2)
        check("adaptive fraction >1 rejected",
              r.get("ok") is False and "fraction" in r.get("error", ""),
              str(r))
        r = c.cmd(cmd="set_adaptive", id=672, enabled=0)
        check("set_adaptive off ok", r.get("ok") is True, str(r))
        g = c.detail(cmd="get_adaptive", id=673)
        check("adaptive reads back off",
              g.get("enabled") is False and g.get("selected", -1) == 0,
              str(g))
        # T2 render-path denoiser (v1.18): the stack is held at 16 samples,
        # so enabling filters a frozen accumulation -- the denoised mean is
        # stable and must differ from the raw mean (the filter smooths).
        m_raw = c.cmd(cmd="get_pt_mean", id=684).get("detail", "")
        r = c.cmd(cmd="set_denoise", id=681, enabled=1)
        check("set_denoise on ok", r.get("ok") is True, str(r))
        g = c.detail(cmd="get_denoise", id=682)
        check("denoise reads back on",
              g.get("enabled") is True, str(g))
        m_den = c.cmd(cmd="get_denoised_mean", id=683).get("detail", "")
        check("denoised mean reads back", '"mean":' in m_den, m_den)
        check("denoised mean differs from raw",
              m_raw != m_den and '"mean":' in m_raw,
              f"{m_raw} vs {m_den}")
        # A resize rebuilds the tracer (create() is re-entrant) and create()
        # RESETS the tracer-side toggles. The app has to re-apply its own
        # state or the wire contradicts the screen: measured live, a resize
        # with the filter on left get_denoise answering "off" over a filtered
        # frame, and a resize of a HELD stack silently restarted an unbounded
        # accumulation. Both re-applies are pinned here because no headless
        # test can see the app's half of them.
        r = c.cmd(cmd="set_denoise", id=700, enabled=1)
        check("denoise on before resize", r.get("ok") is True, str(r))
        r = c.cmd(cmd="set_adaptive", id=701, enabled=1, fraction=0.25)
        check("adaptive on before resize", r.get("ok") is True, str(r))
        r = c.cmd(cmd="set_viewport_size", id=702, width=960, height=540)
        check("resize with denoise on ok", r.get("ok") is True, str(r))
        time.sleep(1.5)
        g = c.detail(cmd="get_denoise", id=703)
        check("denoise survives a resize", g.get("enabled") is True, str(g))
        g = c.detail(cmd="get_adaptive", id=704)
        check("resize drops the stale adaptive mask",
              g.get("enabled") is False and g.get("selected", -1) == 0, str(g))
        r = c.cmd(cmd="set_viewport_size", id=705, width=1280, height=720)
        check("resize back with denoise on ok", r.get("ok") is True, str(r))
        time.sleep(1.5)
        g = c.detail(cmd="get_denoise", id=706)
        check("denoise survives the resize back",
              g.get("enabled") is True, str(g))
        # The stepped target is app state too: the stack re-converged from 0
        # after the rebuild and must HOLD at 16 again rather than run on.
        held = None
        for _ in range(40):
            held = c.detail(cmd="get_pt_samples", id=707).get("samples", -1)
            if held == 16:
                break
            time.sleep(0.25)
        check("stepped target survives a resize", held == 16, str(held))
        time.sleep(1.0)
        still = c.detail(cmd="get_pt_samples", id=708).get("samples", -1)
        check("stepped target still holds after a resize", still == 16,
              str(still))
        r = c.cmd(cmd="set_denoise", id=685, enabled=0)
        check("set_denoise off ok", r.get("ok") is True, str(r))
        g = c.detail(cmd="get_denoise", id=686)
        check("denoise reads back off",
              g.get("enabled") is False, str(g))
        r = c.cmd(cmd="set_render_mode", id=664, mode="forward")
        check("leave pt after determinism ok", r.get("ok") is True, str(r))
        r = c.cmd(cmd="set_pt_sample_target", id=667, samples=16)
        check("set_pt_sample_target refused outside pt",
              r.get("ok") is False and "PT mode" in r.get("error", ""),
              str(r))
        r = c.cmd(cmd="set_pt_sample_target", id=668)
        check("pt target without arg rejected",
              r.get("ok") is False, str(r))
        r = c.cmd(cmd="set_adaptive", id=674, enabled=1, fraction=0.25)
        check("set_adaptive refused outside pt",
              r.get("ok") is False and "PT mode" in r.get("error", ""),
              str(r))
        r = c.cmd(cmd="set_denoise", id=687, enabled=1)
        check("set_denoise refused outside pt",
              r.get("ok") is False and "PT mode" in r.get("error", ""),
              str(r))
    # Parser-level guard (both hosts): a missing toggle is rejected before
    # any dispatch, so no host can read it as an implicit disable.
    r = c.cmd(cmd="set_adaptive", id=675)
    check("adaptive without enabled rejected",
          r.get("ok") is False and "enabled" in r.get("error", ""),
          str(r))
    r = c.cmd(cmd="get_adaptive", id=676)
    check("get_adaptive ok", r.get("ok") is True, str(r))
    r = c.cmd(cmd="set_denoise", id=688)
    check("denoise without enabled rejected",
          r.get("ok") is False and "enabled" in r.get("error", ""),
          str(r))
    r = c.cmd(cmd="get_denoise", id=689)
    check("get_denoise ok", r.get("ok") is True, str(r))
    r = c.cmd(cmd="get_denoised_mean", id=690)
    check("get_denoised_mean ok", r.get("ok") is True, str(r))

    # E2/R4 bloom. Same host-owned class as above; the interesting part is
    # that toggling it must NOT rebuild anything, which the viewport's
    # available/enabled report is the observable half of.
    r = c.cmd(cmd="get_bloom", id=607)
    check("get_bloom ok", r.get("ok") is True, str(r))
    if is_visual_host:
        check("get_bloom reports both fields",
              '"enabled"' in r.get("detail", "")
              and '"available"' in r.get("detail", ""), str(r))
        check("get_bloom reports available",
              '"available":true' in r.get("detail", "").replace(" ", ""),
              str(r))
    r = c.cmd(cmd="set_bloom", id=608, enabled=1)
    check("set_bloom on ok", r.get("ok") is True, str(r))
    if is_visual_host:
        check("bloom reads back on",
              '"enabled":true' in c.cmd(cmd="get_bloom", id=609)
              .get("detail", "").replace(" ", ""),
              str(c.cmd(cmd="get_bloom", id=610)))
    r = c.cmd(cmd="set_bloom", id=611, enabled=0)
    check("set_bloom off ok", r.get("ok") is True, str(r))
    if is_visual_host:
        check("bloom reads back off",
              '"enabled":false' in c.cmd(cmd="get_bloom", id=612)
              .get("detail", "").replace(" ", ""),
              str(c.cmd(cmd="get_bloom", id=613)))
        r = c.cmd(cmd="set_bloom", id=614)
        check("bloom without arg rejected", r.get("ok") is False, str(r))

    # R2 ray-query sub-toggles (reflections + AO). Same host-owned class as
    # bloom: the toggles change what the lit pass records, never what
    # exists, so a headless host acknowledges without visuals. The viewport
    # validates payloads, reports the applied value, and refuses outside RT
    # mode, where there is no TLAS to query. RT mode is enterable at runtime
    # (unlike the WARPLOOM_RT_MODE=1 startup path, which only chooses the
    # initial mode), so this section drives the whole round trip and leaves
    # forward mode behind, whatever the viewport started in.
    r = c.cmd(cmd="get_rt_reflections", id=640)
    check("get_rt_reflections ok", r.get("ok") is True, str(r))
    r = c.cmd(cmd="get_rt_ao", id=641)
    check("get_rt_ao ok", r.get("ok") is True, str(r))
    if is_visual_host:
        r = c.cmd(cmd="set_render_mode", id=642, mode="rt")
        if r.get("ok") is True:
            rt_landed = False
            for _ in range(40):
                m = c.cmd(cmd="get_render_mode", id=643)
                if '"rt"' in m.get("detail", "").replace(" ", ""):
                    rt_landed = True
                    break
                time.sleep(0.25)
            check("rt mode applied on the visual host", rt_landed,
                  "rt never became the reported render mode")
            if rt_landed:
                r = c.cmd(cmd="set_rt_reflections", id=644, enabled=1)
                check("set_rt_reflections on ok", r.get("ok") is True, str(r))
                check("rt_reflections reads back on",
                      '"enabled":true' in c.cmd(cmd="get_rt_reflections",
                                                  id=645)
                      .get("detail", "").replace(" ", ""),
                      str(c.cmd(cmd="get_rt_reflections", id=646)))
                r = c.cmd(cmd="set_rt_ao", id=647, enabled=1)
                check("set_rt_ao on ok", r.get("ok") is True, str(r))
                check("rt_ao reads back on",
                      '"enabled":true' in c.cmd(cmd="get_rt_ao", id=648)
                      .get("detail", "").replace(" ", ""),
                      str(c.cmd(cmd="get_rt_ao", id=649)))
                r = c.cmd(cmd="set_rt_reflections", id=650, enabled=0)
                check("set_rt_reflections off ok", r.get("ok") is True,
                      str(r))
                check("rt_reflections reads back off",
                      '"enabled":false' in c.cmd(cmd="get_rt_reflections",
                                                   id=651)
                      .get("detail", "").replace(" ", ""),
                      str(c.cmd(cmd="get_rt_reflections", id=652)))
                r = c.cmd(cmd="set_rt_ao", id=653, enabled=0)
                check("set_rt_ao off ok", r.get("ok") is True, str(r))
                check("rt_ao reads back off",
                      '"enabled":false' in c.cmd(cmd="get_rt_ao", id=654)
                      .get("detail", "").replace(" ", ""),
                      str(c.cmd(cmd="get_rt_ao", id=655)))
                r = c.cmd(cmd="set_rt_reflections", id=656)
                check("reflections without arg rejected",
                      r.get("ok") is False, str(r))
                r = c.cmd(cmd="set_rt_ao", id=657)
                check("ao without arg rejected", r.get("ok") is False, str(r))
            r = c.cmd(cmd="set_render_mode", id=658, mode="forward")
            check("leave rt ok", r.get("ok") is True, str(r))
        else:
            check("rt unavailable refusal names the cause",
                  "rt stack unavailable" in r.get("error", ""), str(r))
            r = c.cmd(cmd="set_rt_reflections", id=659, enabled=1)
            check("reflections refused outside RT mode",
                  r.get("ok") is False and "RT mode" in r.get("error", ""),
                  str(r))
            r = c.cmd(cmd="set_rt_ao", id=660, enabled=1)
            check("ao refused outside RT mode",
                  r.get("ok") is False and "RT mode" in r.get("error", ""),
                  str(r))

    # v1.11 viewport resize. The wire contract (both fields required, clamped
    # to [64, 8192]) is provable on ANY host; the window itself only exists on
    # a visual host. A resize drives the whole swapchain recreate path, which
    # is where the capture stack has to be rebuilt too -- that part is only
    # meaningful with a viewport.
    # A headless host has no viewport, so it must REFUSE this cleanly rather
    # than pretend to resize something. The rejections below are the parse
    # contract and hold on any host; the acceptance is host-owned.
    if is_visual_host:
        r = c.cmd(cmd="set_viewport_size", id=630, width=1000, height=600)
        check("set_viewport_size ok", r.get("ok") is True, str(r))
    else:
        r = c.cmd(cmd="set_viewport_size", id=630, width=1000, height=600)
        check("set_viewport_size refused on a headless host",
              r.get("ok") is False, str(r))
    r = c.cmd(cmd="set_viewport_size", id=631, width=1000)
    check("viewport size missing height rejected", r.get("ok") is False, str(r))
    r = c.cmd(cmd="set_viewport_size", id=632, width=10, height=10)
    check("viewport size under-clamp rejected", r.get("ok") is False, str(r))
    r = c.cmd(cmd="set_viewport_size", id=633, width=99999, height=600)
    check("viewport size over-clamp rejected", r.get("ok") is False, str(r))
    if is_visual_host:
        # A real round trip: resize away and back, letting the render thread
        # apply each. The snapshot has no size field, so success is "the
        # command was accepted and the viewport kept rendering" -- checked by
        # the follow-up sim step below, which fails loudly if the frame loop
        # died on the recreate.
        r = c.cmd(cmd="set_viewport_size", id=634, width=960, height=540)
        check("viewport resized down ok", r.get("ok") is True, str(r))
        time.sleep(1.0)
        r = c.cmd(cmd="set_viewport_size", id=635, width=1280, height=720)
        check("viewport resized back ok", r.get("ok") is True, str(r))
        time.sleep(1.0)

    # Session health after the visual churn: the sim still steps.
    r = c.cmd(cmd="step", id=615, ticks=1)
    check("sim steps after mode churn", r.get("ok") is True, str(r))


def proof_t5c(c):
    """T5c render-channel tracks (protocol v1.16).

    Session-side behaviour is host-independent, so this runs on both hosts:
    the reserved render_settings object is created by an arm, a channel is
    recorded and played back, and get_render_config reports the values plus
    which channels a playing clip drove on the last tick. A visual host
    appends applied_exposure/applied_bloom/applied_mode, which is the
    observable half of the track -> renderer mapping.
    """
    print("T5c: render-channel tracks over the wire")
    is_visual_host = c.welcome.get("snapshot", {}).get("render_mode") is not None

    cfg = c.detail(cmd="get_render_config", id=680)
    check("get_render_config ok",
          isinstance(cfg, dict) and "exposure" in cfg, str(cfg))
    check("render config starts undriven", cfg.get("driven") == [], str(cfg))

    r = c.cmd(cmd="clip_add", id=681, name="t5c", frame=0, length=1000000)
    check("clip_add t5c", r.get("ok") is True, str(r))
    clip_id = int(r["detail"].rsplit(" ", 1)[1])

    before = c.detail(cmd="list_objects", id=682)
    check("no render_settings object yet",
          all(o["name"] != "render_settings" for o in before["objects"]),
          str(before["objects"])[:200])
    # oid 0 + a bare render channel resolves to the reserved object and
    # creates it on demand (undoable spawn).
    r = c.cmd(cmd="clip_record", id=683, clip=clip_id, oid=0, key="exposure")
    check("clip_record oid=0 exposure armed",
          r.get("ok") is True and "recording clip" in r.get("detail", ""),
          str(r))
    after = c.detail(cmd="list_objects", id=684)
    rs = [o for o in after["objects"] if o["name"] == "render_settings"]
    check("arm created the reserved object exactly once", len(rs) == 1, str(rs))
    rs_id = rs[0]["id"] if rs else 0

    # Drive the channel, then tick: the sample captures the driven value
    # (not the default), which is what makes the playback check meaningful.
    r = c.cmd(cmd="set_property", id=685, object="render_settings",
              key="exposure", x=2.5)
    check("set exposure channel", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=686, ticks=2)
    check("step during record", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_record_stop", id=687)
    check("record stop", r.get("ok") is True, str(r))

    info = c.detail(cmd="clips_info", id=688)
    mine = [k for k in info if k["name"] == "t5c"]
    check("clips_info has t5c", len(mine) == 1, str(info)[:300])
    exp_key = str(rs_id) + ":exposure"
    check("exposure track keyed on the reserved id",
          len(mine) == 1 and exp_key in mine[0]["tracks"],
          str(mine[0]["tracks"]) if mine else "")
    check("recorded the driven value",
          len(mine) == 1 and mine[0]["tracks"].get(exp_key, {}).get("count", 0)
          >= 1 and
          abs(mine[0]["tracks"][exp_key]["samples"][-1] - 2.5) < 1e-9,
          str(mine[0]["tracks"]) if mine else "")

    # Armment pins the reserved object: destroying it would silently kill
    # every render track, so the deletion is refused by name.
    r = c.cmd(cmd="destroy_object", id=689, oid=rs_id)
    check("referenced render_settings cannot be destroyed",
          r.get("ok") is False and "referenced by" in r.get("error", ""),
          str(r))

    # Playback: reset the channel to the default so a value reappearing
    # proves the track wrote it, not that it was never changed.
    r = c.cmd(cmd="set_property", id=690, object="render_settings",
              key="exposure", x=1.0)
    check("reset exposure to default", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_play", id=691, clip=clip_id)
    check("clip_play t5c", r.get("ok") is True, str(r))
    r = c.cmd(cmd="step", id=692, ticks=1)
    check("step during playback", r.get("ok") is True, str(r))
    cfg = c.detail(cmd="get_render_config", id=693)
    check("playback landed on the channel",
          abs(float(cfg.get("exposure", 0.0)) - 2.5) < 1e-9, str(cfg))
    check("playing clip marks exposure driven",
          "exposure" in cfg.get("driven", []), str(cfg))
    if is_visual_host:
        applied = float(cfg.get("applied_exposure", 0.0))
        check("host applied the driven exposure", abs(applied - 2.5) < 1e-3,
              str(cfg))
    r = c.cmd(cmd="clip_stop", id=694)
    check("clip_stop t5c", r.get("ok") is True, str(r))
    # After the clip stops nothing is driven, so the host must stop
    # overriding the channel (a sticky flag would clobber socket control).
    r = c.cmd(cmd="step", id=695, ticks=1)
    cfg = c.detail(cmd="get_render_config", id=696)
    check("stopped clip drives nothing", cfg.get("driven") == [], str(cfg))
    r = c.cmd(cmd="set_property", id=697, object="render_settings",
              key="exposure", x=1.0)
    check("restore exposure", r.get("ok") is True, str(r))
    r = c.cmd(cmd="clip_remove", id=698, clip=clip_id)
    check("clip_remove t5c", r.get("ok") is True, str(r))
    # With no tracks left the object is deletable again: the guard is about
    # dangling tracks, not about the object being special.
    r = c.cmd(cmd="destroy_object", id=699, oid=rs_id)
    check("unreferenced render_settings is deletable",
          r.get("ok") is True, str(r))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "proof", choices=["w1", "g1", "w2", "g3", "e1", "t5c", "cnc",
                            "phys", "all"])
    parser.add_argument("--sock", default="/tmp/omnicpp_hw.sock")
    parser.add_argument("--sock-a", default="/tmp/omnicpp_w2a.sock")
    parser.add_argument("--sock-b", default="/tmp/omnicpp_w2b.sock")
    args = parser.parse_args()

    if args.proof in ("w1", "g1", "e1", "t5c", "cnc", "phys", "all"):
        c = Client(args.sock)
        if args.proof in ("w1", "all"):
            proof_w1(c)
        if args.proof in ("g1", "all"):
            proof_g1(c)
        if args.proof in ("e1", "all"):
            proof_e1(c)
        if args.proof in ("t5c", "all"):
            # Frame-counter independent: the clip spans [0, 1e6), so this
            # runs correctly on a fresh host and after the e1 churn alike.
            proof_t5c(c)
        if args.proof in ("cnc", "all"):
            # Same pause-first, frame-relative shape as g3, on the main
            # (viewport) host: the queue-vs-tick races this pins only
            # exist where a free-running frame loop meets wire steps.
            proof_cnc(c)
        if args.proof in ("phys", "all"):
            # D1 session sim over the wire. Headless-capable (no visuals);
            # on `all` it runs on the main host like cnc.
            proof_phys(c)
    if args.proof in ("w2", "all"):
        proof_w2(Client(args.sock_a), Client(args.sock_b))
    if args.proof in ("g3", "all"):
        # proof_g3 pauses first and places every clip frame-relatively, so
        # it runs on any host state — including sock-b after w2, whose
        # frame counter has free-run the whole time w2 ran.
        proof_g3(Client(args.sock_b if args.proof == "all" else args.sock))

    print()
    print(f"LIVE PROOF: {len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        print("FAILED:", FAIL)
        sys.exit(1)
    print("LIVE_PROOF_OK")


if __name__ == "__main__":
    main()
