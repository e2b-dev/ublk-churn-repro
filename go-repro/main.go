// Go port of ublk_churn_repro.c, syscall for syscall.
//
// It exists to split one question in two. Every program that stays clean
// on the affected kernels is C; the only one that hangs is ublk-go,
// which is Go. This program is the C reproducer's exact sequence written
// in Go, with no dependency on ublk-go: it talks to io_uring through raw
// syscalls and mmap.
//
//   - If this hangs, the trigger is the Go runtime's interaction with
//     io_uring, not anything in the ublk-go library.
//   - If it stays clean, the trigger is in the ublk-go library, and the
//     next step is bisecting its features back in.
//
// Like the C version it needs two threads (a URING_CMD completion is
// task work bound to the submitting task, so one thread deadlocks by
// construction) and its worker must actually service IO (START_DEV's
// add_disk reads the new disk).
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync/atomic"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	opUringCmd = 46
	opPollAdd  = 6

	setupSQE128    = 1 << 10
	enterGetevents = 1 << 0

	offSQRing = 0x00000000
	offCQRing = 0x08000000
	offSQEs   = 0x10000000

	cmdGetFeatures = 0x80207513
	cmdAddDev      = 0xC0207504
	cmdDelDev      = 0xC0207505
	cmdStartDev    = 0xC0207506
	cmdStopDev     = 0xC0207507
	cmdSetParams   = 0xC0207508
	cmdGetQueueAff = 0x80207501

	ioFetchReq          = 0xC0107520
	ioCommitAndFetchReq = 0xC0107521

	fCmdIoctlEncode = 1 << 6
	fUpdateSize     = 1 << 10

	paramTypeBasic  = 1 << 0
	maxQueueDepth   = 4096
	ioDescBytes     = 24
	queueDepth      = 128
	devBytes        = 2 * 1024 * 1024
	ioBufBytes      = 131072
	cancelUserData  = ^uint64(0)
	ioDescQueueSize = queueDepth * ioDescBytes
)

type ctrlCmd struct {
	DevID      uint32
	QueueID    uint16
	Len        uint16
	Addr       uint64
	Data       [1]uint64
	DevPathLen uint16
	Pad        uint16
	Reserved   uint32
}

type devInfo struct {
	NrHwQueues    uint16
	QueueDepth    uint16
	State         uint16
	Pad0          uint16
	MaxIOBufBytes uint32
	DevID         uint32
	UblksrvPid    int32
	Pad1          uint32
	Flags         uint64
	UblksrvFlags  uint64
	OwnerUID      uint32
	OwnerGID      uint32
	Reserved1     uint64
	Reserved2     uint64
}

type paramBasic struct {
	Attrs            uint32
	LogicalBsShift   uint8
	PhysicalBsShift  uint8
	IOOptShift       uint8
	IOMinShift       uint8
	MaxSectors       uint32
	ChunkSectors     uint32
	DevSectors       uint64
	VirtBoundaryMask uint64
}

type params struct {
	Len   uint32
	Types uint32
	Basic paramBasic
	Tail  [256]byte
}

type ioCmd struct {
	QID    uint16
	Tag    uint16
	Result int32
	Addr   uint64
}

type sqe128 struct {
	Opcode      uint8
	Flags       uint8
	Ioprio      uint16
	Fd          int32
	Off         uint64
	Addr        uint64
	Len         uint32
	OpFlags     uint32
	UserData    uint64
	BufIndex    uint16
	Personality uint16
	SpliceFdIn  int32
	Cmd         [80]byte
}

type cqe struct {
	UserData uint64
	Res      int32
	Flags    uint32
}

type ioDesc struct {
	OpFlags   uint32
	NrSectors uint32
	StartSect uint64
	Addr      uint64
}

type ringParams struct {
	SqEntries    uint32
	CqEntries    uint32
	Flags        uint32
	SqThreadCPU  uint32
	SqThreadIdle uint32
	Features     uint32
	WqFd         uint32
	Resv         [3]uint32
	SqOff        sqRingOffsets
	CqOff        cqRingOffsets
}

type sqRingOffsets struct {
	Head, Tail, RingMask, RingEntries, Flags, Dropped, Array, Resv1 uint32
	UserAddr                                                        uint64
}

type cqRingOffsets struct {
	Head, Tail, RingMask, RingEntries, Overflow, Cqes, Flags, Resv1 uint32
	UserAddr                                                        uint64
}

type ring struct {
	fd int
	// ublk-go's Ring.setupCancel: an eventfd plus a private epoll
	// holding the io_uring fd itself. Unused in the hot path there too
	// (its worker waits in io_uring_enter and cancels through an in-ring
	// POLL_ADD), but present on every ring. REPRO_NO_EPOLL=1 omits it.
	cancelEvFD, epFD        int
	sqTail, sqMask, sqArray *uint32
	cqHead, cqTail, cqMask  *uint32
	cqes                    unsafe.Pointer
	sqes                    unsafe.Pointer
	localTail               uint32
	sqBase, cqBase, sqeBase []byte
}

func setupRing(entries uint32) (*ring, error) {
	var p ringParams
	p.Flags = setupSQE128
	fd, _, errno := unix.Syscall(unix.SYS_IO_URING_SETUP, uintptr(entries), uintptr(unsafe.Pointer(&p)), 0)
	if errno != 0 {
		return nil, fmt.Errorf("io_uring_setup: %w", errno)
	}
	r := &ring{fd: int(fd), cancelEvFD: -1, epFD: -1}
	if os.Getenv("REPRO_NO_EPOLL") == "" {
		efd, err := unix.Eventfd(0, unix.EFD_CLOEXEC)
		if err != nil {
			return nil, fmt.Errorf("eventfd: %w", err)
		}
		r.cancelEvFD = efd
		epfd, err := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
		if err != nil {
			return nil, fmt.Errorf("epoll_create1: %w", err)
		}
		r.epFD = epfd
		for _, fd := range []int{r.fd, r.cancelEvFD} {
			ev := unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(fd)}
			if err := unix.EpollCtl(r.epFD, unix.EPOLL_CTL_ADD, fd, &ev); err != nil {
				return nil, fmt.Errorf("epoll_ctl: %w", err)
			}
		}
	}

	sqSz := int(p.SqOff.Array + p.SqEntries*4)
	cqSz := int(p.CqOff.Cqes + p.CqEntries*uint32(unsafe.Sizeof(cqe{})))
	sqeSz := int(p.SqEntries * uint32(unsafe.Sizeof(sqe128{})))

	var err error
	if r.sqBase, err = unix.Mmap(r.fd, offSQRing, sqSz, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED|unix.MAP_POPULATE); err != nil {
		return nil, err
	}
	if r.cqBase, err = unix.Mmap(r.fd, offCQRing, cqSz, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED|unix.MAP_POPULATE); err != nil {
		return nil, err
	}
	if r.sqeBase, err = unix.Mmap(r.fd, offSQEs, sqeSz, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED|unix.MAP_POPULATE); err != nil {
		return nil, err
	}

	at := func(b []byte, off uint32) *uint32 { return (*uint32)(unsafe.Pointer(&b[off])) }
	r.sqTail = at(r.sqBase, p.SqOff.Tail)
	r.sqMask = at(r.sqBase, p.SqOff.RingMask)
	r.sqArray = at(r.sqBase, p.SqOff.Array)
	r.cqHead = at(r.cqBase, p.CqOff.Head)
	r.cqTail = at(r.cqBase, p.CqOff.Tail)
	r.cqMask = at(r.cqBase, p.CqOff.RingMask)
	r.cqes = unsafe.Pointer(&r.cqBase[p.CqOff.Cqes])
	r.sqes = unsafe.Pointer(&r.sqeBase[0])

	// Identity SQ array, populated once, as both the C reproducer and
	// ublk-go do.
	for i := uint32(0); i < p.SqEntries; i++ {
		*(*uint32)(unsafe.Add(unsafe.Pointer(r.sqArray), uintptr(i)*4)) = i
	}
	r.localTail = atomic.LoadUint32(r.sqTail)
	return r, nil
}

func (r *ring) close() {
	if r.epFD >= 0 {
		unix.Close(r.epFD)
	}
	if r.cancelEvFD >= 0 {
		unix.Close(r.cancelEvFD)
	}
	unix.Munmap(r.sqBase)
	unix.Munmap(r.cqBase)
	unix.Munmap(r.sqeBase)
	unix.Close(r.fd)
}

func (r *ring) sqe() *sqe128 {
	idx := r.localTail & atomic.LoadUint32(r.sqMask)
	s := (*sqe128)(unsafe.Add(r.sqes, uintptr(idx)*unsafe.Sizeof(sqe128{})))
	*s = sqe128{}
	r.localTail++
	return s
}

func (r *ring) enter(toSubmit, minComplete uint32) error {
	atomic.StoreUint32(r.sqTail, r.localTail)
	for {
		_, _, errno := unix.Syscall6(unix.SYS_IO_URING_ENTER, uintptr(r.fd),
			uintptr(toSubmit), uintptr(minComplete), enterGetevents, 0, 0)
		if errno == unix.EINTR {
			continue
		}
		if errno != 0 {
			return fmt.Errorf("io_uring_enter: %w", errno)
		}
		return nil
	}
}

func (r *ring) reap() (uint64, int32, bool) {
	head := atomic.LoadUint32(r.cqHead)
	if head == atomic.LoadUint32(r.cqTail) {
		return 0, 0, false
	}
	c := (*cqe)(unsafe.Add(r.cqes, uintptr(head&atomic.LoadUint32(r.cqMask))*unsafe.Sizeof(cqe{})))
	ud, res := c.UserData, c.Res
	atomic.StoreUint32(r.cqHead, head+1)
	return ud, res, true
}

// ctrl issues one control command and waits for it, submit+wait in a
// single enter, exactly as the C reproducer and ublk-go do.
func ctrl(r *ring, ctrlFD int, op uint32, c *ctrlCmd) (int32, error) {
	s := r.sqe()
	s.Opcode = opUringCmd
	s.Fd = int32(ctrlFD)
	s.Off = uint64(op)
	*(*ctrlCmd)(unsafe.Pointer(&s.Cmd[0])) = *c

	for {
		if err := r.enter(1, 1); err != nil {
			return 0, err
		}
		if _, res, ok := r.reap(); ok {
			return res, nil
		}
	}
}

type workerArg struct {
	charFD   int
	cancelFD int
	ready    atomic.Bool
	failed   atomic.Bool
	fetchRes atomic.Int32
	setupErr error
	done     chan struct{}
}

func worker(wa *workerArg) {
	runtime.LockOSThread()
	defer close(wa.done)

	r, err := setupRing(queueDepth + 1)
	if err != nil {
		wa.setupErr = fmt.Errorf("worker ring: %w", err)
		wa.failed.Store(true)
		wa.ready.Store(true)
		return
	}
	defer r.close()

	descs, err := unix.Mmap(wa.charFD, 0, ioDescQueueSize,
		unix.PROT_READ, unix.MAP_SHARED|unix.MAP_POPULATE)
	if err != nil {
		wa.setupErr = fmt.Errorf("mmap io descs: %w", err)
		wa.failed.Store(true)
		wa.ready.Store(true)
		return
	}
	defer unix.Munmap(descs)

	bufs := make([]byte, queueDepth*ioBufBytes)

	fetch := func(tag uint16, op uint32, result int32) {
		s := r.sqe()
		s.Opcode = opUringCmd
		s.Fd = int32(wa.charFD)
		s.Off = uint64(op)
		s.UserData = uint64(tag)
		*(*ioCmd)(unsafe.Pointer(&s.Cmd[0])) = ioCmd{
			QID: 0, Tag: tag, Result: result,
			Addr: uint64(uintptr(unsafe.Pointer(&bufs[int(tag)*ioBufBytes]))),
		}
	}
	for tag := 0; tag < queueDepth; tag++ {
		fetch(uint16(tag), ioFetchReq, 0)
	}
	// Cancellation poll, so teardown can break a blocked enter.
	s := r.sqe()
	s.Opcode = opPollAdd
	s.Fd = int32(wa.cancelFD)
	s.OpFlags = unix.POLLIN
	s.UserData = cancelUserData

	if err := r.enter(queueDepth+1, 0); err != nil {
		wa.setupErr = fmt.Errorf("submit fetches: %w", err)
		wa.failed.Store(true)
		wa.ready.Store(true)
		return
	}
	wa.ready.Store(true)

	for {
		if err := r.enter(0, 1); err != nil {
			return
		}
		for {
			ud, res, ok := r.reap()
			if !ok {
				break
			}
			if ud == cancelUserData {
				return
			}
			if res < 0 {
				wa.fetchRes.Store(res)
				wa.failed.Store(true)
				return
			}
			tag := uint16(ud)
			d := (*ioDesc)(unsafe.Pointer(&descs[int(tag)*ioDescBytes]))
			n := int32(d.NrSectors) * 512
			fetch(tag, ioCommitAndFetchReq, n)
			if err := r.enter(1, 0); err != nil {
				return
			}
		}
	}
}

var (
	iterNow  atomic.Int64
	phaseNow atomic.Value
)

func iteration(i int64, devFlags uint64) error {
	phase := func(s string) { phaseNow.Store(s) }

	phase("open-control")
	ctrlFD, err := unix.Open("/dev/ublk-control", unix.O_RDWR|unix.O_CLOEXEC, 0)
	if err != nil {
		return fmt.Errorf("open /dev/ublk-control: %w", err)
	}
	defer unix.Close(ctrlFD)

	r, err := setupRing(4)
	if err != nil {
		return err
	}
	defer r.close()

	// ublk-go asks the kernel for its features on every device, so on
	// each fresh control ring the first command is GET_FEATURES -- which
	// 7.0 runs inline -- and ADD_DEV is the first one punted to io-wq.
	// REPRO_NO_PER_DEV_FEATURES=1 drops it, which is what the C
	// reproducer and the original port do.
	if os.Getenv("REPRO_NO_PER_DEV_FEATURES") == "" {
		var f uint64
		gf := ctrlCmd{DevID: ^uint32(0), QueueID: ^uint16(0), Len: 8,
			Addr: uint64(uintptr(unsafe.Pointer(&f)))}
		if _, err := ctrl(r, ctrlFD, cmdGetFeatures, &gf); err != nil {
			return err
		}
	}

	phase("ADD_DEV")
	info := devInfo{
		NrHwQueues: 1, QueueDepth: queueDepth, MaxIOBufBytes: ioBufBytes,
		DevID: ^uint32(0), Flags: devFlags,
	}
	add := ctrlCmd{DevID: ^uint32(0), QueueID: ^uint16(0),
		Len: uint16(unsafe.Sizeof(info)), Addr: uint64(uintptr(unsafe.Pointer(&info)))}
	if res, err := ctrl(r, ctrlFD, cmdAddDev, &add); err != nil {
		return err
	} else if res < 0 {
		return fmt.Errorf("ADD_DEV: %w", unix.Errno(-res))
	}
	devID := info.DevID

	phase("open-char")
	var charFD int
	for t := 0; t < 2000; t++ {
		charFD, err = unix.Open(fmt.Sprintf("/dev/ublkc%d", devID), unix.O_RDWR|unix.O_CLOEXEC, 0)
		if err == nil {
			break
		}
		time.Sleep(500 * time.Microsecond)
	}
	if err != nil {
		return fmt.Errorf("open ublkc%d: %w", devID, err)
	}

	phase("SET_PARAMS")
	pp := params{Len: uint32(unsafe.Sizeof(params{})), Types: paramTypeBasic}
	pp.Basic = paramBasic{
		LogicalBsShift: 9, PhysicalBsShift: 9, IOOptShift: 9, IOMinShift: 9,
		MaxSectors: ioBufBytes >> 9, DevSectors: devBytes / 512,
	}
	sp := ctrlCmd{DevID: devID, QueueID: ^uint16(0),
		Len: uint16(unsafe.Sizeof(pp)), Addr: uint64(uintptr(unsafe.Pointer(&pp)))}
	if res, err := ctrl(r, ctrlFD, cmdSetParams, &sp); err != nil {
		return err
	} else if res < 0 {
		return fmt.Errorf("SET_PARAMS: %w", unix.Errno(-res))
	}

	// ublk-go asks for the queue's CPU mask between SET_PARAMS and
	// START_DEV. It matters here for its ordering, not its answer: 7.0
	// runs GET_QUEUE_AFFINITY inline while ADD_DEV, SET_PARAMS and
	// START_DEV are punted to io-wq, so ublk-go interleaves an inline
	// command between punted ones and this program did not.
	// REPRO_NO_QAFF=1 drops it.
	var cpuMask [128]byte
	if os.Getenv("REPRO_NO_QAFF") == "" {
		qa := ctrlCmd{DevID: devID, QueueID: 0,
			Len: uint16(len(cpuMask)), Addr: uint64(uintptr(unsafe.Pointer(&cpuMask[0])))}
		if _, err := ctrl(r, ctrlFD, cmdGetQueueAff, &qa); err != nil {
			return err
		}
	}

	phase("start-worker")
	cancelFD, err := unix.Eventfd(0, unix.EFD_CLOEXEC)
	if err != nil {
		return fmt.Errorf("eventfd: %w", err)
	}
	wa := &workerArg{charFD: charFD, cancelFD: cancelFD, done: make(chan struct{})}
	go worker(wa)
	for !wa.ready.Load() {
		time.Sleep(50 * time.Microsecond)
	}
	if wa.failed.Load() {
		<-wa.done
		unix.Close(cancelFD)
		unix.Close(charFD)
		return fmt.Errorf("worker setup failed: %v (fetch res %d)", wa.setupErr, wa.fetchRes.Load())
	}

	phase("START_DEV")
	st := ctrlCmd{DevID: devID, QueueID: ^uint16(0)}
	st.Data[0] = uint64(os.Getpid())
	if res, err := ctrl(r, ctrlFD, cmdStartDev, &st); err != nil {
		return err
	} else if res < 0 {
		return fmt.Errorf("START_DEV: %w", unix.Errno(-res))
	}

	phase("write")
	if bfd, err := unix.Open(fmt.Sprintf("/dev/ublkb%d", devID), unix.O_WRONLY|unix.O_CLOEXEC, 0); err == nil {
		_, _ = unix.Pwrite(bfd, make([]byte, 4096), 0)
		_ = unix.Fsync(bfd)
		unix.Close(bfd)
	}

	phase("teardown")
	one := uint64(1)
	if _, err := unix.Write(cancelFD, (*[8]byte)(unsafe.Pointer(&one))[:]); err != nil {
		return fmt.Errorf("write cancel eventfd: %w", err)
	}
	<-wa.done
	unix.Close(cancelFD)
	unix.Close(charFD)

	phase("STOP_DEV")
	stop := ctrlCmd{DevID: devID, QueueID: ^uint16(0)}
	if res, err := ctrl(r, ctrlFD, cmdStopDev, &stop); err != nil {
		return err
	} else if res < 0 && res != -int32(unix.EBUSY) {
		return fmt.Errorf("STOP_DEV: %w", unix.Errno(-res))
	}

	phase("DEL_DEV")
	del := ctrlCmd{DevID: devID, QueueID: ^uint16(0)}
	if res, err := ctrl(r, ctrlFD, cmdDelDev, &del); err != nil {
		return err
	} else if res < 0 {
		return fmt.Errorf("DEL_DEV: %w", unix.Errno(-res))
	}
	return nil
}

func main() {
	iters := int64(3000)
	watchdog := 30 * time.Second
	if len(os.Args) > 1 {
		if n, err := strconv.ParseInt(os.Args[1], 10, 64); err == nil {
			iters = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			watchdog = time.Duration(n) * time.Second
		}
	}
	phaseNow.Store("init")

	// Watchdog: turn a hang into a named non-zero exit, like the C
	// version's alarm(), since a wedge can otherwise take the VM with it.
	progress := make(chan struct{}, 1)
	go func() {
		for {
			select {
			case <-progress:
			case <-time.After(watchdog):
				fmt.Printf("WEDGE: iteration %d stuck in %v for %v\n",
					iterNow.Load(), phaseNow.Load(), watchdog)
				os.Exit(3)
			}
		}
	}()

	ctrlFD, err := unix.Open("/dev/ublk-control", unix.O_RDWR|unix.O_CLOEXEC, 0)
	if err != nil {
		fmt.Println("open /dev/ublk-control:", err)
		os.Exit(1)
	}
	r, err := setupRing(4)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	var features uint64
	gf := ctrlCmd{DevID: ^uint32(0), QueueID: ^uint16(0), Len: 8,
		Addr: uint64(uintptr(unsafe.Pointer(&features)))}
	if res, err := ctrl(r, ctrlFD, cmdGetFeatures, &gf); err != nil || res < 0 {
		features = 0
	}
	r.close()
	unix.Close(ctrlFD)

	devFlags := uint64(fCmdIoctlEncode)
	if features&fUpdateSize != 0 {
		devFlags |= fUpdateSize
	}
	fmt.Printf("kernel features 0x%x, device flags 0x%x\n", features, devFlags)

	start := time.Now()
	for i := int64(0); i < iters; i++ {
		iterNow.Store(i)
		select {
		case progress <- struct{}{}:
		default:
		}
		if err := iteration(i, devFlags); err != nil {
			fmt.Printf("iter %d: %v\n", i, err)
			os.Exit(1)
		}
		if (i+1)%200 == 0 {
			fmt.Printf("iter %d/%d ok (%.0f/s)\n", i+1, iters,
				float64(i+1)/time.Since(start).Seconds())
		}
	}
	fmt.Printf("PASS: %d create/start/stop iterations completed without a wedge\n", iters)
}
