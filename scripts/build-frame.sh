#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${project_dir}/build-frame"
architecture="$(uname -m)"

if [[ "${architecture}" != "aarch64" && "${architecture}" != "arm64" && "${FRAME_ALLOW_NON_AARCH64:-0}" != "1" ]]; then
    echo "error: build-frame.sh must run on AArch64 (detected ${architecture})." >&2
    echo "Set FRAME_ALLOW_NON_AARCH64=1 only for a deliberate secondary-platform build." >&2
    exit 1
fi

for command_name in cmake; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "error: required command not found: ${command_name}" >&2
        exit 1
    fi
done

if [[ -z "${OPENVR_SDK_ROOT:-}" && -z "${OPENVR_INCLUDE_DIR:-}" && \
      "${FRAME_NOTIFY_FETCH_OPENVR:-ON}" != "OFF" ]] && ! command -v git >/dev/null 2>&1; then
    echo "error: git is required when fetching the pinned OpenVR SDK" >&2
    exit 1
fi

cmake_args=(
    -S "${project_dir}"
    -B "${build_dir}"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=ON
)

if [[ -n "${OPENVR_SDK_ROOT:-}" ]]; then
    cmake_args+=("-DOPENVR_SDK_ROOT=${OPENVR_SDK_ROOT}")
fi
if [[ -n "${OPENVR_INCLUDE_DIR:-}" ]]; then
    cmake_args+=("-DOPENVR_INCLUDE_DIR=${OPENVR_INCLUDE_DIR}")
fi
if [[ -n "${OPENVR_LIBRARY:-}" ]]; then
    cmake_args+=("-DOPENVR_LIBRARY=${OPENVR_LIBRARY}")
fi
if [[ -n "${FRAME_NOTIFY_FETCH_OPENVR:-}" ]]; then
    cmake_args+=("-DFRAME_NOTIFY_FETCH_OPENVR=${FRAME_NOTIFY_FETCH_OPENVR}")
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure

echo
echo "Built: ${build_dir}/frame-notify"
if command -v file >/dev/null 2>&1; then
    file "${build_dir}/frame-notify"
fi
