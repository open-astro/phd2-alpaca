#!/usr/bin/env bash
# PHD2 .deb Build Script for Ubuntu / Kubuntu / Debian / Raspberry Pi OS
#
# Builds PHD2 and creates a .deb package. Run on the target architecture:
# - On PC (x86_64): produces phd2_*_amd64.deb for Ubuntu/Kubuntu
# - On Raspberry Pi (armv7l/aarch64): produces phd2_*_armhf.deb or phd2_*_arm64.deb
#
# See: https://github.com/OpenPHDGuiding/phd2/wiki/BuildingPHD2OnLinux
#
# INDI: PHD2 requires libindi-dev >= 2.0. On Ubuntu 22.04/24.04 and Debian
# Bookworm, add the INDI PPA first, then install deps:
#   sudo add-apt-repository ppa:mutlaqja/ppa
#   sudo apt-get update
#   sudo apt-get install -y libindi-dev  # then other build deps or use --install-deps

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }
step()  { echo -e "${CYAN}[STEP]${NC} $*"; }

# Project root (directory containing this script)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$ROOT_DIR"

# ---------------------------------------------------------------------------
# Extract version from src/phd.h (for display; package version is in debian/changelog)
# ---------------------------------------------------------------------------
step "Extracting version from src/phd.h..."
PHD_H="${ROOT_DIR}/src/phd.h"
if [[ ! -f "$PHD_H" ]]; then
    err "Cannot find src/phd.h at: $PHD_H"
fi

PHDVERSION=$(grep -E '^\s*#define\s+PHDVERSION\s+_T\("([^"]*)"\)' "$PHD_H" | sed -E 's/.*_T\("([^"]*)"\).*/\1/')
PHDSUBVER=$(grep -E '^\s*#define\s+PHDSUBVER\s+_T\("([^"]*)"\)' "$PHD_H" | sed -E 's/.*_T\("([^"]*)"\).*/\1/')
FULL_VERSION="${PHDVERSION}${PHDSUBVER}"
info "Detected version: ${FULL_VERSION}"

# ---------------------------------------------------------------------------
# Build dependencies (from debian/control and PHD2 Linux wiki)
# One of the wx packages is required (3.2 on newer Ubuntu, 3.0 on RPi/older)
# ---------------------------------------------------------------------------
BUILD_DEPS_CORE=(
    build-essential cmake pkg-config debhelper
    libcfitsio-dev libopencv-dev libusb-1.0-0-dev libudev-dev libv4l-dev
    libnova-dev libcurl4-gnutls-dev libindi-dev libeigen3-dev libgtest-dev
    gettext zlib1g-dev
)
BUILD_DEPS_WX=(libwxgtk3.2-dev libwxgtk3.0-dev libwxgtk3.0-gtk3-dev)

check_deps() {
    local missing=()
    for pkg in "${BUILD_DEPS_CORE[@]}"; do
        dpkg -s "$pkg" &>/dev/null || missing+=("$pkg")
    done
    local has_wx=false
    for pkg in "${BUILD_DEPS_WX[@]}"; do
        dpkg -s "$pkg" &>/dev/null && { has_wx=true; break; }
    done
    $has_wx || missing+=("libwxgtk3.2-dev or libwxgtk3.0-dev")
    # Check libindi-dev version (PHD2 needs >= 2.0)
    local indi_ver
    indi_ver=$(dpkg -s libindi-dev 2>/dev/null | awk '/^Version:/ { print $2 }')
    if [[ -n "$indi_ver" ]]; then
        if dpkg --compare-versions "$indi_ver" lt 2.0 2>/dev/null; then
            missing+=("libindi-dev (>= 2.0, you have $indi_ver)")
        fi
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        warn "Missing build dependencies: ${missing[*]}"
        echo ""
        # Debian with only libindi-dev too old: short message, deps already installed
        if [[ -f /etc/os-release ]] && grep -q '^ID=debian' /etc/os-release 2>/dev/null && \
           [[ ${#missing[@]} -eq 1 ]] && [[ "${missing[0]}" == *"libindi-dev"* ]]; then
            echo "PHD2 wants libindi-dev >= 2.0; you have ${indi_ver:-unknown}."
            echo "All other build deps are installed."
            echo ""
            echo "Run: $0 --force   to try building with your current libindi-dev."
            echo "(Or build INDI 2.x from source from indilib.org if the build fails.)"
            return 1
        fi
        if [[ -f /etc/os-release ]] && grep -q '^ID=debian' /etc/os-release 2>/dev/null; then
            echo "PHD2 requires libindi-dev >= 2.0. On Debian the INDI PPA does not apply."
            echo "Options: build INDI 2.x from source (e.g. from indilib.org), or try: $0 --force"
            echo ""
        else
            echo "PHD2 requires libindi-dev >= 2.0. On Ubuntu, add the INDI PPA first:"
            echo "  sudo add-apt-repository ppa:mutlaqja/ppa"
            echo "  sudo apt-get update"
            echo ""
        fi
        echo "Then install build deps (Ubuntu 22.04+ / Debian Bookworm/Trixie):"
        echo "  sudo apt-get install -y build-essential cmake pkg-config debhelper \\"
        echo "    libwxgtk3.2-dev libcfitsio-dev libopencv-dev libusb-1.0-0-dev \\"
        echo "    libudev-dev libv4l-dev libnova-dev libcurl4-gnutls-dev \\"
        echo "    libindi-dev libeigen3-dev libgtest-dev gettext zlib1g-dev"
        echo ""
        echo "On Raspberry Pi OS or older distros, use libwxgtk3.0-dev instead of libwxgtk3.2-dev."
        if [[ -f /etc/os-release ]] && grep -q '^ID=debian' /etc/os-release 2>/dev/null; then
            echo "Or run: $0 --install-deps  to install available deps, or $0 --force to try anyway."
        else
            echo "Or run: $0 --install-deps  (after adding the INDI PPA on Ubuntu if needed)"
        fi
        return 1
    fi
    return 0
}

install_deps() {
    step "Installing build dependencies..."
    indi_ver=$(dpkg -s libindi-dev 2>/dev/null | awk '/^Version:/ { print $2 }')
    if [[ -z "$indi_ver" ]] || dpkg --compare-versions "$indi_ver" lt 2.0 2>/dev/null; then
        # PPA is Ubuntu-only; skip on Debian (add-apt-repository not available)
        if [[ -f /etc/os-release ]] && grep -q '^ID=ubuntu' /etc/os-release 2>/dev/null && command -v add-apt-repository &>/dev/null; then
            echo ""
            echo "PHD2 needs libindi-dev >= 2.0. On Ubuntu, add the INDI PPA first:"
            echo "  sudo add-apt-repository ppa:mutlaqja/ppa"
            echo "  sudo apt-get update"
            echo ""
            read -r -p "Add INDI PPA now? [y/N] " reply
            if [[ "${reply,,}" =~ ^y ]]; then
                sudo add-apt-repository -y ppa:mutlaqja/ppa
                sudo apt-get update
            fi
        elif [[ -f /etc/os-release ]] && grep -q '^ID=debian' /etc/os-release 2>/dev/null; then
            echo ""
            echo "On Debian, libindi-dev >= 2.0 may not be in the repos. Installing what is available."
            echo "If the build fails, build INDI 2.x from source or use: $0 --force"
            echo ""
        fi
    fi
    sudo apt-get update
    if sudo apt-get install -y build-essential cmake pkg-config debhelper \
        libwxgtk3.2-dev libcfitsio-dev libopencv-dev libusb-1.0-0-dev \
        libudev-dev libv4l-dev libnova-dev libcurl4-gnutls-dev \
        libindi-dev libeigen3-dev libgtest-dev gettext zlib1g-dev 2>/dev/null; then
        info "Build dependencies installed (wx 3.2)."
    else
        info "Trying with wx 3.0 (e.g. RPi / older distro)..."
        sudo apt-get install -y build-essential cmake pkg-config debhelper \
            libwxgtk3.0-dev libcfitsio-dev libopencv-dev libusb-1.0-0-dev \
            libudev-dev libv4l-dev libnova-dev libcurl4-gnutls-dev \
            libindi-dev libeigen3-dev libgtest-dev gettext zlib1g-dev
        info "Build dependencies installed (wx 3.0)."
    fi
}

# ---------------------------------------------------------------------------
# Parse options
# ---------------------------------------------------------------------------
INSTALL_DEPS=false
CLEAN=false
FORCE=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true ;;
        --clean)        CLEAN=true ;;
        --force)        FORCE=true ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Build PHD2 and create a .deb package for the current architecture."
            echo "Result: phd2_<version>_<arch>.deb in the project root parent directory."
            echo ""
            echo "Options:"
            echo "  --install-deps   Install build dependencies (sudo apt-get) then exit."
            echo "  --clean          Remove build artifacts before building."
            echo "  --force          Skip build-dependency check (e.g. try with libindi-dev < 2.0 on Debian)."
            echo "  -h, --help       Show this help."
            echo ""
            echo "Examples:"
            echo "  $0                  # Build .deb (deps must be installed)"
            echo "  $0 --install-deps   # Install deps only"
            echo "  $0 --clean          # Clean and build"
            echo "  $0 --force          # Build anyway (ignore unmet dep check)"
            exit 0
            ;;
        *) err "Unknown option: $1"; ;;
    esac
    shift
done

if "$INSTALL_DEPS"; then
    install_deps
    exit 0
fi

# ---------------------------------------------------------------------------
# Check we're on a Debian-based system
# ---------------------------------------------------------------------------
if ! command -v dpkg-buildpackage &>/dev/null; then
    err "dpkg-buildpackage not found. Install packaging tools: sudo apt-get install dpkg-dev debhelper"
fi

if ! "$FORCE"; then
    if ! check_deps; then
        err "Install dependencies and re-run, or use: $0 --install-deps  (or $0 --force to try anyway)"
    fi
else
    warn "Skipping build dependency check (--force). Build may fail if libindi-dev < 2.0."
fi

# ---------------------------------------------------------------------------
# Clean if requested
# ---------------------------------------------------------------------------
if "$CLEAN"; then
    step "Cleaning previous build artifacts..."
    rm -rf debian/phd2 debian/.debhelper debian/files debian/phd2.substvars
    dpkg-buildpackage -T clean 2>/dev/null || true
    info "Clean done."
fi

# ---------------------------------------------------------------------------
# Build .deb (uses debian/rules: cmake with USE_SYSTEM_LIBINDI=1, OPENSOURCE_ONLY=1)
# ---------------------------------------------------------------------------
step "Building PHD2 .deb package..."
# -us -uc = do not sign source and changes; -d = allow unmet build deps when --force
if "$FORCE"; then
    export PHD2_ALLOW_INDI_1_9=1
    info "PHD2_ALLOW_INDI_1_9=1: allowing system INDI 1.9.x (use at your own risk)"
    dpkg-buildpackage -us -uc -b -d
else
    dpkg-buildpackage -us -uc -b
fi

# ---------------------------------------------------------------------------
# Report result
# ---------------------------------------------------------------------------
PARENT_DIR="$(dirname "$ROOT_DIR")"
DEB=$(find "$PARENT_DIR" -maxdepth 1 -name "phd2_*_*.deb" -type f 2>/dev/null | head -1)
if [[ -n "$DEB" && -f "$DEB" ]]; then
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}.deb built successfully${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "  ${CYAN}$DEB${NC}"
    ls -la "$DEB"
    echo ""
    echo "Install with: sudo dpkg -i $DEB"
    echo "  (resolve deps if needed: sudo apt-get install -f)"
else
    err ".deb not found in $PARENT_DIR"
fi
