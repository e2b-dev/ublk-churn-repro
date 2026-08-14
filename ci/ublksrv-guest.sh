#!/bin/bash
# Guest side of the ublksrv legs: churn the *upstream reference* server
# (ublk-org/ublksrv) instead of ublk-go. This is the test that decides
# whether the START_DEV wedge is a kernel bug or a bug in ublk-go — if
# the ublk maintainer's own implementation wedges, it is not ours.
#
# udev is deliberately left running here, matching the environment in
# which ublk-go wedges.
#
#   $1  expected kernel series
#   $2  iterations
set -x

root=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$root" || exit 1
status="$root/vm-status"
expect="${1:-}"
iters="${2:-600}"

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

UBLK="$root/ublksrv/ublk" ci/ublksrv-churn.sh "$iters" 30
echo $? >"$status"
