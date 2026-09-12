#!/usr/bin/env bash
# Fetch the CC0 Sponza landmark (Khronos glTF sample) into assets/models/sponza.
# Total ~53 MB: Sponza.gltf + Sponza.bin + 69 textures + LICENSE.md.
# The scene activates with OMNICPP_SPONZA=1 on the city viewport
# (examples/viewport). License: assets/models/sponza/LICENSE.md (CC0 1.0).
set -euo pipefail

DEST="$(cd "$(dirname "$0")/.." && pwd)/assets/models/sponza"
BASE="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Sponza/glTF"
REPO="https://github.com/KhronosGroup/glTF-Sample-Assets"

mkdir -p "$DEST"
export DEST  # the URI-extraction python heredoc reads it

fetch() { # fetch <remote-name> <dest-name>
  local out="$DEST/$2"
  if [ -s "$out" ]; then
    echo "have $2"
    return 0
  fi
  echo "fetch $2"
  curl -fsSL --retry 3 -o "$out" "$BASE/$1"
}

fetch Sponza.gltf Sponza.gltf
fetch Sponza.bin Sponza.bin
fetch LICENSE.md LICENSE.md

# The document references images by their source filenames; download each and
# keep the original names so Sponza.gltf resolves them unmodified.
names="$(python3 - <<'PY'
import json, sys
with open(f"{__import__('os').environ['DEST']}/Sponza.gltf") as f:
    doc = json.load(f)
for img in doc.get("images", []):
    uri = img.get("uri", "")
    if uri and not uri.startswith("data:"):
        print(uri)
PY
)"

for n in $names; do
  fetch "$n" "$n"
done

echo "Sponza landmark ready in $DEST ($(ls "$DEST" | wc -l) files)."
echo "Run: OMNICPP_SCENE=city OMNICPP_SPONZA=1 ./build/vulkan-validation/bin/omnicpp_viewport"
