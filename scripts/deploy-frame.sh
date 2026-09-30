#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${FRAME_HOST:-}" ]]; then
    echo "error: set FRAME_HOST, for example FRAME_HOST=steamos@frame" >&2
    exit 1
fi

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
remote_dir="${FRAME_REMOTE_DIR:-frame-notify}"

if ! command -v rsync >/dev/null 2>&1; then
    echo "error: rsync is required for deployment" >&2
    exit 1
fi

ssh "${FRAME_HOST}" "mkdir -p ~/${remote_dir}"
rsync -az \
    --exclude '/.git/' \
    --exclude '/build*/' \
    "${project_dir}/" "${FRAME_HOST}:~/${remote_dir}/"
ssh "${FRAME_HOST}" "chmod +x ~/${remote_dir}/scripts/build-frame.sh ~/${remote_dir}/scripts/deploy-frame.sh"

echo "Sources deployed to ${FRAME_HOST}:~/${remote_dir}/"
echo "Build on the Frame with:"
echo "  ssh -t ${FRAME_HOST} 'cd ~/${remote_dir} && ./scripts/build-frame.sh'"
