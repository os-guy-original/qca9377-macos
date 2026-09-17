#!/bin/bash
# privileged-battery.sh — QCA9377 driver project, ground-truth battery.
# STRICTLY READ-ONLY: dmesg/lspci/setpci-read/iw-read/dmidecode/rfkill/modinfo/sysfs reads.
# No configuration changes, no module loads/unloads, no interface state changes.
# Invoked via: askroot --reason "..." bash privileged-battery.sh > docs/privileged-ground-truth.txt
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

hdr() { echo; echo "=================================================================="; echo "== $1"; echo "=================================================================="; }

hdr "battery timestamp"
date -Is
uname -a

hdr "dmesg: ath10k / atheros / wlan lines (the trace we must reproduce)"
dmesg | grep -Ei "ath10k|ath9k|atheros|168c:0042|11ad:08a6|wlan0|phy0" || echo "(no matching lines in current ring buffer)"

hdr "dmesg: full buffer (context: ACPI/PCI resource assignment, MCFG, ASPM)"
dmesg

hdr "lspci -nn -vv full: 0000:04:00.0 (capabilities now visible)"
lspci -nn -vv -s 04:00.0

hdr "PCI config space, 4096 bytes raw (lspci -xxxx)"
lspci -xxxx -s 04:00.0

hdr "MSI interrupt vectors in use"
ls -la /sys/bus/pci/devices/0000:04:00.0/msi_irqs/ 2>/dev/null || echo "(no msi_irqs dir)"
grep -E "ath10k|147:" /proc/interrupts | head -10 || true

hdr "dmidecode: bios / system / baseboard / chassis"
dmidecode -t bios -t system -t baseboard -t chassis

hdr "iw phy0: full firmware-reported capabilities (bands, channels, HT/VHT, features)"
iw phy

hdr "iw regulatory domain"
iw reg get

hdr "iw dev wlan0: info / link / station dump"
iw dev wlan0 info
iw dev wlan0 link
iw dev wlan0 station dump 2>/dev/null || true

hdr "iw scan dump (firmware scan cache, capped at 400 lines)"
timeout 20 iw dev wlan0 scan dump 2>/dev/null | head -400 || echo "(scan dump empty or timed out)"

hdr "ethtool: driver identity + link"
ethtool -i wlan0
ethtool wlan0 2>/dev/null | head -30 || true

hdr "rfkill state"
rfkill list

hdr "ath10k_pci module parameters (modinfo -p)"
modinfo -p ath10k_pci || true

hdr "debugfs ath10k stats (if mounted; read-only)"
if [ -d /sys/kernel/debug/ieee80211/phy0/ath10k ]; then
  ls -la /sys/kernel/debug/ieee80211/phy0/ath10k/
  for f in fw_checksum fw_info warm_hw_reset; do
    [ -r "/sys/kernel/debug/ieee80211/phy0/ath10k/$f" ] && { echo "--- $f ---"; head -40 "/sys/kernel/debug/ieee80211/phy0/ath10k/$f"; }
  done
else
  echo "(debugfs ath10k not mounted — fine, optional)"
fi

hdr "battery complete"
