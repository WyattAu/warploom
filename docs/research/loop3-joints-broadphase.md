# Research loop 3: joints/constraints and broadphase alternatives
"simulate almost any mechanical system stably and accurately" with XPBD -
distance, ball-and-socket, hinge, fixed joints; rather than a bespoke solver
snapshot too). Deterministic: fixed joint insertion order (Jolt's advice -
blazing fast implementation"; Morton codes to BVH) and 23 (Sweep and
Prune); tutorial 11 (spatial hashing; the method our grid already uses);
- Our uniform spatial hash: O(1) insertion, cell-size tuned per scene -
until the next rebuild; our grid recomputes cell membership per step from