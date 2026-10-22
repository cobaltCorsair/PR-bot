#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ENV_PREFIX="${SCRIPT_DIR}/.env"
DRIVER_PATH="${ENV_PREFIX}/bin/chromedriver"

if [[ ! -x "${ENV_PREFIX}/bin/cmake" ]]; then
    echo "Create the local build environment first with ./cpp/build.sh" >&2
    exit 1
fi

if [[ -n "${CHROME_BIN:-}" ]]; then
    CHROME_BINARY="${CHROME_BIN}"
elif [[ -x "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" ]]; then
    CHROME_BINARY="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
elif command -v google-chrome >/dev/null 2>&1; then
    CHROME_BINARY="$(command -v google-chrome)"
elif command -v chromium >/dev/null 2>&1; then
    CHROME_BINARY="$(command -v chromium)"
elif command -v chromium-browser >/dev/null 2>&1; then
    CHROME_BINARY="$(command -v chromium-browser)"
else
    echo "Chrome/Chromium was not found. Set CHROME_BIN to its executable path." >&2
    exit 1
fi

if [[ ! -x "${CHROME_BINARY}" ]]; then
    echo "Browser executable is not runnable: ${CHROME_BINARY}" >&2
    exit 1
fi

CHROME_VERSION="$("${CHROME_BINARY}" --version | sed -E 's/.* ([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
if [[ ! "${CHROME_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Could not read a Chrome version from: ${CHROME_BINARY}" >&2
    exit 1
fi
CHROME_BUILD="${CHROME_VERSION%.*}"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) PLATFORM="mac-arm64" ;;
    Darwin-x86_64) PLATFORM="mac-x64" ;;
    Linux-aarch64) PLATFORM="linux-arm64" ;;
    Linux-x86_64) PLATFORM="linux64" ;;
    *)
        echo "Unsupported platform: $(uname -s)-$(uname -m)" >&2
        exit 1
        ;;
esac

if [[ -x "${DRIVER_PATH}" ]]; then
    CURRENT_VERSION="$(${DRIVER_PATH} --version | sed -E 's/^ChromeDriver ([0-9.]+).*/\1/')"
    if [[ "${CURRENT_VERSION%.*}" == "${CHROME_BUILD}" ]]; then
        echo "Chrome ${CHROME_VERSION} already has a compatible ChromeDriver ${CURRENT_VERSION} at ${DRIVER_PATH}"
        exit 0
    fi
fi

METADATA_URL="https://googlechromelabs.github.io/chrome-for-testing/latest-patch-versions-per-build.json"
DRIVER_VERSION="$(curl -fsSL "${METADATA_URL}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["builds"].get(sys.argv[1], {}).get("version", ""))' "${CHROME_BUILD}")"
if [[ -z "${DRIVER_VERSION}" ]]; then
    echo "Chrome for Testing has no ChromeDriver entry for build ${CHROME_BUILD}" >&2
    exit 1
fi

ARCHIVE_URL="https://storage.googleapis.com/chrome-for-testing-public/${DRIVER_VERSION}/${PLATFORM}/chromedriver-${PLATFORM}.zip"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TEMP_DIR}"' EXIT
curl -fsSL "${ARCHIVE_URL}" -o "${TEMP_DIR}/chromedriver.zip"
unzip -p "${TEMP_DIR}/chromedriver.zip" "chromedriver-${PLATFORM}/chromedriver" > "${TEMP_DIR}/chromedriver"
chmod 0755 "${TEMP_DIR}/chromedriver"
mv -f "${TEMP_DIR}/chromedriver" "${DRIVER_PATH}"

INSTALLED_VERSION="$(${DRIVER_PATH} --version | sed -E 's/^ChromeDriver ([0-9.]+).*/\1/')"
if [[ "${INSTALLED_VERSION%.*}" != "${CHROME_BUILD}" ]]; then
    echo "Installed ChromeDriver ${INSTALLED_VERSION} does not match Chrome build ${CHROME_BUILD}" >&2
    exit 1
fi

echo "Installed ChromeDriver ${INSTALLED_VERSION} for Chrome ${CHROME_VERSION} at ${DRIVER_PATH}"
