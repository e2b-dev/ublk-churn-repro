// SPDX-License-Identifier: MIT
//
// Minimal ublk churn reproducer.
//
// Runs a tight device create -> start -> stop -> delete loop, using raw
// io_uring (IORING_OP_URING_CMD) with no liburing and no ublk-go
// dependency. Each iteration is a complete miniature ublk server and
// mirrors the real server's threading, which the bug depends on:
//
//   * a per-queue worker thread submits UBLK_U_IO_FETCH_REQ and then
//     blocks in io_uring_enter(GETEVENTS) -- the kernel runs the fetch
//     as task work bound to that thread while it waits;
//   * the main thread issues UBLK_U_CMD_START_DEV on the control ring,
//     which the kernel holds until every queue's fetches are ready.
//
// (A single-threaded version deadlocks by construction -- the thread
// issuing START_DEV is the only one that could run the fetch task work
// -- so the two threads are required to test the kernel, not an
// artifact.)
//
// On a healthy kernel this runs thousands of iterations. On the
// affected kernel a control command (observed: START_DEV) eventually
// never completes: the thread blocks forever in io_uring_enter waiting
// for a CQE that is never posted, and every later ublk control command
// wedges behind the global ublk_ctl_mutex. A per-iteration watchdog
// alarm turns that silent hang into a non-zero exit naming the
// iteration and phase.
//
//   sudo modprobe ublk_drv
//   make
//   sudo ./ublk_churn_repro           # default 5000 iterations
//   sudo ./ublk_churn_repro 20000 30  # iterations, watchdog seconds

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef IORING_OP_URING_CMD
#define IORING_OP_URING_CMD 46
#endif
#ifndef IORING_SETUP_SQE128
#define IORING_SETUP_SQE128 (1U << 10)
#endif
#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS (1U << 0)
#endif
#ifndef IORING_OFF_SQ_RING
#define IORING_OFF_SQ_RING 0x00000000ULL
#endif
#ifndef IORING_OFF_CQ_RING
#define IORING_OFF_CQ_RING 0x08000000ULL
#endif
#ifndef IORING_OFF_SQES
#define IORING_OFF_SQES 0x10000000ULL
#endif

// ioctl-encoded ublk control commands (UBLK_F_CMD_IOCTL_ENCODE) plus the
// per-IO FETCH command. Encodings match drivers/block/ublk_drv.c.
#define UBLK_U_CMD_ADD_DEV 0xC0207504U
#define UBLK_U_CMD_DEL_DEV 0xC0207505U
#define UBLK_U_CMD_START_DEV 0xC0207506U
#define UBLK_U_CMD_STOP_DEV 0xC0207507U
#define UBLK_U_CMD_SET_PARAMS 0xC0207508U
// _IOWR('u', 0x20/0x21, struct ublksrv_io_cmd): 3<<30 | 16<<16 | 'u'<<8 | op
#define UBLK_U_IO_FETCH_REQ 0xC0107520U
#define UBLK_U_IO_COMMIT_AND_FETCH_REQ 0xC0107521U

#ifndef IORING_OP_POLL_ADD
#define IORING_OP_POLL_ADD 6
#endif

#define UBLK_F_CMD_IOCTL_ENCODE (1ULL << 6)
#define UBLK_F_UPDATE_SIZE (1ULL << 10)
#define UBLK_U_CMD_GET_FEATURES 0x80207513U

#ifndef IORING_REGISTER_FILES
#define IORING_REGISTER_FILES 2
#endif
#ifndef IOSQE_FIXED_FILE
#define IOSQE_FIXED_FILE (1U << 0)
#endif
#define UBLK_PARAM_TYPE_BASIC (1U << 0)
#define UBLK_MAX_QUEUE_DEPTH 4096
#define UBLK_IO_DESC_BYTES 24 // sizeof(struct ublksrv_io_desc)

#define QUEUE_DEPTH 128
#define DEV_BYTES (2 * 1024 * 1024)
#define IO_BUF_BYTES 131072

struct ublksrv_ctrl_cmd {
	uint32_t dev_id;
	uint16_t queue_id;
	uint16_t len;
	uint64_t addr;
	uint64_t data[1];
	uint16_t dev_path_len;
	uint16_t pad;
	uint32_t reserved;
};

struct ublksrv_ctrl_dev_info {
	uint16_t nr_hw_queues;
	uint16_t queue_depth;
	uint16_t state;
	uint16_t pad0;
	uint32_t max_io_buf_bytes;
	uint32_t dev_id;
	int32_t ublksrv_pid;
	uint32_t pad1;
	uint64_t flags;
	uint64_t ublksrv_flags;
	uint32_t owner_uid;
	uint32_t owner_gid;
	uint64_t reserved1;
	uint64_t reserved2;
};

struct ublk_param_basic {
	uint32_t attrs;
	uint8_t logical_bs_shift;
	uint8_t physical_bs_shift;
	uint8_t io_opt_shift;
	uint8_t io_min_shift;
	uint32_t max_sectors;
	uint32_t chunk_sectors;
	uint64_t dev_sectors;
	uint64_t virt_boundary_mask;
};

struct ublk_params {
	uint32_t len;
	uint32_t types;
	struct ublk_param_basic basic;
	uint8_t tail[256];
};

struct ublksrv_io_cmd {
	uint16_t q_id;
	uint16_t tag;
	int32_t result;
	uint64_t addr;
};

struct sqe128 {
	uint8_t opcode;
	uint8_t flags;
	uint16_t ioprio;
	int32_t fd;
	uint64_t off;
	uint64_t addr;
	uint32_t len;
	uint32_t op_flags;
	uint64_t user_data;
	uint16_t buf_index;
	uint16_t personality;
	int32_t splice_fd_in;
	uint8_t cmd[80];
};

struct ring {
	int fd;
	uint32_t *sq_tail, *sq_mask, *sq_array;
	uint32_t *cq_head, *cq_tail, *cq_mask;
	struct io_uring_cqe *cqes;
	struct sqe128 *sqes;
	uint32_t sq_local_tail;
};

static volatile sig_atomic_t g_iter;
static volatile sig_atomic_t g_phase; // 0 add 1 params 2 start 3 stop 4 del

static const char *phase_name(int p)
{
	switch (p) {
	case 0: return "ADD_DEV";
	case 1: return "SET_PARAMS";
	case 2: return "START_DEV";
	case 3: return "STOP_DEV";
	case 4: return "DEL_DEV";
	}
	return "?";
}

// Status file written by the watchdog itself: once the kernel is wedged,
// a thread can be stuck in D state and process exit never completes, so
// the caller must not depend on our exit status propagating.
static const char *g_status_path;

static void on_alarm(int sig)
{
	(void)sig;
	char buf[176];
	int n = snprintf(buf, sizeof(buf),
			 "\nWEDGED: iteration %d stuck in %s past the watchdog interval; "
			 "no CQE for that command\n",
			 (int)g_iter, phase_name((int)g_phase));
	if (n > 0)
		(void)!write(STDERR_FILENO, buf, (size_t)n);
	if (g_status_path) {
		int fd = open(g_status_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			(void)!write(fd, "3\n", 2);
			close(fd);
		}
	}
	_exit(3);
}

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static int setup_ring(struct ring *r, unsigned entries)
{
	struct io_uring_params p;
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SQE128;
	memset(r, 0, sizeof(*r));

	r->fd = syscall(__NR_io_uring_setup, entries, &p);
	if (r->fd < 0)
		return -1;

	size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
	size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	size_t sqe_sz = p.sq_entries * sizeof(struct sqe128);

	void *sq = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
	void *cq = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
	void *sqe = mmap(NULL, sqe_sz, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
	if (sq == MAP_FAILED || cq == MAP_FAILED || sqe == MAP_FAILED)
		return -1;

	r->sq_tail = (uint32_t *)((char *)sq + p.sq_off.tail);
	r->sq_mask = (uint32_t *)((char *)sq + p.sq_off.ring_mask);
	r->sq_array = (uint32_t *)((char *)sq + p.sq_off.array);
	r->cq_head = (uint32_t *)((char *)cq + p.cq_off.head);
	r->cq_tail = (uint32_t *)((char *)cq + p.cq_off.tail);
	r->cq_mask = (uint32_t *)((char *)cq + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)((char *)cq + p.cq_off.cqes);
	r->sqes = (struct sqe128 *)sqe;
	return 0;
}

static struct sqe128 *get_sqe(struct ring *r)
{
	uint32_t mask = __atomic_load_n(r->sq_mask, __ATOMIC_ACQUIRE);
	uint32_t idx = r->sq_local_tail & mask;
	struct sqe128 *sqe = &r->sqes[idx];
	memset(sqe, 0, sizeof(*sqe));
	r->sq_array[idx] = idx;
	r->sq_local_tail++;
	return sqe;
}

static int ring_enter(struct ring *r, unsigned to_submit, unsigned min_complete)
{
	__atomic_store_n(r->sq_tail, r->sq_local_tail, __ATOMIC_RELEASE);
	int ret = syscall(__NR_io_uring_enter, r->fd, to_submit, min_complete,
			  IORING_ENTER_GETEVENTS, NULL, 0);
	if (ret < 0)
		return -errno;
	return ret;
}

static int reap_one(struct ring *r, int32_t *res)
{
	uint32_t head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
	uint32_t tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
	if (head == tail)
		return -EAGAIN;
	struct io_uring_cqe *c = &r->cqes[head & __atomic_load_n(r->cq_mask, __ATOMIC_ACQUIRE)];
	*res = c->res;
	__atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
	return 0;
}

static int ctrl_cmd(struct ring *r, int ctrl_fd, uint32_t op, struct ublksrv_ctrl_cmd *cmd)
{
	struct sqe128 *sqe = get_sqe(r);
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = ctrl_fd;
	sqe->off = op;
	memcpy(sqe->cmd, cmd, sizeof(*cmd));

	int ret = ring_enter(r, 1, 1);
	if (ret < 0)
		return ret;
	int32_t res;
	while (reap_one(r, &res) < 0) {
		ret = ring_enter(r, 0, 1);
		if (ret < 0)
			return ret;
	}
	return res;
}

// ublksrv_io_desc, as mmap'd from the char device.
struct ublksrv_io_desc {
	uint32_t op_flags;
	uint32_t nr_sectors;
	uint64_t start_sector;
	uint64_t addr;
};

#define CANCEL_USER_DATA UINT64_MAX

// Per-iteration worker: owns the data queue for one device. Submits
// FETCH_REQ, then blocks in io_uring_enter so the kernel runs the fetch
// task work on this thread (which is what lets START_DEV proceed), and
// services the IOs that arrive.
//
// Servicing is mandatory, not decoration: START_DEV calls add_disk(),
// whose partition scan reads the new disk, and those reads must be
// completed by this loop before START_DEV can return. A worker that
// only waits deadlocks the kernel's add_disk in read_part_sector.
struct worker_arg {
	int cfd;
	int cancel_fd; // eventfd; a write breaks the worker out of enter
	atomic_int ready;
	atomic_int fail;
	atomic_int fetch_res; // first negative FETCH_REQ result, if any
};

static int reap(struct ring *r, int32_t *res, uint64_t *ud)
{
	uint32_t head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
	uint32_t tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
	if (head == tail)
		return -EAGAIN;
	struct io_uring_cqe *c = &r->cqes[head & __atomic_load_n(r->cq_mask, __ATOMIC_ACQUIRE)];
	*res = c->res;
	*ud = c->user_data;
	__atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
	return 0;
}

static void *worker_fn(void *p)
{
	struct worker_arg *wa = p;
	struct ring data;
	uint8_t *bufs = NULL;
	void *descs = MAP_FAILED;
	size_t desc_bytes = (size_t)QUEUE_DEPTH * UBLK_IO_DESC_BYTES;

	if (setup_ring(&data, QUEUE_DEPTH * 4) < 0)
		goto fail;

	bufs = malloc((size_t)QUEUE_DEPTH * IO_BUF_BYTES);
	if (!bufs)
		goto fail;
	memset(bufs, 0, (size_t)QUEUE_DEPTH * IO_BUF_BYTES);

	// Queue 0's descriptors live at qid * UBLK_MAX_QUEUE_DEPTH * 24.
	descs = mmap(NULL, desc_bytes, PROT_READ, MAP_SHARED | MAP_POPULATE,
		     wa->cfd, 0);
	if (descs == MAP_FAILED)
		goto fail;

	// NOTE: the real server registers the char fd as a fixed file and
	// submits with IOSQE_FIXED_FILE. Tried here and reverted: the
	// registration keeps a reference to the char device that outlives
	// close(cfd) (it is dropped by the ring's async exit work), so
	// ublk_ch_release never runs and DEL_DEV deadlocks on iteration 0
	// -- on every kernel, including unaffected ones.

	for (int tag = 0; tag < QUEUE_DEPTH; tag++) {
		struct sqe128 *sqe = get_sqe(&data);
		sqe->opcode = IORING_OP_URING_CMD;
		sqe->fd = wa->cfd;
		sqe->off = UBLK_U_IO_FETCH_REQ;
		sqe->user_data = (uint64_t)tag;
		struct ublksrv_io_cmd ioc;
		memset(&ioc, 0, sizeof(ioc));
		ioc.q_id = 0;
		ioc.tag = (uint16_t)tag;
		ioc.addr = (uintptr_t)(bufs + (size_t)tag * IO_BUF_BYTES);
		memcpy(sqe->cmd, &ioc, sizeof(ioc));
	}
	// Cancellation poll, so teardown can break a blocked enter without
	// relying on the kernel aborting the fetches first.
	{
		struct sqe128 *sqe = get_sqe(&data);
		sqe->opcode = IORING_OP_POLL_ADD;
		sqe->fd = wa->cancel_fd;
		sqe->op_flags = 0x001; // POLLIN
		sqe->user_data = CANCEL_USER_DATA;
	}

	// Submit fetches + poll and run their task work (GETEVENTS), then
	// signal ready so the main thread can issue START_DEV.
	if (ring_enter(&data, QUEUE_DEPTH + 1, 0) < 0)
		goto fail;
	{
		int32_t res;
		uint64_t ud;
		while (reap(&data, &res, &ud) == 0) {
			// A fetch erroring out immediately (bad ABI, device
			// gone) would otherwise look like a START_DEV wedge,
			// since the kernel holds START_DEV until fetches are
			// ready. Surface it instead.
			if (res < 0 && ud != CANCEL_USER_DATA) {
				atomic_store(&wa->fetch_res, res);
				goto fail;
			}
		}
	}
	atomic_store(&wa->ready, 1);

	for (;;) {
		unsigned to_submit = 0;
		int stop = 0;
		int ret = ring_enter(&data, 0, 1);
		if (ret == -EINTR)
			continue;
		if (ret < 0)
			break;

		int32_t res;
		uint64_t ud;
		while (reap(&data, &res, &ud) == 0) {
			if (ud == CANCEL_USER_DATA) {
				stop = 1;
				continue;
			}
			if (res < 0) {
				// Fetch aborted: device is stopping.
				stop = 1;
				continue;
			}
			// An IO arrived for this tag. A memory backend has
			// nothing to do but report the full length as done
			// (reads see the zeroed buffer).
			uint16_t tag = (uint16_t)ud;
			const struct ublksrv_io_desc *d =
				(const struct ublksrv_io_desc *)((const char *)descs +
								 (size_t)tag * UBLK_IO_DESC_BYTES);
			int32_t bytes = (int32_t)(d->nr_sectors << 9);

			struct sqe128 *sqe = get_sqe(&data);
			sqe->opcode = IORING_OP_URING_CMD;
			sqe->fd = wa->cfd;
			sqe->off = UBLK_U_IO_COMMIT_AND_FETCH_REQ;
			sqe->user_data = (uint64_t)tag;
			struct ublksrv_io_cmd ioc;
			memset(&ioc, 0, sizeof(ioc));
			ioc.q_id = 0;
			ioc.tag = tag;
			ioc.result = bytes;
			ioc.addr = (uintptr_t)(bufs + (size_t)tag * IO_BUF_BYTES);
			memcpy(sqe->cmd, &ioc, sizeof(ioc));
			to_submit++;
		}
		if (to_submit && ring_enter(&data, to_submit, 0) < 0)
			break;
		if (stop)
			break;
	}

	close(data.fd);
	if (descs != MAP_FAILED)
		munmap(descs, desc_bytes);
	free(bufs);
	return NULL;

fail:
	atomic_store(&wa->fail, 1);
	atomic_store(&wa->ready, 1);
	if (data.fd > 0)
		close(data.fd);
	if (descs != MAP_FAILED)
		munmap(descs, desc_bytes);
	free(bufs);
	return NULL;
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? strtol(argv[1], NULL, 10) : 5000;
	unsigned watchdog_s = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 30;
	g_status_path = argc > 3 ? argv[3] : NULL;

	signal(SIGALRM, on_alarm);

	int ctrl_fd = open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
	if (ctrl_fd < 0)
		die("open /dev/ublk-control");

	struct ring ctrl;
	if (setup_ring(&ctrl, 8) < 0)
		die("setup control io_uring");

	// Match the real server's device flags: it queries GET_FEATURES and
	// advertises UBLK_F_UPDATE_SIZE whenever the kernel has it, so
	// without this the two are not even creating the same kind of
	// device.
	uint64_t features = 0;
	{
		struct ublksrv_ctrl_cmd gf;
		memset(&gf, 0, sizeof(gf));
		gf.dev_id = UINT32_MAX;
		gf.queue_id = UINT16_MAX;
		gf.addr = (uintptr_t)&features;
		gf.len = sizeof(features);
		if (ctrl_cmd(&ctrl, ctrl_fd, UBLK_U_CMD_GET_FEATURES, &gf) < 0)
			features = 0;
	}
	uint64_t dev_flags = UBLK_F_CMD_IOCTL_ENCODE;
	if (features & UBLK_F_UPDATE_SIZE)
		dev_flags |= UBLK_F_UPDATE_SIZE;
	printf("kernel features 0x%llx, device flags 0x%llx\n",
	       (unsigned long long)features, (unsigned long long)dev_flags);

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (long i = 0; i < iters; i++) {
		g_iter = (sig_atomic_t)i;

		g_phase = 0;
		alarm(watchdog_s);
		struct ublksrv_ctrl_dev_info info;
		memset(&info, 0, sizeof(info));
		info.nr_hw_queues = 1;
		info.queue_depth = QUEUE_DEPTH;
		info.max_io_buf_bytes = IO_BUF_BYTES;
		info.dev_id = UINT32_MAX;
		info.flags = dev_flags;
		struct ublksrv_ctrl_cmd add;
		memset(&add, 0, sizeof(add));
		add.dev_id = UINT32_MAX;
		add.queue_id = UINT16_MAX;
		add.len = sizeof(info);
		add.addr = (uintptr_t)&info;
		int res = ctrl_cmd(&ctrl, ctrl_fd, UBLK_U_CMD_ADD_DEV, &add);
		if (res < 0) {
			fprintf(stderr, "iter %ld ADD_DEV: %s\n", i, strerror(-res));
			return 1;
		}
		uint32_t dev_id = info.dev_id;

		char cpath[64];
		snprintf(cpath, sizeof(cpath), "/dev/ublkc%u", dev_id);
		int cfd = -1;
		for (int t = 0; t < 2000 && cfd < 0; t++) {
			cfd = open(cpath, O_RDWR | O_CLOEXEC);
			if (cfd < 0)
				usleep(500);
		}
		if (cfd < 0)
			die("open ublkc");

		g_phase = 1;
		alarm(watchdog_s);
		struct ublk_params params;
		memset(&params, 0, sizeof(params));
		params.len = sizeof(params);
		params.types = UBLK_PARAM_TYPE_BASIC;
		params.basic.logical_bs_shift = 9;
		params.basic.physical_bs_shift = 9;
		params.basic.io_opt_shift = 9;
		params.basic.io_min_shift = 9;
		params.basic.max_sectors = IO_BUF_BYTES >> 9;
		params.basic.dev_sectors = DEV_BYTES / 512;
		struct ublksrv_ctrl_cmd sp;
		memset(&sp, 0, sizeof(sp));
		sp.dev_id = dev_id;
		sp.queue_id = UINT16_MAX;
		sp.addr = (uintptr_t)&params;
		sp.len = sizeof(params);
		res = ctrl_cmd(&ctrl, ctrl_fd, UBLK_U_CMD_SET_PARAMS, &sp);
		if (res < 0) {
			fprintf(stderr, "iter %ld SET_PARAMS: %s\n", i, strerror(-res));
			return 1;
		}

		// Start the worker; wait until it has submitted its fetches.
		int cancel_fd = eventfd(0, EFD_CLOEXEC);
		if (cancel_fd < 0)
			die("eventfd");
		struct worker_arg wa;
		wa.cfd = cfd;
		wa.cancel_fd = cancel_fd;
		atomic_init(&wa.ready, 0);
		atomic_init(&wa.fail, 0);
		atomic_init(&wa.fetch_res, 0);
		pthread_t th;
		if (pthread_create(&th, NULL, worker_fn, &wa) != 0)
			die("pthread_create");
		while (!atomic_load(&wa.ready))
			usleep(50);
		if (atomic_load(&wa.fail)) {
			int fr = atomic_load(&wa.fetch_res);
			if (fr < 0)
				fprintf(stderr, "iter %ld FETCH_REQ: %s\n", i, strerror(-fr));
			else
				fprintf(stderr, "iter %ld worker setup failed\n", i);
			pthread_join(th, NULL);
			close(cancel_fd);
			return 1;
		}

		g_phase = 2;
		alarm(watchdog_s);
		struct ublksrv_ctrl_cmd start;
		memset(&start, 0, sizeof(start));
		start.dev_id = dev_id;
		start.queue_id = UINT16_MAX;
		start.data[0] = (uint64_t)getpid();
		res = ctrl_cmd(&ctrl, ctrl_fd, UBLK_U_CMD_START_DEV, &start);
		if (res < 0) {
			fprintf(stderr, "iter %ld START_DEV: %s\n", i, strerror(-res));
			return 1;
		}

		// One small buffered write through the block device, matching
		// the real server's churn workload. It also gives udev's probe
		// of the freshly added disk a moment to finish: tearing the
		// server down while udev still has /dev/ublkbN open deadlocks
		// del_gendisk (it waits for the opener; the opener waits for IO
		// no server is left to serve) on every kernel, which would
		// masquerade as the bug under test.
		{
			char bpath[64];
			snprintf(bpath, sizeof(bpath), "/dev/ublkb%u", dev_id);
			int bfd = open(bpath, O_WRONLY | O_CLOEXEC);
			if (bfd >= 0) {
				static uint8_t blk[4096];
				(void)!pwrite(bfd, blk, sizeof(blk), 0);
				// Force the write through the server now: a
				// buffered write would still be in page cache at
				// teardown, so its writeback would fail instead of
				// completing, and the iteration would not have
				// exercised a real IO at all.
				(void)!fsync(bfd);
				close(bfd);
			}
		}

		// Teardown in ublk-go's order: cancel the worker and join it
		// (so its ring, which holds a reference to the char file, is
		// closed first), then close the char fd -- that is what lets
		// ublk_ch_release run -- then STOP_DEV + DEL_DEV.
		uint64_t one = 1;
		if (write(cancel_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
			die("write cancel eventfd");
		pthread_join(th, NULL);
		close(cancel_fd);
		close(cfd);

		g_phase = 3;
		alarm(watchdog_s);
		struct ublksrv_ctrl_cmd stop;
		memset(&stop, 0, sizeof(stop));
		stop.dev_id = dev_id;
		stop.queue_id = UINT16_MAX;
		res = ctrl_cmd(&ctrl, ctrl_fd, UBLK_U_CMD_STOP_DEV, &stop);
		if (res < 0 && res != -EBUSY) {
			fprintf(stderr, "iter %ld STOP_DEV: %s\n", i, strerror(-res));
			return 1;
		}

		g_phase = 4;
		alarm(watchdog_s);
		struct ublksrv_ctrl_cmd del;
		memset(&del, 0, sizeof(del));
		del.dev_id = dev_id;
		del.queue_id = UINT16_MAX;
		res = ctrl_cmd(&ctrl, ctrl_fd, UBLK_U_CMD_DEL_DEV, &del);
		if (res < 0) {
			fprintf(stderr, "iter %ld DEL_DEV: %s\n", i, strerror(-res));
			return 1;
		}
		alarm(0);

		if ((i + 1) % 200 == 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
			printf("iter %ld/%ld ok (%.0f/s)\n", i + 1, iters, (i + 1) / el);
			fflush(stdout);
		}
	}

	printf("PASS: %ld create/start/stop iterations completed without a wedge\n", iters);
	return 0;
}
