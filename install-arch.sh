#!/usr/bin/env bash
#
# WiVRn NX — Arch Linux installer.
#
# Installs the PC-side server + dashboard as a proper pacman package built from
# this fork (via the bundled PKGBUILD), so it can be removed later with
#   sudo pacman -R wivrn-nx-git
#
# The headset side is the Android APK from the GitHub releases, not this script.
#
# Usage:  ./install-arch.sh [--no-setcap] [--yes]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUR_DIR="$HERE/packaging/aur"
DO_SETCAP=1
NOCONFIRM=""

for arg in "$@"; do
	case "$arg" in
		--no-setcap) DO_SETCAP=0 ;;
		--yes|-y)    NOCONFIRM="--noconfirm" ;;
		-h|--help)
			cat <<'EOF'
WiVRn NX — Arch Linux installer

Installs the PC server + dashboard as a pacman package built from this fork.
Remove later with:  sudo pacman -R wivrn-nx-git
The headset side is the Android APK from the GitHub releases, not this script.

Usage:  ./install-arch.sh [options]
  --no-setcap   don't grant cap_sys_nice to wivrn-server
  -y, --yes     pass --noconfirm to pacman/makepkg (non-interactive)
  -h, --help    show this
EOF
			exit 0 ;;
		*) echo "Unknown option: $arg" >&2; exit 2 ;;
	esac
done

err() { printf '\033[1;31m==> %s\033[0m\n' "$*" >&2; }
msg() { printf '\033[1;35m==> %s\033[0m\n' "$*"; }   # #7700FF-ish violet

# --- sanity ---------------------------------------------------------------
command -v pacman >/dev/null 2>&1 || { err "This installer is for Arch-based distros (pacman not found)."; exit 1; }
[[ -f "$AUR_DIR/PKGBUILD" ]] || { err "PKGBUILD not found at $AUR_DIR — run this from a checkout of the fork."; exit 1; }
if [[ $EUID -eq 0 ]]; then err "Do not run as root; makepkg refuses to. It will sudo for the parts that need it."; exit 1; fi

# --- dependencies ---------------------------------------------------------
# Build + runtime deps for the server and the KDE/Qt dashboard. base-devel
# provides makepkg, gcc, make, etc.
DEPS=(
	git base-devel cmake ninja extra-cmake-modules glslang gettext
	vulkan-headers vulkan-icd-loader eigen cli11 nlohmann-json boost boost-libs
	openssl ffmpeg libdrm x264 libpipewire pipewire avahi glib2 libnotify
	librsvg libarchive libpng
	qt6-base qt6-declarative qt6-multimedia qt6-tools
	kirigami ki18n kcoreaddons qqc2-desktop-style kiconthemes qcoro-qt6
)

msg "Installing build and runtime dependencies (sudo)…"
sudo pacman -S --needed $NOCONFIRM "${DEPS[@]}"

# --- build + install ------------------------------------------------------
# makepkg clones the fork, fetches Monado/Boost via the build, compiles the
# server + dashboard, and installs the resulting package with pacman.
msg "Building and installing wivrn-nx-git (this pulls Monado and takes a while)…"
cd "$AUR_DIR"
# -s syncs any still-missing deps, -i installs, -f overwrites a prior build,
# -C cleans the build dir first so a re-run starts fresh.
makepkg -sicf $NOCONFIRM

# --- real-time scheduling -------------------------------------------------
# The compositor thread benefits from CAP_SYS_NICE (distro packages grant it);
# without it, frame pacing competes with the rest of the desktop.
if [[ $DO_SETCAP -eq 1 ]]; then
	SRV="$(command -v wivrn-server || true)"
	if [[ -n "$SRV" ]]; then
		msg "Granting cap_sys_nice to $SRV for smoother frame pacing (sudo)…"
		sudo setcap cap_sys_nice+ep "$SRV" || err "setcap failed (non-fatal); frame pacing may be slightly less smooth."
	fi
fi

msg "Done. Launch \"WiVRn NX server\" from your app menu, or run: wivrn-dashboard"
msg "Install the matching WiVRn NX APK on the headset from the GitHub releases."
echo
echo "Remove later with:  sudo pacman -R wivrn-nx-git"
