#!/usr/bin/env python3
"""vmctl — a tiny cloud control panel on top of minivmm (Cloud Computing HW1).

*** STARTER CODE *** — search for "TODO". Functions that are already complete
(state helpers, admission control, run/stop/logs/delete wrappers, argument
parsing) show the conventions the rest of the file should follow.

The control plane (this file) never touches KVM. It only:
  * keeps a small "database" of VM definitions (a JSON file),
  * starts / stops the data plane (one minivmm process per VM),
  * observes those processes through ordinary OS interfaces (/proc),
  * enforces resource policies (admission control) before starting a VM.

Usage:
  vmctl.py create NAME --guest FILE [--mem MiB] [--vcpus N]
  vmctl.py run    NAME [--follow]
  vmctl.py list
  vmctl.py stop   NAME
  vmctl.py stats  (NAME | --pid PID) [--interval SEC] [--count N]
  vmctl.py resize NAME [--mem MiB] [--vcpus N] [--restart]
  vmctl.py logs   NAME [--lines N]
  vmctl.py delete NAME
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE_BIN = ROOT / "core" / "minivmm"
STATE_DIR = Path(os.environ.get("VMCTL_HOME", ROOT / ".vmctl"))
STATE_FILE = STATE_DIR / "vms.json"

CLK_TCK = os.sysconf("SC_CLK_TCK")      # kernel ticks per second for utime/stime
HOST_CPUS = os.cpu_count() or 1

MIN_MEM_MB, MAX_MEM_MB = 2, 3072          # must match core/minivmm.c
MAX_VCPUS = 64
HOST_MEM_LIMIT = 0.80                     # running VMs may reserve <= 80% of host RAM
NAME_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9_-]{0,31}$")


class VmctlError(Exception):
    pass


# --------------------------------------------------------------------------- #
# State ("database") helpers                                                    #
# --------------------------------------------------------------------------- #

def load_state():
    if not STATE_FILE.exists():
        return {}
    with open(STATE_FILE) as f:
        return json.load(f)


def save_state(state):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    tmp = STATE_FILE.with_suffix(".tmp")
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, STATE_FILE)           # atomic: never a half-written database


def get_vm(state, name):
    if name not in state:
        raise VmctlError(f"no VM named '{name}' (see: vmctl.py list)")
    return state[name]


def log_path(name):
    return STATE_DIR / f"{name}.log"


def stats_path(name):
    return STATE_DIR / f"{name}.stats.json"


def read_vm_stats(name):
    """The JSON that minivmm publishes every second (exit counters, state)."""
    try:
        with open(stats_path(name)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


# --------------------------------------------------------------------------- #
# /proc readers: the VM is just a process, so ordinary OS tools can see it     #
# --------------------------------------------------------------------------- #

def is_running(vm):
    """True if vm['pid'] is alive *and* is really a minivmm (PIDs get reused)."""
    pid = vm.get("pid")
    if not pid:
        return False
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            argv = f.read().split(b"\0")
        with open(f"/proc/{pid}/stat") as f:
            state = f.read().rsplit(")", 1)[1].split()[0]
    except OSError:
        return False
    return state != "Z" and bool(argv) and argv[0].endswith(b"minivmm")


def read_cpu_ticks(pid, tid=None):
    """utime + stime (in clock ticks) of a process, or of one of its threads.

    TODO (Part 4): parse /proc/<pid>/stat (or /proc/<pid>/task/<tid>/stat).
    See `man 5 proc`: utime is field 14, stime is field 15. Careful: field 2
    (comm) is in parentheses and may contain spaces, so split *after* the
    last ')'. Convert ticks to seconds with CLK_TCK.
    """
    raise NotImplementedError("TODO: read_cpu_ticks")


def read_mem_status(pid):
    """VmRSS / VmSize / VmHWM (KiB) and Threads from /proc/<pid>/status."""
    out = {}
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            key, _, value = line.partition(":")
            if key in ("VmRSS", "VmSize", "VmHWM"):
                out[key] = int(value.split()[0])          # value is "1234 kB"
            elif key == "Threads":
                out[key] = int(value)
    return out


def read_threads(pid):
    """{tid: comm} for every thread of the process (vcpu0, vcpu1, ...)."""
    threads = {}
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            with open(f"/proc/{pid}/task/{tid}/comm") as f:
                threads[int(tid)] = f.read().strip()
        except OSError:
            pass                                          # thread just exited
    return threads


def host_mem_mb():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemTotal:"):
                return int(line.split()[1]) // 1024
    raise VmctlError("cannot read MemTotal from /proc/meminfo")


# --------------------------------------------------------------------------- #
# Policy / admission control                                                   #
# --------------------------------------------------------------------------- #

def validate_resources(mem_mb, vcpus, overcommit=False):
    if not MIN_MEM_MB <= mem_mb <= MAX_MEM_MB:
        raise VmctlError(f"--mem must be in [{MIN_MEM_MB}, {MAX_MEM_MB}] MiB")
    if not 1 <= vcpus <= MAX_VCPUS:
        raise VmctlError(f"--vcpus must be in [1, {MAX_VCPUS}]")
    if vcpus > HOST_CPUS and not overcommit:
        raise VmctlError(f"{vcpus} vCPUs > {HOST_CPUS} host CPUs (use --overcommit to allow)")


def admit(state, name, mem_mb):
    """Refuse to start a VM if running VMs would reserve too much host RAM."""
    reserved = sum(vm["mem_mb"] for n, vm in state.items() if n != name and is_running(vm))
    limit = int(host_mem_mb() * HOST_MEM_LIMIT)
    if reserved + mem_mb > limit:
        raise VmctlError(f"admission refused: {reserved} MiB already reserved + {mem_mb} MiB "
                         f"> {limit} MiB ({int(HOST_MEM_LIMIT * 100)}% of host RAM)")


# --------------------------------------------------------------------------- #
# Commands                                                                     #
# --------------------------------------------------------------------------- #

# def cmd_create(args):
#     """vmctl.py create NAME --guest FILE [--mem MiB] [--vcpus N] [--overcommit]

#     TODO (Part 3): validate the name (NAME_RE), refuse duplicates, check that the
#     guest file exists (store its absolute path), call validate_resources(),
#     then add an entry to the state and save it. Suggested entry fields:
#     name, guest, mem_mb, vcpus, overcommit, pid (None), created_at, started_at.
#     """
#     raise NotImplementedError("TODO: cmd_create")



def cmd_create(args):
    state = load_state()

    if not NAME_RE.fullmatch(args.name):
        raise VmctlError(
            "name must start with a letter and contain only "
            "letters, digits, '_' or '-' (maximum 32 characters)"
        )

    if args.name in state:
        raise VmctlError(f"VM '{args.name}' already exists")

    guest = Path(args.guest).expanduser().resolve()
    if not guest.is_file():
        raise VmctlError(f"guest file does not exist: {guest}")

    validate_resources(args.mem, args.vcpus, args.overcommit)

    state[args.name] = {
        "name": args.name,
        "guest": str(guest),
        "mem_mb": args.mem,
        "vcpus": args.vcpus,
        "overcommit": args.overcommit,
        "pid": None,
        "created_at": time.time(),
        "started_at": None,
    }

    save_state(state)

    print(
        f"created VM '{args.name}': "
        f"{args.vcpus} vCPU, {args.mem} MiB, guest={guest.name}"
    )


# def start_vm(state, name):
#     """Start the minivmm process for VM `name`; return the subprocess.Popen.

#     TODO (Part 3):
#       * refuse if it is already running; check CORE_BIN exists
#       * re-validate resources and call admit() (admission control)
#       * build the command line:
#             core/minivmm --mem M --vcpus N --stats <stats_path(name)> <guest>
#       * start it with subprocess.Popen, stdout+stderr redirected to
#         log_path(name), stdin=DEVNULL, start_new_session=True (so the VM
#         survives the panel exiting and does not get the panel's Ctrl+C)
#       * store the pid (and start time) in the state and save it
#     """
#     raise NotImplementedError("TODO: start_vm")



def start_vm(state, name):
    vm = get_vm(state, name)

    if is_running(vm):
        raise VmctlError(f"VM '{name}' is already running")

    if not CORE_BIN.is_file():
        raise VmctlError(f"minivmm executable not found: {CORE_BIN}; run make")

    if not os.access(CORE_BIN, os.X_OK):
        raise VmctlError(f"minivmm is not executable: {CORE_BIN}")

    if not Path(vm["guest"]).is_file():
        raise VmctlError(f"guest file does not exist: {vm['guest']}")

    validate_resources(
        vm["mem_mb"], vm["vcpus"], vm.get("overcommit", False)
    )
    admit(state, name, vm["mem_mb"])

    STATE_DIR.mkdir(parents=True, exist_ok=True)

    command = [
        str(CORE_BIN),
        "--mem", str(vm["mem_mb"]),
        "--vcpus", str(vm["vcpus"]),
        "--stats", str(stats_path(name).resolve()),
        vm["guest"],
    ]

    try:
        stats_path(name).unlink(missing_ok=True)

        with open(log_path(name), "w") as log:
            proc = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
    except OSError as exc:
        raise VmctlError(f"cannot start VM '{name}': {exc}") from exc

    vm["pid"] = proc.pid
    vm["started_at"] = time.time()
    save_state(state)

    return proc




def cmd_run(args):
    state = load_state()
    proc = start_vm(state, args.name)
    vm = state[args.name]
    print(f"started VM '{args.name}' as pid {proc.pid} "
          f"({vm['vcpus']} vCPU, {vm['mem_mb']} MiB) — it is a normal host process")

    time.sleep(0.3)
    if proc.poll() is not None and proc.returncode != 0:
        print(f"VM exited immediately with code {proc.returncode}; log:", file=sys.stderr)
        print(log_path(args.name).read_text(), file=sys.stderr)
        sys.exit(1)

    if args.follow:                       # behave like `tail -f` until the VM ends
        try:
            with open(log_path(args.name)) as f:
                while True:
                    chunk = f.read()
                    if chunk:
                        print(chunk, end="", flush=True)
                    elif proc.poll() is not None:
                        break
                    else:
                        time.sleep(0.1)
        except KeyboardInterrupt:
            print(f"\n(detached; VM '{args.name}' is still running)")


def vm_status(name, vm):
    if is_running(vm):
        return "running"
    if vm.get("pid") is None:
        return "created"
    stats = read_vm_stats(name)
    return stats["state"] if stats else "exited"


# def cmd_list(args):
#     """vmctl.py list — one row per VM: NAME STATE PID VCPUS MEM GUEST.

#     TODO (Part 3): use vm_status() for the STATE column; show the PID only for
#     running VMs.
#     """
#     raise NotImplementedError("TODO: cmd_list")


def cmd_list(args):
    state = load_state()

    print(
        f"{'NAME':<32} {'STATE':<10} {'PID':>8} "
        f"{'VCPUS':>6} {'MEM(MiB)':>10}  GUEST"
    )

    for name, vm in sorted(state.items()):
        status = vm_status(name, vm)
        pid = str(vm["pid"]) if status == "running" else "-"

        print(
            f"{name:<32} {status:<10} {pid:>8} "
            f"{vm['vcpus']:>6} {vm['mem_mb']:>10}  "
            f"{Path(vm['guest']).name}"
        )


# def stop_vm(state, name, timeout=5.0):
#     """Stop a running VM gracefully; return True if it was running.

#     TODO (Part 3): send SIGTERM (minivmm then kicks its vCPUs out of KVM_RUN,
#     prints the exit table and exits). Poll is_running() for up to `timeout`
#     seconds; if it is still alive, send SIGKILL.
#     """
#     raise NotImplementedError("TODO: stop_vm")



def stop_vm(state, name, timeout=5.0):
    vm = get_vm(state, name)

    if not is_running(vm):
        print(f"VM '{name}' is not running")
        return False

    pid = vm["pid"]

    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        print(f"VM '{name}' has already exited")
        return False
    except PermissionError as exc:
        raise VmctlError(f"cannot stop VM '{name}': {exc}") from exc

    deadline = time.monotonic() + timeout

    while is_running(vm) and time.monotonic() < deadline:
        time.sleep(0.05)

    forced = False

    if is_running(vm):
        try:
            os.kill(pid, signal.SIGKILL)
            forced = True
        except ProcessLookupError:
            pass
        except PermissionError as exc:
            raise VmctlError(f"cannot kill VM '{name}': {exc}") from exc

        deadline = time.monotonic() + 1.0

        while is_running(vm) and time.monotonic() < deadline:
            time.sleep(0.05)

        if is_running(vm):
            raise VmctlError(f"VM '{name}' has not exited after SIGKILL")

    suffix = " using SIGKILL" if forced else ""
    print(f"stopped VM '{name}' (pid {pid}){suffix}")

    return True


def cmd_stop(args):
    state = load_state()
    if stop_vm(state, args.name):
        lines = log_path(args.name).read_text().splitlines()
        tail = [l for l in lines if l.strip()][-12:]
        print("\n".join(tail))


def cmd_stats(args):
    """vmctl.py stats (NAME | --pid PID) [-i SEC] [-n COUNT]

    With --pid (Part 6), monitor that process instead of a VM from the state
    (e.g. a qemu-system-x86_64 you started by hand): same columns, the process
    is "alive" while /proc/<pid> exists, and there is no stats file.

    TODO (Part 4): every `interval` seconds print one line with:
      * the time
      * CPU%  = delta(utime+stime) / CLK_TCK / delta(wall seconds) * 100
                (100% = one host CPU fully busy; 2 busy vCPUs => ~200%)
      * RSS and VSZ in MiB, and the number of threads (read_mem_status())
    Stop after `count` samples (0 = until Ctrl+C) or when the VM exits.

    Optional: per-thread CPU% (vcpu0=.., vcpu1=..) using read_threads(), and
    VM exits per second from the stats file (read_vm_stats(): total_exits,
    uptime_s).
    """
    raise NotImplementedError("TODO: cmd_stats")


def cmd_resize(args):
    """vmctl.py resize NAME [--mem MiB] [--vcpus N] [--overcommit] [--restart]

    TODO (Part 5): compute the new values (keep the old ones if an option is not
    given) and validate them. minivmm can only be resized *cold*:
      * VM not running        -> just update and save the definition
      * running, no --restart -> refuse with a clear error
      * running, --restart    -> stop_vm(), update + save, start_vm()
                                 (start_vm() already calls admit())
    Print old -> new values.
    """
    raise NotImplementedError("TODO: cmd_resize")


def cmd_logs(args):
    state = load_state()
    get_vm(state, args.name)
    path = log_path(args.name)
    if not path.exists():
        raise VmctlError(f"VM '{args.name}' has never run")
    lines = path.read_text().splitlines()
    print("\n".join(lines[-args.lines:]))


def cmd_delete(args):
    state = load_state()
    vm = get_vm(state, args.name)
    if is_running(vm):
        raise VmctlError(f"VM '{args.name}' is running; stop it first")
    del state[args.name]
    save_state(state)
    for p in (log_path(args.name), stats_path(args.name)):
        p.unlink(missing_ok=True)
    print(f"deleted VM '{args.name}'")


# --------------------------------------------------------------------------- #

def build_parser():
    p = argparse.ArgumentParser(prog="vmctl.py", description="control panel for minivmm")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("create", help="define a new VM")
    s.add_argument("name")
    s.add_argument("--guest", required=True, help="raw guest image (.bin)")
    s.add_argument("--mem", type=int, default=64, help="RAM in MiB (default 64)")
    s.add_argument("--vcpus", type=int, default=1, help="number of vCPUs (default 1)")
    s.add_argument("--overcommit", action="store_true", help="allow more vCPUs than host CPUs")
    s.set_defaults(func=cmd_create)

    s = sub.add_parser("run", help="start a VM in the background")
    s.add_argument("name")
    s.add_argument("-f", "--follow", action="store_true", help="stream the guest's serial output")
    s.set_defaults(func=cmd_run)

    s = sub.add_parser("list", help="list VMs")
    s.set_defaults(func=cmd_list)

    s = sub.add_parser("stop", help="stop a running VM")
    s.add_argument("name")
    s.set_defaults(func=cmd_stop)

    s = sub.add_parser("stats", help="monitor CPU / memory / exits of a running VM")
    s.add_argument("name", nargs="?")
    s.add_argument("-p", "--pid", type=int, help="monitor any process instead (e.g. qemu)")
    s.add_argument("-i", "--interval", type=float, default=1.0, help="seconds between samples")
    s.add_argument("-n", "--count", type=int, default=0, help="number of samples (0 = until Ctrl+C)")
    s.set_defaults(func=cmd_stats)

    s = sub.add_parser("resize", help="change the memory / vCPUs of a VM")
    s.add_argument("name")
    s.add_argument("--mem", type=int)
    s.add_argument("--vcpus", type=int)
    s.add_argument("--overcommit", action="store_true")
    s.add_argument("--restart", action="store_true", help="stop, resize and start a running VM")
    s.set_defaults(func=cmd_resize)

    s = sub.add_parser("logs", help="show the VM's serial output and exit table")
    s.add_argument("name")
    s.add_argument("-n", "--lines", type=int, default=40)
    s.set_defaults(func=cmd_logs)

    s = sub.add_parser("delete", help="forget a stopped VM")
    s.add_argument("name")
    s.set_defaults(func=cmd_delete)
    return p


def main():
    args = build_parser().parse_args()
    try:
        args.func(args)
    except VmctlError as e:
        print(f"vmctl: error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
