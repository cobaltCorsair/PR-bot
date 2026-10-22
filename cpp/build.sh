#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ENV_PREFIX="${SCRIPT_DIR}/.env"
BUILD_DIR="${SCRIPT_DIR}/build"

if [[ -n "${CONDA_EXE:-}" ]]; then
    CONDA_BIN="${CONDA_EXE}"
elif command -v conda >/dev/null 2>&1; then
    CONDA_BIN="$(command -v conda)"
else
    echo "Conda was not found. Install Miniconda or Anaconda, then rerun this script." >&2
    exit 1
fi

if [[ ! -x "${ENV_PREFIX}/bin/cmake" ]]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64) SUBDIR="osx-arm64" ;;
        Darwin-x86_64) SUBDIR="osx-64" ;;
        Linux-aarch64) SUBDIR="linux-aarch64" ;;
        Linux-x86_64) SUBDIR="linux-64" ;;
        *)
            echo "Unsupported platform: $(uname -s)-$(uname -m)" >&2
            exit 1
            ;;
    esac

    mkdir -p "${SCRIPT_DIR}/.conda/pkgs"
    CONDA_PKGS_DIRS="${SCRIPT_DIR}/.conda/pkgs" \
        "${CONDA_BIN}" env create \
            --prefix "${ENV_PREFIX}" \
            --file "${SCRIPT_DIR}/environment.yml" \
            --subdir "${SUBDIR}" \
            --yes
fi

"${CONDA_BIN}" run --prefix "${ENV_PREFIX}" cmake \
    -S "${SCRIPT_DIR}" \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON

"${CONDA_BIN}" run --prefix "${ENV_PREFIX}" cmake \
    --build "${BUILD_DIR}" \
    --parallel

echo "Build complete: ${BUILD_DIR}/prbot"
echo "Run from the repository root with: conda run --prefix '${ENV_PREFIX}' '${BUILD_DIR}/prbot'"
