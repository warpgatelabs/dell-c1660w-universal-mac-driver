#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Remove c1660w-universal-mac-driver. Run with sudo. With --purge, also delete its saved printers,
# settings and logs in /Library/Application Support/c1660w-universal-mac-driver.
#
# macOS printer queues that point at the app (ipp://localhost:8631/...) are left in place;
# remove them in System Settings > Printers & Scanners.

LABEL=com.warpgate.c1660w-universal-mac-driver
if [ "$(/usr/bin/id -u)" != 0 ]; then
    echo "uninstall: run with sudo" >&2
    exit 1
fi
/bin/launchctl bootout "system/$LABEL" 2>/dev/null
/bin/rm -f "/Library/LaunchDaemons/$LABEL.plist" /usr/local/bin/c1660w-universal-mac-driver
/bin/rm -f /usr/local/share/c1660w-universal-mac-driver/licenses/*
/bin/rmdir /usr/local/share/c1660w-universal-mac-driver/licenses 2>/dev/null
/bin/rm -f /usr/local/share/c1660w-universal-mac-driver/*
/bin/rmdir /usr/local/share/c1660w-universal-mac-driver 2>/dev/null
if [ "${1:-}" = --purge ]; then
    /bin/rm -rf "/Library/Application Support/c1660w-universal-mac-driver"
fi
/usr/sbin/pkgutil --forget "$LABEL" >/dev/null 2>&1
echo "c1660w-universal-mac-driver removed."
