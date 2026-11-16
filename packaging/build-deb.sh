#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the firmware and the limeboard-io-firmware Debian package into dist/.
# The version is the one src/version.h reports over USB; CROSS selects the
# toolchain (arm-none-eabi- by default).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
VERSION=$(awk '/define FIRMWARE_VERSION/ { v = substr($3, 3); printf "%d.%d", substr(v, 1, 2), substr(v, 3, 2) }' src/version.h)
TAG=$(git describe --tags --exact-match 2>/dev/null || true)
if [ -n "$TAG" ] && [ "${TAG#v}" != "$VERSION" ]; then
	echo "ERROR: tag $TAG but src/version.h says $VERSION" >&2
	exit 1
fi

make clean >/dev/null
make CROSS="${CROSS:-arm-none-eabi-}" >/dev/null

DIST="$ROOT/dist"
P=$(mktemp -d)
trap 'rm -rf "$P"' EXIT
chmod 0755 "$P"
mkdir -p "$DIST"
install -D -m 0644 build/limeboard-io.bin "$P/usr/share/limeboard/limeboard-io.bin"
echo "$VERSION" > "$P/usr/share/limeboard/limeboard-io.version"
mkdir -p "$P/usr/share/doc/limeboard-io-firmware" "$P/DEBIAN"
{
	echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
	echo "Upstream-Name: limeboard-firmware"
	echo "Source: https://github.com/Citronics/limeboard-firmware"
	echo
	echo "Files: *"
	echo "Copyright: 2026 Citronics"
	echo "License: MIT"
	echo
	echo "Files: lib/tinyusb/*"
	echo "Copyright: 2018 hathach (tinyusb.org)"
	echo "License: MIT"
	echo
	echo "Files: lib/cmsis-core/* lib/cmsis-device-f4/*"
	echo "Copyright: Arm Limited, STMicroelectronics"
	echo "License: Apache-2.0"
	echo
	echo "License: MIT"
	sed 's/^$/./; s/^/ /' LICENSE | tail -n +2
	echo
	echo "License: Apache-2.0"
	echo " On Debian systems, the full text of the Apache License 2.0 is in"
	echo " /usr/share/common-licenses/Apache-2.0."
} > "$P/usr/share/doc/limeboard-io-firmware/copyright"
cat > "$P/DEBIAN/control" <<CONTROL
Package: limeboard-io-firmware
Version: $VERSION
Architecture: all
Maintainer: Citronics <info@citronics.eu>
Section: misc
Priority: optional
Homepage: https://github.com/Citronics/limeboard-firmware
Description: Limeboard I/O firmware for the Limeboard's STM32F401
 The firmware that makes the Limeboard's Raspberry Pi style header appear in
 Linux as a GPIO chip, I2C and SPI buses, an ADC and a serial port (DLN-2
 protocol over USB). limeboard-io installs it on the STM32.
CONTROL
cat > "$P/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
# SPDX-License-Identifier: MIT
# A newer firmware goes onto the STM32 now, not only at the next boot.
set -e
if [ "$1" = configure ] && [ -d /run/systemd/system ] && \
   systemctl cat limeboard-firmware-update.service >/dev/null 2>&1; then
	systemctl --no-block start limeboard-firmware-update.service || true
fi
exit 0
POSTINST
chmod 0755 "$P/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$P" "$DIST/limeboard-io-firmware_${VERSION}_all.deb"
cp build/limeboard-io.bin "$DIST/limeboard-io-${VERSION}.bin"
ls -l "$DIST"
