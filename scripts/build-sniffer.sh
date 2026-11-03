#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
python3 "$ROOT/scripts/bootstrap.py"
IMAGE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["idf_image"])' "$ROOT/vendor.lock.json")"
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$ROOT:/project" -w /project/lora-sniffer "$IMAGE" idf.py "$@" build
