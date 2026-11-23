#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Python 3.14 + the current raylib (6.2) build into build/ninja. "py311" picks the legacy Python 3.11, "rl55" the
# legacy raylib 5.5 (in this order); each adds a suffix: build/ninja-py311, build/ninja-rl55, build/ninja-py311-rl55.
# A raylib bump changes RL_VERSION and the legacy argument here, in build.bat and the places SPEC §0.1 lists.
PY_VERSION="3.14"
RL_VERSION="6.2"
BUILD_DIR="${SCRIPT_DIR}/build/ninja"
if [[ "${1:-}" == "py311" ]]; then
    PY_VERSION="3.11"
    BUILD_DIR="${BUILD_DIR}-py311"
    shift
fi
if [[ "${1:-}" == "rl55" ]]; then
    RL_VERSION="5.5"
    BUILD_DIR="${BUILD_DIR}-rl55"
    shift
fi
BUILD_TYPE="${1:-Release}"

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
fi

# The raylib version is passed every time: a cache variable's default never reaches an existing build tree.
echo "[IYI] Configuring (${BUILD_TYPE}, Python ${PY_VERSION}, raylib ${RL_VERSION}) with ${GENERATOR}..."
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DIYI_SHIPPING_BUILD=OFF -DIYI_PYTHON_VERSION="${PY_VERSION}" -DIYI_RAYLIB_VERSION="${RL_VERSION}"

echo "[IYI] Building IYIEngine (${BUILD_TYPE})..."
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}"

BIN_PATH="${BUILD_DIR}/IYIEngine"
if [[ -f "${BIN_PATH}" ]]; then
    echo "[IYI] Build successful: ${BIN_PATH}"
else
    echo "[ERROR] Output binary not found at ${BIN_PATH}"
    exit 1
fi