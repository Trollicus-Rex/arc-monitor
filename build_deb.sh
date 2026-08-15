#!/bin/bash
set -e

PACKAGE_NAME="arc-monitor"
VERSION="1.0"
ARCH="amd64"
PKG_DIR="${PACKAGE_NAME}_${VERSION}_${ARCH}"

echo "Compiling C binaries..."
mkdir -p build && cd build
cmake .. && make
cd ..

echo "Creating .deb staging directories..."
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/usr/bin"
mkdir -p "$PKG_DIR/lib/systemd/system"
mkdir -p "$PKG_DIR/usr/share/cinnamon/applets"

echo "Copying files to staging..."
cp build/arcd "$PKG_DIR/usr/bin/"
cp build/arc-cli "$PKG_DIR/usr/bin/"
cp -r arc-monitor@local "$PKG_DIR/usr/share/cinnamon/applets/"

echo "Creating Systemd Service File..."
cat << 'SYS' > "$PKG_DIR/lib/systemd/system/arcd.service"
[Unit]
Description=Intel Arc Multi-Card Telemetry Daemon
After=network.target dbus.service

[Service]
Type=simple
ExecStart=/usr/bin/arcd
Restart=always
User=root

[Install]
WantedBy=multi-user.target
SYS

echo "Creating DEBIAN/control..."
cat << DEB > "$PKG_DIR/DEBIAN/control"
Package: $PACKAGE_NAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: libc6, libsystemd0, level-zero, cinnamon
Maintainer: Community <example@example.com>
Description: Steve ArcMonitor & Telemetry Daemon
 A bare-metal hardware discovery, unprivileged telemetry, and device 
 management suite for Intel Arc (Alchemist & Battlemage) GPUs on Linux.
DEB

echo "Creating DEBIAN/postinst..."
cat << 'POST' > "$PKG_DIR/DEBIAN/postinst"
#!/bin/bash
systemctl daemon-reload
systemctl enable --now arcd.service
exit 0
POST
chmod 755 "$PKG_DIR/DEBIAN/postinst"

echo "Creating DEBIAN/prerm..."
cat << 'PRE' > "$PKG_DIR/DEBIAN/prerm"
#!/bin/bash
systemctl stop arcd.service || true
systemctl disable arcd.service || true
exit 0
PRE
chmod 755 "$PKG_DIR/DEBIAN/prerm"

echo "Building the .deb package..."
dpkg-deb --build "$PKG_DIR"

rm -rf "$PKG_DIR"
echo "Done! Created: ${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
