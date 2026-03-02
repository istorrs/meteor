#!/bin/bash
set -euo pipefail

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "Downloading ASTAP..."
wget -q --show-progress -O "$TMPDIR/astap.deb" \
    "https://sourceforge.net/projects/astap-program/files/linux_installer/astap_amd64.deb/download"

echo "Downloading W08 wide-field star database..."
wget -q --show-progress -O "$TMPDIR/w08.deb" \
    "https://sourceforge.net/projects/astap-program/files/star_databases/w08_star_database_mag08_astap.deb/download"

echo "Installing..."
sudo dpkg -i "$TMPDIR/astap.deb"
sudo dpkg -i "$TMPDIR/w08.deb"

echo "Done. Testing:"
astap -h | head -3
