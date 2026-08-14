#!/bin/bash
# Churn the *upstream reference* ublk server (ublk-org/ublksrv), not
# ublk-go: add a device, do one small write, delete it, repeat. If this
# wedges, the bug cannot be in ublk-go.
#
#   $1  iterations (default 300)
#   $2  per-step watchdog seconds (default 30)
set -u

UBLK=${UBLK:-/tmp/ublksrv/ublk}
iters=${1:-300}
wd=${2:-30}

cleanup() { timeout 20 "$UBLK" del -a >/dev/null 2>&1 || true; }
trap cleanup EXIT

start=$(date +%s)
for ((i = 0; i < iters; i++)); do
	# add
	if ! out=$(timeout "$wd" "$UBLK" add -t null -q 1 -d 128 2>&1); then
		echo "WEDGED: iteration $i stuck or failed in 'ublk add' after ${wd}s"
		echo "$out" | tail -3
		exit 3
	fi
	id=$(echo "$out" | sed -n 's/^dev id \([0-9]*\):.*/\1/p' | head -1)
	if [ -z "$id" ]; then
		echo "iteration $i: could not parse dev id from: $out"
		exit 1
	fi

	# one small write through the block device, like the Go churn test
	if [ -b "/dev/ublkb$id" ]; then
		timeout "$wd" dd if=/dev/zero of="/dev/ublkb$id" bs=4096 count=1 \
			conv=fsync status=none 2>/dev/null || true
	fi

	# delete
	if ! timeout "$wd" "$UBLK" del -n "$id" >/dev/null 2>&1; then
		echo "WEDGED: iteration $i stuck or failed in 'ublk del' after ${wd}s"
		exit 3
	fi

	if (((i + 1) % 50 == 0)); then
		el=$(($(date +%s) - start))
		echo "iter $((i + 1))/$iters ok (${el}s)"
	fi
done

echo "PASS: $iters ublksrv add/del iterations completed without a wedge"
