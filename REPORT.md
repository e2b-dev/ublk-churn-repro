# Bug report draft

Subject: ublk: START_DEV never completes under repeated device
create/start/stop on 7.0.0-28-generic

## Summary

On Ubuntu's HWE 7.0 kernel (`7.0.0-28-generic`), a loop that repeatedly
creates, starts, stops and deletes a ublk device eventually hangs: a
`UBLK_U_CMD_START_DEV` uring_cmd never completes. The issuing thread
stays blocked in `io_uring_enter` waiting for a CQE that is never
posted. Every subsequent ublk control command from any process then
hangs as well, which is consistent with the handler never releasing
`ublk_ctl_mutex`.

The hang is silent: userspace waits interruptibly, so there is no Oops,
no `WARN`, and no hung-task report on the console.

## Affected / unaffected

| Kernel | Result |
| --- | --- |
| `7.0.0-28-generic` (Ubuntu noble HWE, linux-meta-hwe-7.0) | hangs |
| Ubuntu noble HWE 6.14 series | not reproduced |
| `6.17.0-7-generic` | not reproduced |
| Ubuntu 26.04 stock 6.18 | not reproduced |

All runs used the same reproducer, the same userspace, and (for the
6.8/6.14/7.0 rows) the same `virtme-ng` microVM harness, so the
difference is the kernel and not the environment.

## Reproducer

Standalone C, raw io_uring, no liburing dependency:
<https://github.com/e2b-dev/ublk-churn-repro>

```sh
sudo modprobe ublk_drv
make
sudo ./ublk_churn_repro 5000 30
```

Each iteration performs a complete device lifecycle: `ADD_DEV`,
`SET_PARAMS`, a worker thread that submits `FETCH_REQ` for every tag
and services IOs with `COMMIT_AND_FETCH_REQ`, `START_DEV`, then
worker cancel, char-fd close, `STOP_DEV`, `DEL_DEV`. A per-iteration
`alarm()` watchdog reports the stuck phase and exits 3.

Expected output on an unaffected kernel:

```
PASS: 5000 create/start/stop iterations completed without a wedge
```

On the affected kernel:

```
WEDGED: iteration N stuck in START_DEV past the watchdog interval;
no CQE for that command -- kernel bug
```

## Original observation

Found by the integration suite of a Go ublk server
(<https://github.com/e2b-dev/ublk-go>), whose goroutine dump on the
affected kernel showed the stall inside the control-ring
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

- Reproduced 2 of 2 attempts on 7.0 under the Go suite; the C
  reproducer is the reduction of that workload.
- Related but distinct: noble's 6.8 GA kernel is affected by
  CVE-2025-37906, which panics under close-under-load with in-flight
  IO. That is fixed in v6.15 and is not this bug.
