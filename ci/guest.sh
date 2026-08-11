#!/bin/bash
# Guest side of the microVM legs. A script rather than an inline
# `vng -- <command>` because vng flattens its command argv and re-parses
# it through a shell, which loses quoting and expands $? before the
# guest sees it.
set -x

root=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$root" || exit 1
status="$root/vm-status"
expect="${1:-}"

if [ -n "$expect" ]; then
	case "$(uname -r)" in
	"$expect".*) ;;
	*)
		echo "guest kernel $(uname -r) is not ${expect}.x" >&2
		echo 1 >"$status"
		exit 1
		;;
	esac
fi

# Stop udev: it opens each freshly added /dev/ublkbN to probe it, and if
# that probe is still in flight when the iteration tears the device down,
# del_gendisk waits for udev's opener while the opener waits for IO no
# server is left to serve. That deadlocks STOP_DEV on *every* kernel
# (seen at iteration ~52 on 6.14) and would masquerade as the bug under
# test. devtmpfs still creates the device nodes without udev.
systemctl stop systemd-udevd.service systemd-udevd-kernel.socket \
	systemd-udevd-control.socket 2>/dev/null || pkill -f udevd || true
sleep 1

if ! modprobe ublk_drv; then
	echo "modprobe ublk_drv failed" >&2
	echo 1 >"$status"
	exit 1
fi

# The watchdog, not the job timeout, is what turns a wedge into a
# failure: on the affected kernel the guest can stop making progress
# entirely.
# The watchdog also writes $status itself: once the kernel is wedged a
# thread can sit in D state and process exit never returns here.
./ublk_churn_repro 3000 30 "$status"
echo $? >"$status"
