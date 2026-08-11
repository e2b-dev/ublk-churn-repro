# ublk churn wedge repro

Minimal reproducer for a `ublk_drv` control-command hang seen on
Ubuntu's HWE **7.0** kernel:

```
7.0.0-28-generic (linux-image-generic-hwe-24.04, noble)
UBLK_U_CMD_START_DEV never completes; the caller blocks forever in
io_uring_enter and every later ublk control command wedges behind it
```

The program is a miniature ublk server built on raw `io_uring`
(`IORING_OP_URING_CMD`, SQE128). It does not depend on `ublk-go` or
liburing. Each iteration is one full device lifecycle:

1. `UBLK_U_CMD_ADD_DEV`
2. `UBLK_U_CMD_SET_PARAMS`
3. a worker thread submits `UBLK_U_IO_FETCH_REQ` for every tag and then
   blocks in `io_uring_enter(GETEVENTS)`, servicing IOs with
   `UBLK_U_IO_COMMIT_AND_FETCH_REQ`
4. `UBLK_U_CMD_START_DEV`
5. cancel the worker, close the char fd, `UBLK_U_CMD_STOP_DEV`,
   `UBLK_U_CMD_DEL_DEV`

Two details are load-bearing and worth knowing before you simplify it:

- **Two threads are required.** A `URING_CMD` completion is task work
  bound to the submitting task, so a single-threaded version deadlocks
  by construction: the thread issuing `START_DEV` is the only one that
  could run the FETCH task work the kernel is waiting for.
- **The worker must actually service IOs.** `START_DEV` calls
  `add_disk()`, whose partition scan reads the new disk; those reads
  must be completed by the worker before `START_DEV` returns. A worker
  that only waits deadlocks the kernel in
  `blk_add_partitions → read_part_sector`.

A per-iteration `alarm()` watchdog turns the hang into a non-zero exit
naming the iteration and phase, so CI reports a failure instead of
timing out silently.

## Run

```sh
sudo modprobe ublk_drv
make
sudo ./ublk_churn_repro            # 5000 iterations, 30s watchdog
sudo ./ublk_churn_repro 20000 30   # iterations, watchdog seconds
```

Exit codes: `0` all iterations completed, `1` a command returned an
error, `3` the watchdog fired (the wedge).

## Known results

| Kernel | Result |
| --- | --- |
| `7.0.0-28-generic` (noble HWE) | **wedges** — `START_DEV` never completes |
| `6.14` (noble HWE series) | passes |
| `6.17.0-7-generic` | passes (1500 iterations, ~21/s) |
| `6.18` (Ubuntu 26.04) | passes |
| `6.8.0-137-generic` (noble GA) | passes here, but see note |

Note on 6.8: this loop passes, but 6.8 is separately affected by
CVE-2025-37906 (`ublk: fix race between io_uring_cmd_complete_in_task
and ublk_cancel_cmd`, fixed upstream in v6.15, never backported to
noble's 6.8 GA kernel), which panics that kernel under close-under-load
with in-flight IO. That is a different bug from the one reproduced here.

See `REPORT.md` for the bug-report text.

## CI

`.github/workflows/kernels.yml` runs the reproducer across kernels in a
`virtme-ng` microVM on a `ubuntu-24.04` runner, plus directly on the
`ubuntu-26.04` runner's own 6.18 host kernel. Because a wedge can take
the guest down, each leg relies on the watchdog exit rather than the
job timeout.
