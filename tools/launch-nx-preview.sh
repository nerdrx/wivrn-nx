#!/usr/bin/env bash
set -eu
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
dashboard_binary="${NX_DASHBOARD_BINARY:-$project_dir/build-server/server/wivrn-dashboard}"
if [[ ! -x "$dashboard_binary" ]]; then
    echo "Build wivrn-dashboard first, or set NX_DASHBOARD_BINARY to your build." >&2
    exit 1
fi
export NX_DASHBOARD_PREVIEW=1
exec "$dashboard_binary"
