#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# NOTE: modes (3840x2160@60 / 3840x2400@60) + connectors (DP-1/eDP-1) are for the
#       DS1 dev box (GNOME/Mutter Wayland). Adjust for your hardware. See linux/NATIVE_WAYLAND.md.
# DS1-only: laptop (eDP-1) OFF, DP-1 (DS1) @1.0 primary. Restore: ~/restore-dual-monitor.sh
SER=$(gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig \
      --method org.gnome.Mutter.DisplayConfig.GetCurrentState | sed -n 's/^(uint32 \([0-9]*\).*/\1/p')
gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig \
  --method org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig "$SER" 2 \
  "[(0, 0, 1.0, uint32 0, true, [('DP-1', '3840x2160@60.000', @a{sv} {})])]" "@a{sv} {}"
