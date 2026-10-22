/*
 * minivmm.c — a miniature type-2 hypervisor (VMM) built directly on /dev/kvm.
 *
 * Cloud Computing — HW1                                   *** STARTER CODE ***
 *
 * Search for "TODO" — there are 4 of them (TODO 1..4). Everything else is
 * complete and worth reading: it shows every KVM ioctl a VMM needs.
 *
 * The Linux kernel module KVM does the hard part (VMX/SVM, VMCS, EPT). This
 * program is the *userspace* half of the hypervisor, the same role QEMU or
 * Firecracker plays: it creates a VM, gives it memory and vCPUs, loads raw
 * machine code into it, and emulates every device access the guest makes.
 *
 * Guest ABI (what the guest sees when its first instruction runs):
 *   - 32-bit protected mode, flat segments (base 0, limit 4 GiB), paging off,
 *     interrupts disabled (RFLAGS.IF = 0).
 *   - The guest image is loaded at guest-physical GUEST_LOAD_ADDR (0x1000),
 *     and every vCPU starts executing there.
 *   - EDI = vCPU id, ESI = guest RAM size in bytes, EDX = number of vCPUs,
 *     ESP = top of a private VCPU_STACK_SIZE stack for this vCPU.
 *   - OUT 0x3F8 (COM1 data) : the byte is printed on our stdout.
 *   - IN  0x3FD (COM1 LSR)  : returns 0x20 ("transmitter ready").
 *   - OUT 0x80  (heartbeat) : ignored, only counted.
 *   - HLT                   : this vCPU is finished. The VM ends when all
 *                             vCPUs have halted (or on SIGTERM / SIGINT).
 *
 * Usage: minivmm [--mem MiB] [--vcpus N] [--stats FILE] guest.bin
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GUEST_LOAD_ADDR   0x1000UL
#define VCPU_STACK_SIZE   0x10000UL      /* 64 KiB per vCPU, at the top of RAM */
#define MAX_VCPUS         64
#define MIN_MEM_MB        2
#define MAX_MEM_MB        3072           /* must stay below 4 GiB: 32-bit guest */

#define PORT_COM1_DATA    0x3f8
#define PORT_COM1_LSR     0x3fd
#define PORT_HEARTBEAT    0x80

#define NR_EXIT_REASONS   64             /* KVM_EXIT_* are small integers */

struct vm;

struct vcpu {
    int id;
    int fd;                              /* from KVM_CREATE_VCPU */
    struct kvm_run *run;                 /* shared with the kernel (mmap of fd) */
    pthread_t thread;
    struct vm *vm;
    atomic_int running;
    /* exits[r] = how many times KVM_RUN returned to us with exit_reason r */
    atomic_ullong exits[NR_EXIT_REASONS];
};

struct vm {
    int kvm_fd;                          /* /dev/kvm */
    int vm_fd;                           /* from KVM_CREATE_VM */
    uint8_t *mem;                        /* guest RAM, as seen by us */
    size_t mem_size;
    int nr_vcpus;
    struct vcpu vcpus[MAX_VCPUS];
    const char *guest_path;
    const char *stats_path;              /* NULL: no live stats file */
    struct timespec start;
};

static atomic_int stop_requested;        /* SIGTERM / SIGINT received */
static atomic_int vm_failed;             /* a vCPU hit an unrecoverable exit */
static pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;

static void die(const char *what)
{
    perror(what);
    exit(1);
}

/* ------------------------------------------------------------------------- */
/* VM exit bookkeeping                                                        */
/* ------------------------------------------------------------------------- */

static const char *exit_reason_name(unsigned r)
{
    switch (r) {
    case KVM_EXIT_UNKNOWN:          return "UNKNOWN";
    case KVM_EXIT_EXCEPTION:        return "EXCEPTION";
    case KVM_EXIT_IO:               return "IO";
    case KVM_EXIT_HYPERCALL:        return "HYPERCALL";
    case KVM_EXIT_DEBUG:            return "DEBUG";
    case KVM_EXIT_HLT:              return "HLT";
    case KVM_EXIT_MMIO:             return "MMIO";
    case KVM_EXIT_IRQ_WINDOW_OPEN:  return "IRQ_WINDOW_OPEN";
    case KVM_EXIT_SHUTDOWN:         return "SHUTDOWN";
    case KVM_EXIT_FAIL_ENTRY:       return "FAIL_ENTRY";
    case KVM_EXIT_INTR:             return "INTR";
    case KVM_EXIT_SET_TPR:          return "SET_TPR";
    case KVM_EXIT_TPR_ACCESS:       return "TPR_ACCESS";
    case KVM_EXIT_NMI:              return "NMI";
    case KVM_EXIT_INTERNAL_ERROR:   return "INTERNAL_ERROR";
    case KVM_EXIT_SYSTEM_EVENT:     return "SYSTEM_EVENT";
    default:                        return "OTHER";
    }
}

// static void count_exit(struct vcpu *v, unsigned reason)
// {
//     /*
//      * TODO 3 (Part 2): count this VM exit in v->exits[reason].
//      *   - guard against reason >= NR_EXIT_REASONS (count it as KVM_EXIT_UNKNOWN)
//      *   - write_stats() and the control panel read these counters from another
//      *     thread while vCPUs are running, so increment them atomically, e.g.
//      *     atomic_fetch_add_explicit(..., 1, memory_order_relaxed)
//      */
//     (void)v;
//     (void)reason;
// }


static void count_exit(struct vcpu *v, unsigned reason)
{
    if (reason >= NR_EXIT_REASONS)
        reason = KVM_EXIT_UNKNOWN;

    atomic_fetch_add_explicit(&v->exits[reason], 1,
                              memory_order_relaxed);
}






static uint64_t exits_of(struct vcpu *v, unsigned reason)
{
    return atomic_load_explicit(&v->exits[reason], memory_order_relaxed);
}

static double uptime(struct vm *vm)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - vm->start.tv_sec) + (now.tv_nsec - vm->start.tv_nsec) / 1e9;
}

/*
 * Prints a table of VM exits on stderr: one row per exit reason that happened
 * at least once with its count, and a TOTAL row. Called once, when the VM
 * ends. One column per vCPU is optional. Example (2 vCPUs, with columns):
 *
 *   [minivmm] ===== VM exits seen by userspace (0.01 s) =====
 *   reason                  vcpu0        vcpu1        total
 *   IO                         54            0           54
 *   HLT                         1            1            2
 *   TOTAL                      55            1           56
 */



// static void print_exit_table(struct vm *vm)
// {
//     /*
//      * TODO 4 (Part 2): print the table described above.
//      *   Hints: iterate r over 0..NR_EXIT_REASONS-1, read counters with
//      *   exits_of(), name rows with exit_reason_name(), skip all-zero rows.
//      */
//     fprintf(stderr, "\n[minivmm] VM ran for %.2f s (exit table not implemented yet)\n", uptime(vm));
// }



static void print_exit_table(struct vm *vm)
{
    uint64_t per_vcpu[MAX_VCPUS] = {0};
    uint64_t grand_total = 0;

    fprintf(stderr,
            "\n[minivmm] ===== VM exits seen by userspace (%.2f s) =====\n",
            uptime(vm));

    fprintf(stderr, "%-20s", "reason");

    for (int i = 0; i < vm->nr_vcpus; i++) {
        char label[16];
        snprintf(label, sizeof(label), "vcpu%d", i);
        fprintf(stderr, " %12s", label);
    }

    fprintf(stderr, " %12s\n", "total");

    for (unsigned r = 0; r < NR_EXIT_REASONS; r++) {
        uint64_t counts[MAX_VCPUS] = {0};
        uint64_t row_total = 0;

        for (int i = 0; i < vm->nr_vcpus; i++) {
            counts[i] = exits_of(&vm->vcpus[i], r);
            row_total += counts[i];
        }

        if (row_total == 0)
            continue;

        fprintf(stderr, "%-20s", exit_reason_name(r));

        for (int i = 0; i < vm->nr_vcpus; i++) {
            fprintf(stderr, " %12llu",
                    (unsigned long long)counts[i]);
            per_vcpu[i] += counts[i];
        }

        fprintf(stderr, " %12llu\n",
                (unsigned long long)row_total);
        grand_total += row_total;
    }

    fprintf(stderr, "%-20s", "TOTAL");

    for (int i = 0; i < vm->nr_vcpus; i++)
        fprintf(stderr, " %12llu",
                (unsigned long long)per_vcpu[i]);

    fprintf(stderr, " %12llu\n",
            (unsigned long long)grand_total);
}




/*
 * Writes the live statistics as JSON to vm->stats_path (atomically, via
 * rename) so the control panel can read them without talking to us.
 */
static void write_stats(struct vm *vm, const char *state)
{
    if (!vm->stats_path)
        return;

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", vm->stats_path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    uint64_t total = 0;
    fprintf(f, "{\n  \"pid\": %d,\n  \"state\": \"%s\",\n", getpid(), state);
    fprintf(f, "  \"mem_mb\": %zu,\n  \"vcpus\": %d,\n", vm->mem_size >> 20, vm->nr_vcpus);
    fprintf(f, "  \"uptime_s\": %.3f,\n  \"exits\": {", uptime(vm));
    int first = 1;
    for (unsigned r = 0; r < NR_EXIT_REASONS; r++) {
        uint64_t row = 0;
        for (int i = 0; i < vm->nr_vcpus; i++)
            row += exits_of(&vm->vcpus[i], r);
        if (row == 0)
            continue;
        fprintf(f, "%s\"%s\": %llu", first ? "" : ", ", exit_reason_name(r), (unsigned long long)row);
        first = 0;
        total += row;
    }
    fprintf(f, "},\n  \"total_exits\": %llu,\n  \"per_vcpu\": [", (unsigned long long)total);
    for (int i = 0; i < vm->nr_vcpus; i++) {
        struct vcpu *v = &vm->vcpus[i];
        uint64_t n = 0;
        for (unsigned r = 0; r < NR_EXIT_REASONS; r++)
            n += exits_of(v, r);
        fprintf(f, "%s{\"id\": %d, \"running\": %s, \"exits\": %llu}", i ? ", " : "",
                i, atomic_load(&v->running) ? "true" : "false", (unsigned long long)n);
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    rename(tmp, vm->stats_path);
}

/* ------------------------------------------------------------------------- */
/* Device emulation: this is the "emulate" half of trap-and-emulate           */
/* ------------------------------------------------------------------------- */

// static void handle_io(struct vcpu *v)
// {
//     struct kvm_run *run = v->run;
//     /*
//      * TODO 2 (Part 1): emulate the port I/O that made the guest exit.
//      *
//      * The kernel describes the access in run->io:
//      *   run->io.direction  KVM_EXIT_IO_OUT (guest wrote) or KVM_EXIT_IO_IN (guest reads)
//      *   run->io.port       I/O port number
//      *   run->io.size       bytes per access (1, 2 or 4)
//      *   run->io.count      number of accesses (> 1 only for rep ins/outs)
//      *   run->io.data_offset  the data itself is at (uint8_t *)run + data_offset
//      *
//      * Implement the guest ABI documented at the top of this file:
//      *   OUT PORT_COM1_DATA : write the byte(s) to stdout and fflush() it.
//      *                        Several vCPUs may print at once: hold serial_lock.
//      *   OUT PORT_HEARTBEAT : do nothing (it is only counted).
//      *   other OUT          : print a warning on stderr and ignore it.
//      *   IN  PORT_COM1_LSR  : fill the data with 0x20 (transmitter ready).
//      *   other IN           : fill the data with 0xff.
//      *
//      * When you return, the next KVM_RUN resumes the guest *after* the IN/OUT
//      * instruction (for IN, with the value you placed in the data buffer).
//      */
//     (void)run;
// }

static void handle_io(struct vcpu *v)
{
    struct kvm_run *run = v->run;
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    size_t length = (size_t)run->io.size * run->io.count;

    if (run->io.direction == KVM_EXIT_IO_OUT) {
        switch (run->io.port) {
        case PORT_COM1_DATA:
            pthread_mutex_lock(&serial_lock);
            fwrite(data, 1, length, stdout);
            fflush(stdout);
            pthread_mutex_unlock(&serial_lock);
            break;

        case PORT_HEARTBEAT:
            /* The run loop already counted this I/O exit. */
            break;

        default:
            fprintf(stderr,
                    "[minivmm] vcpu%d: OUT to unknown port 0x%x ignored\n",
                    v->id, (unsigned)run->io.port);
            break;
        }
    } else if (run->io.direction == KVM_EXIT_IO_IN) {
        memset(data,
               run->io.port == PORT_COM1_LSR ? 0x20 : 0xff,
               length);
    }
}









static void handle_mmio(struct vcpu *v)
{
    struct kvm_run *run = v->run;
    fprintf(stderr, "[minivmm] vcpu%d: MMIO %s at 0x%llx (outside RAM) ignored\n", v->id,
            run->mmio.is_write ? "write" : "read", (unsigned long long)run->mmio.phys_addr);
    if (!run->mmio.is_write)
        memset(run->mmio.data, 0xff, sizeof(run->mmio.data));
}

/* ------------------------------------------------------------------------- */
/* The vCPU run loop                                                          */
/* ------------------------------------------------------------------------- */

// static void vcpu_loop(struct vcpu *v)
// {
//     struct kvm_run *run = v->run;

//     for (;;) {
//         if (atomic_load(&stop_requested))
//             return;

//         /*
//          * TODO 1a (Part 1): enter the guest with the KVM_RUN ioctl on the vCPU fd
//          * (VM entry: root -> non-root mode). It only returns when the guest
//          * does something the kernel cannot handle alone (a VM exit that must
//          * be emulated in userspace) or when a signal interrupts it.
//          *   - On failure with errno EINTR or EAGAIN, a signal (e.g. our own
//          *     kick from kick_vcpus()) interrupted the guest: count it as
//          *     KVM_EXIT_INTR and `continue` (the loop re-checks stop_requested).
//          *   - On any other failure: perror(), set vm_failed, return.
//          * TODO 3 (second half): after a successful KVM_RUN, call
//          *   count_exit(v, run->exit_reason).
//          */

//         switch (run->exit_reason) {
//         /*
//          * TODO 1b (Part 1): handle the two exits our guests rely on:
//          *   KVM_EXIT_HLT : the guest executed HLT, i.e. this vCPU has finished.
//          *                  End this vCPU's loop (return).
//          *   KVM_EXIT_IO  : the guest executed IN/OUT. Call handle_io(v) and
//          *                  keep looping so the guest is resumed.
//          */

//         case KVM_EXIT_MMIO:
//             handle_mmio(v);
//             break;

//         case KVM_EXIT_SHUTDOWN:
//             fprintf(stderr, "[minivmm] vcpu%d: SHUTDOWN (triple fault)\n", v->id);
//             atomic_store(&vm_failed, 1);
//             return;

//         case KVM_EXIT_FAIL_ENTRY:
//             fprintf(stderr, "[minivmm] vcpu%d: FAIL_ENTRY, hardware reason 0x%llx\n", v->id,
//                     (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
//             atomic_store(&vm_failed, 1);
//             return;

//         case KVM_EXIT_INTERNAL_ERROR:
//             fprintf(stderr, "[minivmm] vcpu%d: INTERNAL_ERROR, suberror %u\n", v->id,
//                     run->internal.suberror);
//             atomic_store(&vm_failed, 1);
//             return;

//         default:
//             fprintf(stderr, "[minivmm] vcpu%d: unhandled exit reason %u (%s)\n", v->id,
//                     run->exit_reason, exit_reason_name(run->exit_reason));
//             atomic_store(&vm_failed, 1);
//             return;
//         }
//     }
// }


static void vcpu_loop(struct vcpu *v)
{
    struct kvm_run *run = v->run;

    for (;;) {
        if (atomic_load(&stop_requested))
            return;

        if (ioctl(v->fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                count_exit(v, KVM_EXIT_INTR);
                continue;
            }

            perror("KVM_RUN");
            atomic_store(&vm_failed, 1);
            return;
        }

        count_exit(v, run->exit_reason);

        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            return;

        case KVM_EXIT_IO:
            handle_io(v);
            break;

        case KVM_EXIT_INTR:
            break;

        case KVM_EXIT_MMIO:
            handle_mmio(v);
            break;

        case KVM_EXIT_SHUTDOWN:
            fprintf(stderr,
                    "[minivmm] vcpu%d: SHUTDOWN (triple fault)\n",
                    v->id);
            atomic_store(&vm_failed, 1);
            return;

        case KVM_EXIT_FAIL_ENTRY:
            fprintf(stderr,
                    "[minivmm] vcpu%d: FAIL_ENTRY, hardware reason 0x%llx\n",
                    v->id,
                    (unsigned long long)
                    run->fail_entry.hardware_entry_failure_reason);
            atomic_store(&vm_failed, 1);
            return;

        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr,
                    "[minivmm] vcpu%d: INTERNAL_ERROR, suberror %u\n",
                    v->id, run->internal.suberror);
            atomic_store(&vm_failed, 1);
            return;

        default:
            fprintf(stderr,
                    "[minivmm] vcpu%d: unhandled exit reason %u (%s)\n",
                    v->id, run->exit_reason,
                    exit_reason_name(run->exit_reason));
            atomic_store(&vm_failed, 1);
            return;
        }
    }
}























static void *vcpu_thread(void *arg)
{
    struct vcpu *v = arg;
    char name[16];

    /* Shows up in /proc/<pid>/task/<tid>/comm and in htop (press H). */
    snprintf(name, sizeof(name), "vcpu%d", v->id);
    pthread_setname_np(pthread_self(), name);

    vcpu_loop(v);
    atomic_store(&v->running, 0);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* VM construction                                                            */
/* ------------------------------------------------------------------------- */

static void kvm_open(struct vm *vm)
{
    vm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (vm->kvm_fd < 0)
        die("open /dev/kvm");

    int api = ioctl(vm->kvm_fd, KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) {
        fprintf(stderr, "KVM API version %d, expected %d\n", api, KVM_API_VERSION);
        exit(1);
    }

    int max = ioctl(vm->kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS);
    if (max <= 0)
        max = ioctl(vm->kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_NR_VCPUS);
    if (max > 0 && vm->nr_vcpus > max) {
        fprintf(stderr, "KVM supports at most %d vCPUs per VM\n", max);
        exit(1);
    }
    if (ioctl(vm->kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT) <= 0) {
        fprintf(stderr, "kernel lacks KVM_CAP_IMMEDIATE_EXIT (need Linux >= 4.11)\n");
        exit(1);
    }

    /* A VM is just a file descriptor. */
    vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0)
        die("KVM_CREATE_VM");

    /* Intel needs a 3-page region for real-mode emulation; harmless on AMD. */
    if (ioctl(vm->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000UL) < 0)
        die("KVM_SET_TSS_ADDR");
}

static void setup_memory(struct vm *vm)
{
    /*
     * Guest RAM is ordinary anonymous memory of *this process*. Nothing is
     * physically allocated yet: the host kernel hands out pages on first
     * touch. Transparent huge pages are disabled so that RSS grows page by
     * page, which makes the resize experiment easier to read.
     */
    vm->mem = mmap(NULL, vm->mem_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vm->mem == MAP_FAILED)
        die("mmap guest RAM");
    madvise(vm->mem, vm->mem_size, MADV_NOHUGEPAGE);

    /* Tell KVM: guest-physical [0, mem_size) is backed by this host buffer. */
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .flags = 0,
        .guest_phys_addr = 0,
        .memory_size = vm->mem_size,
        .userspace_addr = (uint64_t)(uintptr_t)vm->mem,
    };
    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        die("KVM_SET_USER_MEMORY_REGION");
}

static void load_guest(struct vm *vm)
{
    int fd = open(vm->guest_path, O_RDONLY);
    if (fd < 0)
        die(vm->guest_path);

    struct stat st;
    if (fstat(fd, &st) < 0)
        die("fstat guest");
    size_t limit = vm->mem_size - GUEST_LOAD_ADDR - vm->nr_vcpus * VCPU_STACK_SIZE;
    if (st.st_size == 0 || (size_t)st.st_size > limit) {
        fprintf(stderr, "guest image is %lld bytes; it must be 1..%zu bytes for this RAM size\n",
                (long long)st.st_size, limit);
        exit(1);
    }

    /* "Loading" a guest is just copying bytes into its RAM. */
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, vm->mem + GUEST_LOAD_ADDR + done, st.st_size - done);
        if (n <= 0)
            die("read guest");
        done += n;
    }
    close(fd);
}

static void setup_vcpu(struct vm *vm, int id)
{
    struct vcpu *v = &vm->vcpus[id];
    v->id = id;
    v->vm = vm;

    v->fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, id);
    if (v->fd < 0)
        die("KVM_CREATE_VCPU");

    /* kvm_run: the page through which the kernel tells us why the guest exited. */
    int run_size = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (run_size <= 0)
        die("KVM_GET_VCPU_MMAP_SIZE");
    v->run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, 0);
    if (v->run == MAP_FAILED)
        die("mmap kvm_run");

    /*
     * Special registers: switch the vCPU straight into flat 32-bit protected
     * mode. No GDT is needed because KVM loads the hidden part of the segment
     * registers directly from these structs.
     */
    struct kvm_sregs sregs;
    if (ioctl(v->fd, KVM_GET_SREGS, &sregs) < 0)
        die("KVM_GET_SREGS");
    struct kvm_segment code = {
        .base = 0, .limit = 0xffffffff, .selector = 1 << 3,
        .type = 11 /* execute/read, accessed */, .present = 1, .dpl = 0,
        .db = 1 /* 32-bit */, .s = 1, .l = 0, .g = 1 /* 4 KiB granularity */,
    };
    struct kvm_segment data = code;
    data.type = 3;                       /* read/write, accessed */
    data.selector = 2 << 3;
    sregs.cs = code;
    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;
    sregs.cr0 |= 1;                      /* CR0.PE: protected mode */
    sregs.cr0 &= ~(1UL << 31);           /* CR0.PG: paging stays off */
    if (ioctl(v->fd, KVM_SET_SREGS, &sregs) < 0)
        die("KVM_SET_SREGS");

    /* General purpose registers: entry point and the guest ABI arguments. */
    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip = GUEST_LOAD_ADDR;
    regs.rflags = 0x2;                   /* bit 1 is reserved and must be 1; IF = 0 */
    regs.rdi = id;
    regs.rsi = vm->mem_size;
    regs.rdx = vm->nr_vcpus;
    regs.rsp = vm->mem_size - (uint64_t)id * VCPU_STACK_SIZE;
    if (ioctl(v->fd, KVM_SET_REGS, &regs) < 0)
        die("KVM_SET_REGS");
}

/* ------------------------------------------------------------------------- */
/* Signals: SIGTERM/SIGINT ask us to stop, SIGUSR1 kicks a vCPU out of KVM_RUN */
/* ------------------------------------------------------------------------- */

static void on_stop_signal(int sig)
{
    (void)sig;
    atomic_store(&stop_requested, 1);
}

static void on_kick_signal(int sig)
{
    (void)sig;                           /* only purpose: make KVM_RUN return EINTR */
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                     /* no SA_RESTART: we want EINTR */
    sa.sa_handler = on_stop_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_kick_signal;
    sigaction(SIGUSR1, &sa, NULL);
}

static void kick_vcpus(struct vm *vm)
{
    for (int i = 0; i < vm->nr_vcpus; i++) {
        struct vcpu *v = &vm->vcpus[i];
        /* If the signal lands just before KVM_RUN, immediate_exit still stops it. */
        v->run->immediate_exit = 1;
        pthread_kill(v->thread, SIGUSR1);
    }
}

static int any_vcpu_running(struct vm *vm)
{
    for (int i = 0; i < vm->nr_vcpus; i++)
        if (atomic_load(&vm->vcpus[i].running))
            return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--mem MiB] [--vcpus N] [--stats FILE] guest.bin\n"
            "  --mem   guest RAM in MiB (%d..%d, default 64)\n"
            "  --vcpus number of virtual CPUs (1..%d, default 1)\n"
            "  --stats write live VM-exit statistics (JSON) to FILE every second\n",
            prog, MIN_MEM_MB, MAX_MEM_MB, MAX_VCPUS);
    exit(2);
}

int main(int argc, char **argv)
{
    static struct vm vm;                 /* static: vcpus[] is large-ish */
    long mem_mb = 64;
    vm.nr_vcpus = 1;

    static const struct option opts[] = {
        {"mem", required_argument, NULL, 'm'},
        {"vcpus", required_argument, NULL, 'c'},
        {"stats", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "m:c:s:h", opts, NULL)) != -1) {
        switch (opt) {
        case 'm': mem_mb = strtol(optarg, NULL, 10); break;
        case 'c': vm.nr_vcpus = (int)strtol(optarg, NULL, 10); break;
        case 's': vm.stats_path = optarg; break;
        default: usage(argv[0]);
        }
    }
    if (optind != argc - 1)
        usage(argv[0]);
    if (mem_mb < MIN_MEM_MB || mem_mb > MAX_MEM_MB || vm.nr_vcpus < 1 || vm.nr_vcpus > MAX_VCPUS)
        usage(argv[0]);
    vm.guest_path = argv[optind];
    vm.mem_size = (size_t)mem_mb << 20;
    clock_gettime(CLOCK_MONOTONIC, &vm.start);

    fprintf(stderr, "[minivmm] pid=%d guest=%s mem=%ldMiB vcpus=%d\n",
            getpid(), vm.guest_path, mem_mb, vm.nr_vcpus);

    kvm_open(&vm);
    setup_memory(&vm);
    load_guest(&vm);
    for (int i = 0; i < vm.nr_vcpus; i++)
        setup_vcpu(&vm, i);

    install_signals();

    /*
     * vCPU threads must not receive SIGTERM/SIGINT (only the main thread
     * handles them), so block them while creating the threads, which inherit
     * the mask, then unblock them again in the main thread.
     */
    sigset_t stopset, old;
    sigemptyset(&stopset);
    sigaddset(&stopset, SIGTERM);
    sigaddset(&stopset, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stopset, &old);
    for (int i = 0; i < vm.nr_vcpus; i++) {
        atomic_store(&vm.vcpus[i].running, 1);
        if (pthread_create(&vm.vcpus[i].thread, NULL, vcpu_thread, &vm.vcpus[i]) != 0)
            die("pthread_create");
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    /* The main thread is the "management" thread: it only publishes stats. */
    write_stats(&vm, "running");
    int ticks = 0;
    while (!atomic_load(&stop_requested) && !atomic_load(&vm_failed) && any_vcpu_running(&vm)) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        if (++ticks % 10 == 0)
            write_stats(&vm, "running");
    }

    int stopped_by_signal = atomic_load(&stop_requested);
    atomic_store(&stop_requested, 1);
    kick_vcpus(&vm);
    for (int i = 0; i < vm.nr_vcpus; i++)
        pthread_join(vm.vcpus[i].thread, NULL);

    const char *state = atomic_load(&vm_failed) ? "failed" : stopped_by_signal ? "stopped" : "halted";
    print_exit_table(&vm);
    write_stats(&vm, state);
    fprintf(stderr, "[minivmm] VM %s\n", state);
    return atomic_load(&vm_failed) ? 1 : 0;
}
