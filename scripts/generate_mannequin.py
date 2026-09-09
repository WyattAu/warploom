#!/usr/bin/env python3
"""Generate assets/models/mannequin.gltf + mannequin.bin.

A deterministic humanoid test asset for the skeletal pipeline:
  - 15 joints (hips -> spine -> chest -> neck -> head, shoulders/elbows/
    hands, hips/knees),
  - 9 box body-part meshes with JOINTS_0/WEIGHTS_0 single-joint bindings,
  - a 1-second LINEAR walk-cycle animation (leg swing, knee bend, arm
    counter-swing, hip bob) sampled into exact keyframes.

Every accessor is 4-byte aligned per the glTF spec; positions are meters,
y-up, +Z forward. The rest pose reproduces itself exactly under skinning
(skinned vertex = global_rest(j) * inverse(global_rest(j)) * v_bind).
"""

import json
import math
import struct
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent / "assets" / "models"

bin_data = bytearray()
buffer_views = []
accessors = []


def pad():
    while len(bin_data) % 4:
        bin_data.append(0)


def push(data):
    pad()
    offset = len(bin_data)
    bin_data.extend(data)
    buffer_views.append({"buffer": 0, "byteOffset": offset,
                         "byteLength": len(data)})
    return len(buffer_views) - 1


def add_accessor(view, component_type, count, atype, mins=None, maxs=None):
    accessor = {"bufferView": view, "componentType": component_type,
                "count": count, "type": atype}
    if mins is not None:
        accessor["min"] = mins
    if maxs is not None:
        accessor["max"] = maxs
    accessors.append(accessor)
    return len(accessors) - 1


def floats(values):
    return struct.pack("<%df" % len(values), *values)


def u32s(values):
    return struct.pack("<%dI" % len(values), *values)


def u8s(values):
    return bytes(values)


# ---------------------------------------------------------------------------
# Geometry: one axis-aligned box = 24 vertices (4/face with normals) + 36
# indices. Returns (positions, normals, indices) in world space.
# ---------------------------------------------------------------------------

FACES = [
    ((1, 0, 0), ((1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1))),
    ((-1, 0, 0), ((-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1))),
    ((0, 1, 0), ((-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1))),
    ((0, -1, 0), ((-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1))),
    ((0, 0, 1), ((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1))),
    ((0, 0, -1), ((1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1))),
]


def box(center, size, joint):
    cx, cy, cz = center
    sx, sy, sz = size
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    positions, normals, indices = [], [], []
    for normal, corners in FACES:
        base = len(positions)
        for u, v, w in corners:
            positions.append((cx + u * hx, cy + v * hy, cz + w * hz))
            normals.append(normal)
        indices += [base, base + 1, base + 2,
                    base, base + 2, base + 3]
    # Single-joint binding: weights (1,0,0,0).
    return positions, normals, indices, [joint] * 4 * len(positions), \
        [1.0, 0.0, 0.0, 0.0] * len(positions)


PARTS = [
    # name, center, size, joint, material
    ("hips", (0.0, 0.95, 0.0), (0.30, 0.12, 0.17), 0, 2),
    ("torso", (0.0, 1.20, 0.0), (0.34, 0.38, 0.18), 1, 1),
    ("head", (0.0, 1.62, 0.0), (0.22, 0.24, 0.22), 4, 0),
    ("l_upper_arm", (-0.26, 1.26, 0.0), (0.10, 0.30, 0.10), 5, 1),
    ("l_forearm", (-0.26, 0.96, 0.0), (0.09, 0.28, 0.09), 6, 0),
    ("r_upper_arm", (0.26, 1.26, 0.0), (0.10, 0.30, 0.10), 8, 1),
    ("r_forearm", (0.26, 0.96, 0.0), (0.09, 0.28, 0.09), 9, 0),
    ("l_leg", (-0.09, 0.455, 0.0), (0.13, 0.91, 0.13), 11, 2),
    ("r_leg", (0.09, 0.455, 0.0), (0.13, 0.91, 0.13), 13, 2),
]

# ---------------------------------------------------------------------------
# Node hierarchy (15 joints, then one mesh node per part at indices 15..).
# TRS: [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]
# ---------------------------------------------------------------------------

JOINT_NODES = {
    0: ([0.0, 0.95, 0.0], [1, 11, 13]),          # hips
    1: ([0.0, 0.10, 0.0], [2]),                  # spine
    2: ([0.0, 0.18, 0.0], [3, 5, 8]),            # chest
    3: ([0.0, 0.28, 0.0], [4]),                  # neck
    4: ([0.0, 0.08, 0.0], []),                   # head
    5: ([-0.20, 0.22, 0.0], [6]),                # l_shoulder
    6: ([-0.06, -0.26, 0.0], [7]),               # l_elbow
    7: ([0.0, -0.24, 0.0], []),                  # l_hand
    8: ([0.20, 0.22, 0.0], [9]),                 # r_shoulder
    9: ([0.06, -0.26, 0.0], [10]),               # r_elbow
    10: ([0.0, -0.24, 0.0], []),                 # r_hand
    11: ([-0.09, -0.05, 0.0], [12]),             # l_hip
    12: ([0.0, -0.45, 0.0], []),                 # l_knee
    13: ([0.09, -0.05, 0.0], [14]),              # r_hip
    14: ([0.0, -0.45, 0.0], []),                 # r_knee
}

JOINT_NAMES = ["hips", "spine", "chest", "neck", "head",
               "l_shoulder", "l_elbow", "l_hand",
               "r_shoulder", "r_elbow", "r_hand",
               "l_hip", "l_knee", "r_hip", "r_knee"]

# ---------------------------------------------------------------------------
# Column-major 4x4 matrix helpers (matching the engine's GltfTransform).
# ---------------------------------------------------------------------------


def mat_identity():
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def mat_mul(a, b):
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            out[r + 4 * c] = sum(a[r + 4 * k] * b[k + 4 * c] for k in range(4))
    return out


def mat_trs(t, q, s):
    """Compose translation * rotation(xyzw) * scale, column-major."""
    x, y, z, w = q
    m = mat_identity()
    m[0] = (1 - 2 * (y * y + z * z)) * s[0]
    m[1] = 2 * (x * y + z * w) * s[0]
    m[2] = 2 * (x * z - y * w) * s[0]
    m[4] = 2 * (x * y - z * w) * s[1]
    m[5] = (1 - 2 * (x * x + z * z)) * s[1]
    m[6] = 2 * (y * z + x * w) * s[1]
    m[8] = 2 * (x * z + y * w) * s[2]
    m[9] = 2 * (y * z - x * w) * s[2]
    m[10] = (1 - 2 * (x * x + y * y)) * s[2]
    m[12], m[13], m[14] = t
    return m


def mat_inverse(m):
    """Invert a 4x4 column-major matrix (gl-matrix mat4.invert algorithm)."""
    a00, a01, a02, a03 = m[0:4]
    a10, a11, a12, a13 = m[4:8]
    a20, a21, a22, a23 = m[8:12]
    a30, a31, a32, a33 = m[12:16]

    b00 = a00 * a11 - a01 * a10
    b01 = a00 * a12 - a02 * a10
    b02 = a00 * a13 - a03 * a10
    b03 = a01 * a12 - a02 * a11
    b04 = a01 * a13 - a03 * a11
    b05 = a02 * a13 - a03 * a12
    b06 = a20 * a31 - a21 * a30
    b07 = a20 * a32 - a22 * a30
    b08 = a20 * a33 - a23 * a30
    b09 = a21 * a32 - a22 * a31
    b10 = a21 * a33 - a23 * a31
    b11 = a22 * a33 - a23 * a32

    det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + \
        b05 * b06
    if det == 0:
        raise ValueError("singular matrix")

    out = [0.0] * 16
    out[0] = (a11 * b11 - a12 * b10 + a13 * b09) / det
    out[1] = (a02 * b10 - a01 * b11 - a03 * b09) / det
    out[2] = (a31 * b05 - a32 * b04 + a33 * b03) / det
    out[3] = (a22 * b04 - a21 * b05 - a23 * b03) / det
    out[4] = (a12 * b08 - a10 * b11 - a13 * b07) / det
    out[5] = (a00 * b11 - a02 * b08 + a03 * b07) / det
    out[6] = (a32 * b02 - a30 * b05 - a33 * b01) / det
    out[7] = (a20 * b05 - a22 * b02 + a23 * b01) / det
    out[8] = (a10 * b10 - a11 * b08 + a13 * b06) / det
    out[9] = (a01 * b08 - a00 * b10 - a03 * b06) / det
    out[10] = (a30 * b04 - a31 * b02 + a33 * b00) / det
    out[11] = (a21 * b02 - a20 * b04 - a23 * b00) / det
    out[12] = (a11 * b07 - a10 * b09 - a12 * b06) / det
    out[13] = (a00 * b09 - a01 * b07 + a02 * b06) / det
    out[14] = (a31 * b01 - a30 * b03 - a32 * b00) / det
    out[15] = (a20 * b03 - a21 * b01 + a22 * b00) / det
    return out


# ---------------------------------------------------------------------------
# Rest-pose globals -> inverse bind matrices.
# ---------------------------------------------------------------------------

rest_locals = {}
for index, (t, children) in JOINT_NODES.items():
    rest_locals[index] = mat_trs(t, [0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0])

rest_globals = {}
for index in JOINT_NODES:
    chain = []
    cursor = index
    while True:
        chain.append(cursor)
        parent = next((p for p, (_, kids) in JOINT_NODES.items()
                       if cursor in kids), None)
        if parent is None:
            break
        cursor = parent
    m = mat_identity()
    for node in reversed(chain):
        m = mat_mul(m, rest_locals[node])
    rest_globals[index] = m

ibms = [mat_inverse(rest_globals[j]) for j in range(len(JOINT_NODES))]

# ---------------------------------------------------------------------------
# Binary payloads.
# ---------------------------------------------------------------------------

meshes = []
materials = [
    {"name": "skin", "pbrMetallicRoughness":
        {"baseColorFactor": [0.87, 0.72, 0.53, 1.0]}},
    {"name": "shirt", "pbrMetallicRoughness":
        {"baseColorFactor": [0.20, 0.50, 0.80, 1.0]}},
    {"name": "pants", "pbrMetallicRoughness":
        {"baseColorFactor": [0.25, 0.25, 0.30, 1.0]}},
]

for name, center, size, joint, material in PARTS:
    positions, normals, indices, joints, weights = box(center, size, joint)
    count = len(positions)

    pos_flat = [c for p in positions for c in p]
    nrm_flat = [c for n in normals for c in n]
    pos_view = push(floats(pos_flat))
    nrm_view = push(floats(nrm_flat))
    jnt_view = push(u8s(joints))
    wgt_view = push(floats(weights))
    idx_view = push(u32s(indices))

    xs = pos_flat[0::3]
    ys = pos_flat[1::3]
    zs = pos_flat[2::3]
    pos_acc = add_accessor(pos_view, 5126, count, "VEC3",
                           [min(xs), min(ys), min(zs)],
                           [max(xs), max(ys), max(zs)])
    nrm_acc = add_accessor(nrm_view, 5126, count, "VEC3")
    jnt_acc = add_accessor(jnt_view, 5121, count, "VEC4")
    wgt_acc = add_accessor(wgt_view, 5126, count, "VEC4")
    idx_acc = add_accessor(idx_view, 5125, len(indices), "SCALAR")

    meshes.append({
        "name": name,
        "primitives": [{
            "attributes": {
                "POSITION": pos_acc,
                "NORMAL": nrm_acc,
                "JOINTS_0": jnt_acc,
                "WEIGHTS_0": wgt_acc,
            },
            "indices": idx_acc,
            "material": material,
        }],
    })

# Mesh nodes hold world-space bind vertices with identity TRS; skinning
# reproduces the bind pose exactly at rest.
mesh_nodes = []
for mesh_index in range(len(meshes)):
    mesh_nodes.append({
        "mesh": mesh_index,
        "skin": 0,
        "name": PARTS[mesh_index][0] + "_node",
    })

# Inverse bind matrices accessor.
ibm_view = push(floats([v for m in ibms for v in m]))
ibm_acc = add_accessor(ibm_view, 5126, len(ibms), "MAT4")

skin = {
    "name": "mannequin_skin",
    "joints": list(range(len(JOINT_NODES))),
    "skeleton": 0,
    "inverseBindMatrices": ibm_acc,
}

# ---------------------------------------------------------------------------
# Walk-cycle animation (1 s). Quaternions: x-rotation theta -> xyzw.
# ---------------------------------------------------------------------------


def xrot(theta):
    return [math.sin(theta / 2.0), 0.0, 0.0, math.cos(theta / 2.0)]


KEYS = [0.0, 0.25, 0.5, 0.75, 1.0]
LEG_AMPLITUDE = 0.6    # rad hip swing
ARM_AMPLITUDE = 0.4    # rad shoulder counter-swing
KNEE_AMPLITUDE = 0.45  # rad knee bend

samplers = []
channels = []


def add_channel(node, path, times, values):
    stride = len(values[0])
    in_view = push(floats(times))
    in_acc = add_accessor(in_view, 5126, len(times), "SCALAR")
    out_view = push(floats([c for v in values for c in v]))
    out_acc = add_accessor(out_view, 5126, len(values),
                           "VEC4" if stride == 4 else "VEC3")
    samplers.append({"input": in_acc, "output": out_acc,
                     "interpolation": "LINEAR"})
    channels.append({"sampler": len(samplers) - 1,
                     "target": {"node": node, "path": path}})


# Hip bob (translation y dips mid-stride).
add_channel(0, "translation", [0.0, 0.5, 1.0],
            [[0.0, 0.95, 0.0], [0.0, 0.93, 0.0], [0.0, 0.95, 0.0]])

# Leg swing: l_hip forward while r_hip back.
add_channel(11, "rotation", KEYS, [xrot(LEG_AMPLITUDE * math.sin(2 * math.pi * t))
                                   for t in KEYS])
add_channel(13, "rotation", KEYS, [xrot(-LEG_AMPLITUDE * math.sin(2 * math.pi * t))
                                   for t in KEYS])
# Knees bend twice per cycle (one bend per stride).
add_channel(12, "rotation", KEYS,
            [xrot(KNEE_AMPLITUDE * (0.5 - 0.5 * math.cos(4 * math.pi * t)))
             for t in KEYS])
add_channel(14, "rotation", KEYS,
            [xrot(KNEE_AMPLITUDE * (0.5 - 0.5 * math.cos(4 * math.pi * t)))
             for t in KEYS])
# Arms counter-swing against the same-side leg.
add_channel(5, "rotation", KEYS,
            [xrot(-ARM_AMPLITUDE * math.sin(2 * math.pi * t)) for t in KEYS])
add_channel(8, "rotation", KEYS,
            [xrot(ARM_AMPLITUDE * math.sin(2 * math.pi * t)) for t in KEYS])

animation = {
    "name": "walk",
    "samplers": samplers,
    "channels": channels,
}

# ---------------------------------------------------------------------------
# Assemble nodes/document.
# ---------------------------------------------------------------------------

nodes = []
for index in range(len(JOINT_NODES)):
    t, children = JOINT_NODES[index]
    node = {"name": JOINT_NAMES[index], "children": children,
            "translation": t}
    nodes.append(node)
nodes.extend(mesh_nodes)

document = {
    "asset": {"version": "2.0",
              "generator": "omnicpp mannequin generator"},
    "scene": 0,
    "scenes": [{"nodes": [0] + list(range(15, 15 + len(mesh_nodes)))}],
    "nodes": nodes,
    "meshes": meshes,
    "skins": [skin],
    "materials": materials,
    "animations": [animation],
    "buffers": [{"byteLength": len(bin_data), "uri": "mannequin.bin"}],
    "bufferViews": buffer_views,
    "accessors": accessors,
}

OUT_DIR.mkdir(parents=True, exist_ok=True)
(OUT_DIR / "mannequin.bin").write_bytes(bytes(bin_data))
(OUT_DIR / "mannequin.gltf").write_text(json.dumps(document, indent=1))
print("mannequin.gltf + mannequin.bin written:",
      f"{len(meshes)} meshes, {len(JOINT_NODES)} joints, "
      f"{len(channels)} channels, {len(bin_data)} buffer bytes")
