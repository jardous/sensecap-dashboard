#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
cmake -S desktop -B build-desktop -DCMAKE_BUILD_TYPE=Debug
cmake --build build-desktop --parallel 8
mkdir -p docs/previews
for page in climate network reticulum lora lora-simulated lora-settings-connection lora-settings-receiver unavailable; do
  args=(--simulate --page 0)
  case "$page" in
    network) args=(--simulate --page 1);;
    reticulum) args=(--unavailable --page 2);;
    lora) args=(--unavailable --page 3);;
    lora-simulated) args=(--simulate --page 3);;
    lora-settings-connection) args=(--simulate --settings connection);;
    lora-settings-receiver) args=(--simulate --settings receiver);;
    unavailable) args=(--unavailable --page 0);;
  esac
  build-desktop/simulator "${args[@]}" --screenshot "build-desktop/$page.ppm"
  python3 scripts/ppm_to_png.py "build-desktop/$page.ppm" "docs/previews/$page.png"
done
