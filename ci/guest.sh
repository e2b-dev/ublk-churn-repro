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
