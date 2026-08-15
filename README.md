# ublk churn wedge repro

> **Archived, August 2026 — this never reproduced the bug.** It runs
> clean everywhere, including the exact kernels the hang was measured on
> (mainline **v6.18.40** and **v7.0.14**, 3000 iterations each). Keep it
> only as a record of what was eliminated. The live evidence is in
> [ublk-go](https://github.com/e2b-dev/ublk-go) — `TestChurnLiveness`
> plus the io_uring tracepoints described under "What the bug actually
> is" below. See AGENTS.md there for the full write-up.

Attempted reproducer for a `ublk_drv` control-command hang seen on
Ubuntu's HWE **7.0** kernel:

```
7.0.0-28-generic (linux-image-generic-hwe-24.04, noble)
a ublk control command never completes; the caller blocks forever in
io_uring_enter and every later ublk control command wedges behind it
```

## What the bug actually is

Diagnosed later, from the Go server rather than from this program, so
the framing below (and the original "START_DEV never completes") is
narrower than the truth. Captured hangs hit `ADD_DEV` as well as
`START_DEV` — whichever punted control command happens to be in flight.

At a hang, no thread is inside ublk code at all. The io_uring
tracepoints show the whole story: `io_uring_submit_req` for the command,
`io_uring_queue_async_work` two microseconds later, then silence. Linux
7.0 punts every non-read-only ublk control command to io-wq
(`ublk_ctrl_uring_cmd_may_sleep` returns `-EAGAIN` under
`IO_URING_F_NONBLOCK`), and the punted work is never picked up: no
worker runs it, no completion is posted, and the process's only io-wq
worker belongs to a different task and sits idle in `io_wq_worker`. So
the fault is in io_uring's io-wq, not in `ublk_drv`, which merely
arranges for the punt.

Affected releases, measured with `TestChurnLiveness` (ten runs per
kernel, ~250 create/close cycles each): 6.17 and 6.18.0 clean, **6.18.40
hangs**, 7.0.0 hangs 5 times in 40 runs, **7.0.14 hangs 5 in 10**. It is
the stable point releases that are affected, not the series bases — so
something landed in the 6.18.y range and is still in 7.0.y.

## Why this program never reproduced it

Two server-side differences remain, and one of them was removed from
this program on purpose:

1. **Fixed-file registration.** ublk-go used to register the char fd and
   submit FETCH/COMMIT with `IOSQE_FIXED_FILE`; dropping it took 7.0.0
   from 29 hangs in 40 runs to 5. This program tried it and reverted
   (see the note in `worker_fn`) because the registration's reference to
   the char device outlives `close(cfd)` and deadlocked `DEL_DEV` at
   iteration 0 on every kernel. Reintroducing it with ublk-go's teardown
   ordering is the untested variant.
2. **Concurrency.** This loop is strictly serial. In ublk-go the
   *parallel* integration suite hangs on 7.0 nearly every run, while
   serial churn needs thousands of cycles — so concurrent device
   creation is by far the stronger trigger, and it is the ingredient
   this program lacks entirely.

Neither was pursued: ublk-go reproduces the hang directly and its
tracepoint capture is better evidence than a C loop that passes.

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

## Status: this reproducer never triggered the bug

It runs **3000 iterations clean on every kernel tested**, including ones
where the Go server it was reduced from
(<https://github.com/e2b-dev/ublk-go>, `TestChurnLiveness`) hangs within
tens of iterations. Matching the Go server's queue count, queue depth,
max IO size and device flags did not change that, and neither did
finally testing the two releases that do hang — v6.18.40 and v7.0.14
were added to CI in the last commit and both passed 3000 iterations.
That rules out "wrong kernel" as the explanation; see "Why this program
never reproduced it" above for what is left.

Run it with udevd stopped. udev opens each freshly added `/dev/ublkbN`
to probe it, and if that probe is in flight at teardown, `del_gendisk`
waits for udev's opener while the opener waits for IO no server is left
to serve — deadlocking `STOP_DEV` on every kernel, unaffected ones
included. `ci/guest.sh` stops udevd for that reason.

## Known results

C reproducer, udevd stopped, 3000 iterations each:

| Kernel | Result |
| --- | --- |
| `7.0.0-28-generic` (noble HWE) | passes |
| mainline `v7.0`, `v7.0.12` | passes |
| `6.14` (noble HWE series) | passes |
| `6.17.0-7-generic` | passes (also 1500 iterations locally, ~21/s) |
| `6.8.0-137-generic` (noble GA) | passes, but see note |
| mainline `v6.18.40` | passes — and this is a kernel ublk-go hangs on |
| mainline `v7.0.14` | passes — and this is a kernel ublk-go hangs on |

The last two rows are the decisive ones. Until they were added, every
kernel tested here was one where the hang is mild or absent, because the
mainline tag picker's `sort -V` ranked `v6.18/` above `v6.18.40/` and
silently pinned the leg to the series base release. With that fixed,
this program still passes 3000 iterations on both.

For contrast, `TestChurnLiveness` in the Go server, run alone and
serialized, ten repeats: mainline `6.18.40` wedged 4 of 6 (iterations
77–200) and `7.0.0-28-generic` wedged 6 of 6 (iterations 12–248).

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
