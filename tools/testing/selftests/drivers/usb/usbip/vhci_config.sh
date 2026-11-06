#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise idle VHCI pool replacement without an exported USB device.

driver=/sys/bus/platform/drivers/vhci_hcd
helper=$(dirname "$0")/vhci_config_helper
loaded=0
restore=0
saved=
testno=0
failed=0

skip()
{
	printf '1..0 # SKIP %s\n' "$1"
	exit 4
}

idle()
{
	# Unlike the driver's links, this list includes unbound controllers.
	for device in /sys/bus/platform/devices/vhci_hcd.*; do
		status=$device/status
		if [ ! -r "$status" ]; then
			# Older kernels expose other controllers only through zero.
			status=$driver/vhci_hcd.0/status.${device##*.}
		fi
		[ -r "$status" ] || return 1
		awk 'NR > 1 && $3 != 4 { busy = 1 }
		     END { exit busy }' "$status" || return 1
	done
}

geometry()
{
	[ "$(cat "$driver/configuration")" = "$1 $2" ] || return 1
	[ "$(cat "$driver/nports")" = "$((2 * $1 * $2))" ] || return 1
	devices=0
	for device in "$driver"/vhci_hcd.*; do
		[ "$(cat "$device/num_ports")" = "$2" ] || return 1
		id=${device##*.}
		awk -v ports="$2" -v base="$((2 * $2 * id))" '
			NR == 1 { next }
			{
				hub = n < ports ? "hs" : "ss"
				if ($1 != hub || $2 != base + n || $3 != 4)
					bad = 1
				n++
			}
			END { exit bad || n != 2 * ports }
		' "$device/status" || return 1
		devices=$((devices + 1))
	done
	[ "$devices" = "$1" ]
}

configure()
{
	printf '%s %s\n' "$1" "$2" > "$driver/configuration" &&
		geometry "$1" "$2"
}

noop()
{
	# A removed status file cannot be read through its existing descriptor,
	# even when the replacement reuses the same USB bus numbers.
	exec 3< "$driver/vhci_hcd.0/status" || return 1
	configure 1 2 && cat <&3 >/dev/null
	rc=$?
	exec 3<&-
	return "$rc"
}

record()
{
	testno=$((testno + 1))
	if "$@"; then
		printf 'ok %s - %s\n' "$testno" "$*"
	else
		printf 'not ok %s - %s\n' "$testno" "$*"
		failed=1
	fi
}

malformed()
{
	before=$(cat "$driver/configuration") || return 1
	for request in '0 0' '0 1' '1 0' '1 16' '-1 1' '1 -1' \
		'2147483648 1' '1 4294967297' 'text' '1' '1 1 extra'; do
		if (printf '%s\n' "$request" > "$driver/configuration") 2>/dev/null; then
			return 1
		fi
		[ "$(cat "$driver/configuration")" = "$before" ] || return 1
	done
	geometry 1 2
}

repeated()
{
	i=0
	while [ "$i" -lt 10 ]; do
		configure 1 1 && configure 1 2 || return 1
		i=$((i + 1))
	done
}

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ "$restore" = 1 ]; then
		# Do not unload a module whose original pool could not be restored.
		if ! idle || ! printf '%s\n' "$saved" > "$driver/configuration" ||
			[ "$(cat "$driver/configuration")" != "$saved" ]; then
			printf '# Could not restore VHCI configuration: %s\n' "$saved"
			loaded=0
			rc=1
		fi
	fi
	if [ "$loaded" = 1 ]; then
		if ! idle || ! modprobe -r vhci_hcd; then
			printf '# Could not unload test-owned vhci_hcd\n'
			rc=1
		fi
	fi
	exit "$rc"
}

printf 'TAP version 13\n'
[ "$(id -u)" = 0 ] || skip 'root is required'
command -v modprobe >/dev/null 2>&1 || skip 'modprobe is required'
[ -x "$helper" ] || skip 'build vhci_config_helper first'
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
if [ ! -d /sys/module/vhci_hcd ]; then
	modprobe vhci_hcd 2>/dev/null || skip 'vhci_hcd is unavailable'
	loaded=1
fi
[ -w "$driver/configuration" ] || skip 'runtime configuration is unavailable'
saved=$(cat "$driver/configuration") || skip 'pool replacement is in progress'
set -- $saved
[ "$#" = 2 ] && [ "$1" -gt 0 ] && [ "$2" -gt 0 ] ||
	skip 'no restorable positive configuration'
idle || skip 'VHCI ports are busy or controllers are unbound'
restore=1
printf '1..8\n'
record configure 1 1
record configure 1 2
record noop
record "$helper"
record malformed
record repeated
record configure 2 1
record configure 2 15
exit "$failed"
