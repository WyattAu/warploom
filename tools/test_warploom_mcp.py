#!/usr/bin/env python3
"""Self-test for tools/warploom_mcp.py (stdlib only, no test framework).

Spawns a headless host on a temp socket, drives the MCP bridge over stdio
(initialize -> tools/list -> ping -> list -> spawn -> step -> get -> undo),
and asserts every hop. Exit 0 = pass. Works headless in CI; the same bridge
binary talks to the viewport when pointed at its socket.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MCP = os.path.join(ROOT, "tools", "warploom_mcp.py")
HOST_CANDIDATES = [
    os.path.join(ROOT, "build", "default", "bin", "warploom_headless_host"),
    os.path.join(ROOT, "build", "headless-debug", "bin",
                 "warploom_headless_host"),
]

FAILURES = []


def check(cond, label, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'} {label}"
          + (f" [{detail}]" if detail and not cond else ""))
    if not cond:
        FAILURES.append(label)


def wait_for_sock(path, timeout=15.0):
    # NOTE: a unix socket is not open()able — probe with connect instead.
    end = time.time() + timeout
    while time.time() < end:
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            probe.settimeout(0.2)
            probe.connect(path)
            return True
        except OSError:
            time.sleep(0.05)
        finally:
            probe.close()
    return False


def rpc(proc, method, params=None, req_id=None):
    msg = {"jsonrpc": "2.0", "method": method}
    if req_id is not None:
        msg["id"] = req_id
    if params is not None:
        msg["params"] = params
    proc.stdin.write(json.dumps(msg) + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("MCP bridge closed stdout")
    return json.loads(line)


def call_tool(proc, req_id, name, args):
    reply = rpc(proc, "tools/call",
                {"name": name, "arguments": args}, req_id)
    if "error" in reply:
        return True, {"protocol_error": reply["error"]}
    result = reply.get("result", {})
    text = result.get("content", [{}])[0].get("text", "")
    try:
        return result.get("isError", True), json.loads(text) if text else {}
    except json.JSONDecodeError:
        # Bridge-level errors are plain "error: ..." text, not JSON.
        return True, {"text": text}


def main():
    host_bin = next((p for p in HOST_CANDIDATES if os.path.isfile(p)), None)
    if host_bin is None:
        print("SKIP: no headless host binary built")
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        sock = os.path.join(tmp, "mcp_test.sock")
        host = subprocess.Popen([host_bin, sock],
                                stderr=subprocess.DEVNULL)
        try:
            if not wait_for_sock(sock):
                print("FAIL: headless host never bound its socket")
                return 1
            bridge = subprocess.Popen(
                [sys.executable, MCP, "--sock", sock],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, bufsize=1)
            try:
                nxt = iter(range(100, 1000)).__next__

                hello = rpc(bridge, "initialize",
                            {"protocolVersion": "2024-11-05",
                             "capabilities": {},
                             "clientInfo": {"name": "self-test",
                                            "version": "0"}}, nxt())
                check(hello.get("result", {}).get("serverInfo", {}).get(
                    "name") == "warploom", "initialize/handshake",
                    str(hello))

                listed = rpc(bridge, "tools/list", {}, nxt())
                names = [t["name"] for t in
                         listed.get("result", {}).get("tools", [])]
                for want in ("engine_ping", "scene_list_objects",
                             "scene_spawn_cube", "sim_step",
                             "telemetry_read_log", "engine_raw"):
                    check(want in names, f"tool listed: {want}", str(names))

                is_err, out = call_tool(bridge, nxt(), "engine_ping", {})
                check(not is_err, "engine_ping over bridge", str(out))

                is_err, before = call_tool(bridge, nxt(),
                                           "scene_list_objects", {})
                n0 = before.get("detail", {}).get("count", -1)
                check(not is_err and n0 >= 0, "list baseline", str(before))

                is_err, spawned = call_tool(
                    bridge, nxt(), "scene_spawn_cube",
                    {"x": 1, "y": 2, "z": 3, "size": 0.5})
                check(not is_err, "spawn via bridge", str(spawned))

                is_err, stepped = call_tool(bridge, nxt(), "sim_step",
                                            {"ticks": 2})
                check(not is_err, "step via bridge", str(stepped))

                is_err, after = call_tool(bridge, nxt(),
                                          "scene_list_objects", {})
                check(not is_err and
                      after.get("detail", {}).get("count") == n0 + 1,
                      "spawn visible after step", str(after))

                objs = after["detail"]["objects"]
                target = [o for o in objs if o["name"] not in
                          [q["name"] for q in before["detail"]["objects"]]][0]

                is_err, got = call_tool(bridge, nxt(), "scene_get_object",
                                        {"oid": target["id"]})
                check(not is_err, "get spawned object", str(got))

                is_err, moved = call_tool(
                    bridge, nxt(), "scene_set_property",
                    {"object": target["name"], "key": "position",
                     "x": 9, "y": 8, "z": 7})
                check(not is_err, "mutate via bridge", str(moved))

                is_err, undone = call_tool(bridge, nxt(), "scene_undo", {})
                check(not is_err, "undo via bridge", str(undone))

                # An unknown command must surface the HOST's rejection
                # verbatim through engine_raw (protocol: "unknown
                # command"), not a bridge invention or mask.
                is_err, rej = call_tool(bridge, nxt(), "engine_raw",
                                        {"cmd": "bogus_command_xyz",
                                         "params": {}})
                check(is_err and "unknown command" in str(rej).lower(),
                      "host rejection surfaces verbatim", str(rej))

                # Escape hatch forwards unknown-but-valid commands.
                is_err, raw = call_tool(bridge, nxt(), "engine_raw",
                                        {"cmd": "schema", "params": {}})
                check(not is_err, "engine_raw escape hatch", str(raw)[:120])

                # Bad input is a bridge-level error, never a socket write.
                is_err, _ = call_tool(bridge, nxt(), "scene_get_object", {})
                check(is_err, "missing param rejected pre-socket")
            finally:
                try:
                    bridge.stdin.close()
                except BrokenPipeError:
                    pass
                bridge.wait(timeout=10)
        finally:
            host.terminate()
            host.wait(timeout=10)

    if FAILURES:
        print(f"{len(FAILURES)} FAILURES")
        return 1
    print("mcp self-test: all pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
