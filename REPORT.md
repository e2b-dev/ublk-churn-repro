# Bug report draft

Subject: ublk: START_DEV never completes under repeated device
create/start/stop (6.18 and 7.0)

## Summary

A loop that repeatedly creates, starts, stops and deletes a ublk device
eventually hangs: a `UBLK_U_CMD_START_DEV` uring_cmd never completes.
The issuing thread stays blocked in `io_uring_enter` waiting for a CQE
that is never posted. Every subsequent ublk control command from any
process then hangs as well, which is consistent with the handler never
releasing `ublk_ctl_mutex`. Recovery requires a reboot.

The hang is silent: userspace waits interruptibly, so there is no Oops,
no `WARN`, and no hung-task report on the console.

`ublk_ctrl_start_dev()` blocks in
`wait_for_completion_interruptible(&ub->completion)`, waiting for every
queue to report its FETCH commands ready, so the failure looks like a
lost wakeup of that completion — the server has submitted its
`UBLK_U_IO_FETCH_REQ` for every tag by then.

## Affected

Measured with an in-tree Go test looping create → 4 KiB write → close,
run alone and serialized, ten repeats per kernel:

| Kernel | Wedged | Iteration at wedge |
| --- | --- | --- |
| mainline `v6.18.40` | 4 of 6 | 77, 118, 125, 200 |
| Ubuntu noble HWE `7.0.0-28-generic` | 6 of 6 | 12, 32, 37, 95, 184, 248 |
| Ubuntu noble HWE 6.14 series | not reproduced | — |
| `6.17.0-7-generic` | not reproduced (1500 iterations) | — |
| Ubuntu 26.04 azure `7.0.0-1011` | not reproduced | — |

At roughly 20 cycles/s, "iteration 77" is about four seconds of churn.
Concurrency accelerates it: with the rest of the test suite running in
parallel, 7.0 wedged on the first iteration. The same runs on 6.18 also
produced spurious `ENOSPC` on in-range block writes (a 3.6 MB write to a
16 MB device), which may be the same underlying problem.

All rows used the same reproducer and the same `virtme-ng` microVM
harness, so the difference is the kernel, not the environment.

## Reproducer

The reliable reproducer is a Go ublk server's integration test:

```sh
git clone https://github.com/e2b-dev/ublk-go && cd ublk-go/test
go test -c -race -tags=integration -o /tmp/ublk.test .
sudo /tmp/ublk.test -test.v -test.run=TestChurnLiveness -test.count=10
```

It loops create → 4 KiB write → close for 8 s per repeat and reports the
iteration and phase on a stall. Wedge output:

```
churn wedged at iteration 118 in stage "New" (1m0s past budget)
```

### A minimal C server does NOT reproduce it

<https://github.com/e2b-dev/ublk-churn-repro> is a standalone C ublk
server (raw io_uring, no liburing) performing the same command sequence.
It ran **3000 iterations clean on every kernel tested**, including the
ones where the Go server wedges within tens of iterations. That is
offered as a narrowing clue, not a reproducer.

It was made to match the Go server on everything cheap to match:
one queue, queue depth 128, 128 KiB max IO, the same device flags
(`UBLK_F_CMD_IOCTL_ENCODE` plus `UBLK_F_UPDATE_SIZE` when
`GET_FEATURES` reports it), a worker thread that submits `FETCH_REQ`
for every tag and services IOs, and the same teardown order. None of
those changed the outcome.

Known remaining differences, any of which may be the trigger:

- The Go server registers the char fd as an io_uring **fixed file** and
  submits FETCH/COMMIT with `IOSQE_FIXED_FILE`. The C version cannot:
  the registration holds a reference to the char device that outlives
  `close()` of the char fd (dropped only by the ring's async exit work),
  so `ublk_ch_release` never runs and `DEL_DEV` deadlocks immediately —
  on every kernel.
- The Go runtime delivers `SIGURG` preemption signals constantly, so its
  `io_uring_enter` calls are interrupted and restarted far more often.
- Its worker runs `LockOSThread` pinned to the queue's CPUs.

udev must be stopped when running the C version: udev opens each freshly
added `/dev/ublkbN` to probe it, and if that probe is in flight at
teardown, `del_gendisk` waits for udev's opener while the opener waits
for IO no server is left to serve. That deadlocks `STOP_DEV` on every
kernel including unaffected ones, and is a separate userspace-ordering
issue, not this bug.

```sh
sudo modprobe ublk_drv
make
sudo ./ublk_churn_repro 5000 30
```

Each C iteration is a complete device lifecycle: `ADD_DEV`, `SET_PARAMS`,
a worker thread that submits `FETCH_REQ` for every tag and services IOs
with `COMMIT_AND_FETCH_REQ`, `START_DEV`, then worker cancel, char-fd
close, `STOP_DEV`, `DEL_DEV`. A per-iteration `alarm()` watchdog reports
the stuck phase and exits 3.

Two details of the reproducer are deliberate and required:

- Two threads. A `URING_CMD` completion is task work bound to the
  submitting task, so a single-threaded version deadlocks by
  construction: the thread issuing `START_DEV` would be the only one
  able to run the FETCH task work it is waiting for.
- udev is stopped in the test VM. udev opens each freshly added
  `/dev/ublkbN` to probe it, and if that probe is in flight at teardown,
  `del_gendisk` waits for udev's opener while the opener waits for IO no
  server is left to serve. That deadlocks `STOP_DEV` on every kernel
  including unaffected ones, and is a separate userspace-ordering issue,
  not this bug.

Expected output on an unaffected kernel:

```
PASS: 5000 create/start/stop iterations completed without a wedge
```

On an affected kernel:

```
WEDGED: iteration N stuck in START_DEV past the watchdog interval;
no CQE for that command
```

## Original observation

Found by the integration suite of a Go ublk server
(<https://github.com/e2b-dev/ublk-go>), whose goroutine dump on the
affected kernels showed the stall inside the control-ring
submit-and-wait for `START_DEV`:

```
uring.(*Ring).SubmitAndWaitCQE
ublk.(*Device).ctrlCommand(..., 0xc0207506 /* START_DEV */, ...)
ublk.(*Device).startDev
ublk.New
```

A later, independent `New` in the same process wedged in the identical
stack, i.e. the device state is unrecoverable process-wide once it
happens.

## Notes

- No userspace workaround found. The loop is already serial (one device
  at a time, one thread), so serializing device creation does not help;
  a serialized full test suite hit its timeout on every round. A stuck
  command also cannot be cancelled or deadlined from userspace, since
  the wait happens in an io-wq worker.
- Related but distinct: noble's 6.8 GA kernel is affected by
  CVE-2025-37906, which panics under close-under-load with in-flight IO.
  That is fixed in v6.15 and is not this bug.
