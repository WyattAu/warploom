#!/usr/bin/env python3
"""Warploom MCP server: read, telemetry, and controlled mutation over stdio.

Bridge, not a new protocol: every tool forwards to the existing Unix-socket
JSONL control channel (`modules/core/include/warploom/core/control_server.hpp`
`ControlCommand::kind_names()`), so the engine keeps one authoritative
command surface and this file carries no engine logic of its own.

Works with BOTH hosts because the wire is shared: the headless host
(`tools/headless_host.cpp`, CI-friendly, no GPU) and the live viewport
(`WARPLOOM_CONTROL_SOCKET=<path>`, needs display/GPU). Host-owned visuals
(render mode, exposure, bloom, RT toggles, stepped PT, viewport size,
capture) simply return the host's own rejection on a host that does not own
them; the error is surfaced verbatim, never masked.

Dependency budget (AGENTS.md): ONE package manager (CPM) + ONE third-party
library (GoogleTest). This bridge is STDLIB ONLY (socket, json, sys, os,
argparse) — no MCP SDK, no pip install — so the budget is untouched.

Transport: MCP JSON-RPC 2.0 over stdio (MCP 2024-11-05: `initialize`,
`notifications/initialized`, `tools/list`, `tools/call`, `ping`).
Framing is one JSON object per line on stdin/stdout; stderr is log-only.

Socket resolution (first hit wins):
  1. `python3 tools/warploom_mcp.py --sock /path/to.sock`
  2. `$WARPLOOM_SOCK` (MCP-specific override, checked first so an MCP
     client can point at headless or viewport without disturbing them)
  3. `$WARPLOOM_CONTROL_SOCKET` / `$OMNICPP_CONTROL_SOCKET` (the same env
     the hosts themselves read, so the bridge defaults to the live host)
  4. `/tmp/warploom.sock`

Usage (Claude Code / MCP client config):
  {"command": ["python3", "/path/to/tools/warploom_mcp.py"],
   "env": {"WARPLOOM_SOCK": "/tmp/proof_a.sock"}}

Determinism contract: the sim advances ONLY via `sim_step` (fixed ticks);
nothing here reads the wall clock. `scene_snapshot` re-reads the host's
welcome (authoritative state, < 1 MiB by protocol).
"""

import argparse
import json
import os
import socket
import sys

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "warploom", "version": "0.1.0"}

DEFAULT_SOCK = "/tmp/warploom.sock"


def resolve_sock(explicit=None):
    if explicit:
        return explicit
    for env in ("WARPLOOM_SOCK", "WARPLOOM_CONTROL_SOCKET",
                "OMNICPP_CONTROL_SOCKET"):
        val = os.environ.get(env)
        if val:
            return val
    return DEFAULT_SOCK


class Engine:
    """One persistent JSONL control-socket client (welcome precedes replies)."""

    def __init__(self, path):
        self.path = path
        self.sock = None
        self.buf = b""
        self.welcome = None
        self.next_id = 1

    def _ensure(self):
        if self.sock is not None:
            return
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(self.path)
        self.sock = sock
        self.buf = b""
        self.welcome = self._read_line()

    def _read_line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("engine socket closed by peer")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def _reconnect(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self.sock = None
        self._ensure()

    def cmd(self, name, **params):
        """Send one control command; retry once on a dropped connection."""
        payload = {"cmd": name, "id": self.next_id}
        self.next_id += 1
        payload.update(params)
        for attempt in (0, 1):
            try:
                self._ensure()
                self.sock.sendall((json.dumps(payload) + "\n").encode())
                return self._read_line()
            except (OSError, RuntimeError):
                if attempt == 1:
                    raise
                self.sock = None
        raise RuntimeError("unreachable")  # noqa: unreachable


# (tool name, control cmd, kind, required params, optional params, help)
# kind "local" = answered by the bridge itself, not the socket.
TOOLS = [
    ("engine_ping", "ping", "socket", [], [],
     "Health check: round-trips the engine control socket."),
    ("scene_snapshot", None, "local", [], [],
     "Authoritative engine state (the host welcome snapshot). Re-read live."),
    ("scene_list_objects", "list_objects", "socket", [], [],
     "List scene objects (count + objects[] with id/name). Telemetry base."),
    ("scene_get_object", "get_object", "socket", ["oid"], [],
     "One object's properties by numeric id."),
    ("scene_schema", "schema", "socket", [], [],
     "Property schema: which keys each object kind accepts."),
    ("scene_spawn_cube", "spawn_cube", "socket", [], ["x", "y", "z", "size"],
     "Spawn a cube. Defaults x=y=z=0, size=1."),
    ("scene_set_property", "set_property", "socket", ["object", "key"],
     ["x", "y", "z", "value"],
     "Set a property: vec3 needs x+y+z, number uses x, bool/string uses value."),
    ("scene_destroy_object", "destroy_object", "socket", ["oid"], [],
     "Destroy an object by numeric id."),
    ("scene_undo", "undo", "socket", [], [],
     "Undo the last session edit."),
    ("scene_redo", "redo", "socket", [], [],
     "Redo the last undone session edit."),
    ("scene_select", "select", "socket", [], ["oid"],
     "Mirror editor selection (oid=0 deselects)."),
    ("sim_step", "step", "socket", [], ["ticks"],
     "Advance the deterministic sim (default 1 tick; fixed dt)."),
    ("sim_pause", "pause", "socket", [], [],
     "Pause the host frame loop (step still works)."),
    ("sim_resume", "resume", "socket", [], [],
     "Resume the host frame loop."),
    ("document_save", "save_document", "socket", ["path"], [],
     "Save the document to a host-side path."),
    ("document_load", "load_document", "socket", ["path"], [],
     "Load a document from a host-side path (hash-verified)."),
    ("graph_get", "get_graph", "socket", [], [],
     "Full node-graph JSON (nodes, links, params)."),
    ("graph_add_node", "add_node", "socket", ["type"], ["x", "y"],
     "Add a node of a type (e.g. const_number, script:<module>)."),
    ("graph_remove_node", "remove_node", "socket", ["nid"], [],
     "Remove a node by id."),
    ("graph_link", "link_nodes", "socket", ["from", "to"], ["out", "in"],
     "Link two node pins (default pins out=value, in=a)."),
    ("graph_unlink", "unlink_nodes", "socket", ["nid"], ["in"],
     "Remove the link into a node's input pin."),
    ("graph_set_param", "set_node_param", "socket", ["nid", "key"], ["x", "value"],
     "Set a node param: numeric uses x, bool/string uses value."),
    ("graph_set_position", "set_node_position", "socket", ["nid"], ["x", "y"],
     "Move a node card in the editor canvas."),
    ("graph_list_bindings", "list_bindings", "socket", [], [],
     "Graph->scene bindings (which node pin drives which property)."),
    ("graph_bind", "bind_node_property", "socket", ["node", "object", "out", "property"],
     ["node_alias"],
     "Bind a node out-pin to a scene property (axis suffix like position.y allowed)."),
    ("graph_unbind", "unbind_node_property", "socket", ["object", "property"], [],
     "Remove the binding driving a scene property."),
    ("replay_scrub_info", "scrub_info", "socket", [], [],
     "Checkpoint ring state (checkpoints[], capacity, start)."),
    ("replay_scrub_start", "scrub_start", "socket", [], ["frame"],
     "Checkpoint the document timeline at a sim frame (default 0)."),
    ("replay_scrub_to", "scrub_to", "socket", ["frame"], [],
     "Warp the document timeline back to a checkpointed frame."),
    ("replay_capture_status", "capture_status", "socket", [], [],
     "Record/replay status (recording, frame, commands, checkpoints, path)."),
    ("replay_start_capture", "start_capture", "socket", [], ["frame"],
     "Begin recording commands + opening checkpoint (default frame 0)."),
    ("replay_stop_capture", "stop_capture", "socket", ["path"], [],
     "Write the recording atomically to a host-side path and stop."),
    ("replay_load", "load_replay", "socket", ["path"], [],
     "Hydrate checkpoints + re-apply a command log in sequence order."),
    ("clip_add", "clip_add", "socket", ["name"], ["frame", "length"],
     "Add a timeline clip (default start 0, length 0). Replies with its id."),
    ("clip_remove", "clip_remove", "socket", ["clip"], [],
     "Remove a timeline clip by id."),
    ("clip_move", "clip_move", "socket", ["clip"], ["frame"],
     "Move a clip to a new start frame."),
    ("clip_record", "clip_record", "socket", ["clip", "oid", "key"], [],
     "Arm sample capture (property key, axis form like position.x)."),
    ("clip_record_stop", "clip_record_stop", "socket", [], [],
     "Disarm sample capture."),
    ("clip_play", "clip_play", "socket", ["clip"], ["frame"],
     "Arm playback (default frame = clip start)."),
    ("clip_stop", "clip_stop", "socket", [], [],
     "Disarm playback."),
    ("clips_info", "clips_info", "socket", [], [],
     "All clips with tracks (id, name, start_frame, tracks)."),
    ("camera_set", "set_camera", "socket", [], ["ex", "ey", "ez", "tx", "ty", "tz", "fov"],
     "Host-owned orbit camera (viewport; rejected headless)."),
    ("sun_set", "set_sun", "socket", [], ["x", "y", "z"],
     "Host-owned sun direction (viewport; rejected headless)."),
    ("render_get_state", None, "local", [], [],
     "Best-effort render state: mode, bloom, RT toggles, PT samples/mean. "
     "Hosts report only what they own; missing pieces are listed, not hidden."),
    ("render_set_mode", "set_render_mode", "socket", ["mode"], [],
     "Host-owned visual stack: forward | rt | pt (viewport only)."),
    ("render_set_exposure", "set_exposure", "socket", ["exposure"], [],
     "Host-owned tonemap exposure multiplier (viewport only)."),
    ("render_set_bloom", "set_bloom", "socket", ["enabled"], [],
     "Host-owned bloom toggle, 0/1 (viewport only)."),
    ("render_set_rt_reflections", "set_rt_reflections", "socket", ["enabled"], [],
     "Host-owned RT reflections toggle, 0/1 (viewport only)."),
    ("render_set_rt_ao", "set_rt_ao", "socket", ["enabled"], [],
     "Host-owned RT ambient-occlusion toggle, 0/1 (viewport only)."),
    ("render_set_pt_sample_target", "set_pt_sample_target", "socket", ["samples"], [],
     "Stepped path tracing: accumulate exactly K frames, 0=unbounded (viewport only)."),
    ("render_set_viewport_size", "set_viewport_size", "socket", ["width", "height"], [],
     "Drive the swapchain recreate path on demand (viewport only)."),
    ("telemetry_read_log", None, "local", ["dir"], ["limit"],
     "Collect file telemetry: read <dir>/telemetry.jsonl (manifest + last N "
     "frames, default 5) written by a WARPLOOM_TELEMETRY_DIR viewport run."),
    ("engine_raw", None, "local", ["cmd"], ["params"],
     "Escape hatch: forward an arbitrary control-socket command with a "
     "params object (e.g. {\"cmd\":\"capture\",\"params\":{\"frame\":5}}). "
     "Forwards-compatible with future protocol versions."),
]

TOOL_MAP = {name: (cmd, kind, req, opt, help)
            for (name, cmd, kind, req, opt, help) in TOOLS}

INT_PARAMS = {"id", "oid", "nid", "clip", "from", "to", "ticks", "frame",
              "length", "width", "height", "samples", "enabled", "limit",
              "node", "object"}


def coerce_arg(name, value):
    """MCP clients send numbers as int/float/str; the wire wants numbers."""
    if name in INT_PARAMS and isinstance(value, str):
        try:
            return int(value)
        except ValueError:
            try:
                return float(value)
            except ValueError:
                return value
    return value


def tool_defs():
    defs = []
    for (name, _cmd, _kind, req, opt, help) in TOOLS:
        props = {}
        for p in req + opt:
            props[p] = {"description": p}
        defs.append({"name": name, "description": help,
                     "inputSchema": {"type": "object",
                                     "properties": props,
                                     "required": req}})
    return defs


def snapshot_of(engine):
    engine._ensure()
    return engine.welcome


def call_tool(engine, name, args):
    if name not in TOOL_MAP:
        raise ValueError(f"unknown tool: {name}")
    cmd, kind, req, _opt, _help = TOOL_MAP[name]
    missing = [p for p in req if p not in args]
    if missing:
        raise ValueError(f"missing required params: {missing}")
    clean = {k: coerce_arg(k, v) for k, v in args.items()
             if not (isinstance(v, str) and v == "")}

    if kind == "local" and cmd is None:
        if name == "scene_snapshot":
            return {"welcome": snapshot_of(engine)}
        if name == "render_get_state":
            state = {"ok_queries": {}, "missing": []}
            for probe in ("get_render_mode", "get_bloom",
                          "get_rt_reflections", "get_rt_ao",
                          "get_pt_samples", "get_pt_mean"):
                try:
                    reply = engine.cmd(probe)
                except OSError as exc:
                    state["missing"].append({probe: f"transport: {exc}"})
                    continue
                if reply.get("ok"):
                    state["ok_queries"][probe] = reply.get("detail")
                else:
                    state["missing"].append({probe: reply.get("error")})
            return state
        if name == "telemetry_read_log":
            import pathlib
            log = pathlib.Path(clean["dir"]) / "telemetry.jsonl"
            if not log.is_file():
                raise ValueError(f"no telemetry.jsonl in {clean['dir']}")
            limit = int(clean.get("limit", 5))
            manifest, frames = None, []
            with open(log, encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if record.get("type") == "manifest" and manifest is None:
                        manifest = record
                    elif record.get("type") == "frame":
                        frames.append(record)
            return {"manifest": manifest,
                    "frame_count": len(frames),
                    "tail_frames": frames[-limit:] if limit > 0 else []}
        if name == "engine_raw":
            raw_cmd = clean["cmd"]
            params = clean.get("params", {})
            if not isinstance(params, dict):
                raise ValueError("params must be an object")
            reply = engine.cmd(raw_cmd, **params)
            if not reply.get("ok"):
                raise RuntimeError(
                    f"engine rejected {raw_cmd}: {reply.get('error')}")
            detail = reply.get("detail", "")
            try:
                return {"detail": json.loads(detail)}
            except (json.JSONDecodeError, TypeError):
                return {"detail": detail}
        raise ValueError(f"local tool {name} has no handler")

    reply = engine.cmd(cmd, **clean)
    if not reply.get("ok"):
        raise RuntimeError(f"engine rejected {cmd}: {reply.get('error')}")
    detail = reply.get("detail", "")
    try:
        return {"detail": json.loads(detail)}
    except (json.JSONDecodeError, TypeError):
        return {"detail": detail}


def respond(req_id, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": req_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result if result is not None else {}
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def err(code, message, data=None):
    payload = {"code": code, "message": message}
    if data is not None:
        payload["data"] = str(data)
    return payload


def serve(sock_path):
    engine = Engine(sock_path)
    initialized = {"seen_init": False}
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError as exc:
            respond(None, error=err(-32700, f"parse error: {exc}"))
            continue
        method = req.get("method", "")
        req_id = req.get("id")
        params = req.get("params", {}) or {}

        if method == "initialize":
            initialized["seen_init"] = True
            respond(req_id, result={
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": SERVER_INFO})
        elif method in ("notifications/initialized", "notifications/cancelled"):
            continue  # notification: no reply
        elif method == "ping":
            respond(req_id, result={"ok": True})
        elif method == "tools/list":
            cursor = params.get("cursor")
            if cursor is not None:
                respond(req_id, error=err(-32602, "pagination unsupported"))
                continue
            respond(req_id, result={"tools": tool_defs()})
        elif method == "tools/call":
            name = params.get("name", "")
            args = params.get("arguments", {}) or {}
            if not isinstance(args, dict):
                respond(req_id, error=err(-32602, "arguments must be an object"))
                continue
            try:
                out = call_tool(engine, name, args)
                respond(req_id, result={
                    "content": [{"type": "text",
                                 "text": json.dumps(out, indent=2)}],
                    "isError": False})
            except ValueError as exc:
                respond(req_id, result={
                    "content": [{"type": "text", "text": f"error: {exc}"}],
                    "isError": True})
            except RuntimeError as exc:
                respond(req_id, result={
                    "content": [{"type": "text", "text": f"error: {exc}"}],
                    "isError": True})
            except OSError as exc:
                respond(req_id, result={
                    "content": [{"type": "text",
                                 "text": f"transport error: {exc} "
                                         f"(sock={engine.path})"}],
                    "isError": True})
        else:
            if req_id is None:
                continue
            respond(req_id, error=err(-32601, f"unknown method: {method}"))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Warploom MCP stdio bridge (stdlib only).")
    parser.add_argument("--sock", default=None,
                        help="engine control socket path "
                             "(else $WARPLOOM_SOCK / $WARPLOOM_CONTROL_SOCKET / "
                             "$OMNICPP_CONTROL_SOCKET / /tmp/warploom.sock)")
    args = parser.parse_args(argv)
    serve(resolve_sock(args.sock))


if __name__ == "__main__":
    main()
