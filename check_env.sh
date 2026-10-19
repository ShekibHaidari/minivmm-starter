#!/usr/bin/env bash
# check_env.sh — verify that this machine can run minivmm (Cloud Computing HW1).
# Usage: ./check_env.sh
# Exit code: 0 if every required check passed, 1 otherwise.

RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; RST=$'\e[0m'
fails=0

pass() { printf '  %s[PASS]%s %s\n' "$GRN" "$RST" "$1"; }
fail() { printf '  %s[FAIL]%s %s\n' "$RED" "$RST" "$1"; [ -n "$2" ] && printf '         -> %s\n' "$2"; fails=$((fails + 1)); }
warn() { printf '  %s[WARN]%s %s\n' "$YEL" "$RST" "$1"; [ -n "$2" ] && printf '         -> %s\n' "$2"; }

echo "== Host =="
if [ "$(uname -s)" = "Linux" ]; then
    pass "Linux kernel $(uname -r)"
else
    fail "not Linux ($(uname -s))" "KVM only exists on Linux. Use a Linux machine/VM (see the assignment, section 'Environment')."
fi

arch=$(uname -m)
if [ "$arch" = "x86_64" ]; then
    pass "architecture x86_64"
else
    fail "architecture is $arch" "the guests are x86 machine code; you need an x86_64 host (Apple Silicon / ARM will not work)."
fi

echo "== CPU virtualization extensions =="
if grep -qw vmx /proc/cpuinfo 2>/dev/null; then
    pass "Intel VT-x (vmx flag present)"; vendor=intel
elif grep -qw svm /proc/cpuinfo 2>/dev/null; then
    pass "AMD-V (svm flag present)"; vendor=amd
else
    fail "no vmx/svm flag in /proc/cpuinfo" "enable VT-x/AMD-V in BIOS, or enable nested virtualization if this is itself a VM."
    vendor=none
fi

echo "== KVM =="
if lsmod 2>/dev/null | grep -q '^kvm'; then
    pass "kvm modules loaded: $(lsmod | awk '/^kvm/{printf "%s ", $1}')"
else
    warn "no kvm module in lsmod" "try: sudo modprobe kvm_intel  (or kvm_amd); it may also be built into the kernel."
fi

if [ -e /dev/kvm ]; then
    pass "/dev/kvm exists"
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        pass "/dev/kvm is readable and writable by $(id -un)"
    else
        fail "no read/write permission on /dev/kvm" "sudo usermod -aG kvm $(id -un)   then log out and back in."
    fi
else
    fail "/dev/kvm does not exist" "load the module (sudo modprobe kvm_$vendor) and check virtualization is enabled."
fi

if [ "$vendor" != none ]; then
    nested_file=/sys/module/kvm_$vendor/parameters/nested
    if [ -r "$nested_file" ]; then
        echo "  [INFO] nested virtualization on this host: $(cat "$nested_file")"
    fi
fi
if grep -qw hypervisor /proc/cpuinfo 2>/dev/null; then
    echo "  [INFO] this machine is itself a virtual machine (cpuinfo has the 'hypervisor' flag) -> you are already nested!"
fi

echo "== Tools =="
need() {  # need <cmd> <hint> [version-flag]
    if command -v "$1" >/dev/null 2>&1; then
        pass "$1: $("$1" "${3:---version}" 2>&1 | head -n1)"
    else
        fail "$1 not found" "$2"
    fi
}
need gcc     "sudo apt install build-essential"
need make    "sudo apt install build-essential"
need nasm    "sudo apt install nasm" -v
need python3 "sudo apt install python3"
need qemu-system-x86_64 "sudo apt install qemu-system-x86 qemu-utils  (needed for Part 6)"
need qemu-img "sudo apt install qemu-utils  (needed for Part 6)"
if command -v python3 >/dev/null 2>&1; then
    if python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)'; then
        pass "python3 >= 3.8"
    else
        fail "python3 is older than 3.8"
    fi
fi
if [ -e /usr/include/linux/kvm.h ]; then
    pass "/usr/include/linux/kvm.h present"
else
    fail "linux/kvm.h header missing" "sudo apt install linux-libc-dev"
fi
command -v htop >/dev/null 2>&1 && pass "htop installed" || warn "htop not found" "sudo apt install htop  (needed for the screenshots)"

echo
if [ "$fails" -eq 0 ]; then
    echo "${GRN}All required checks passed. You are ready: run 'make && make test'.${RST}"
    exit 0
else
    echo "${RED}$fails check(s) failed. Fix them before starting the assignment.${RST}"
    exit 1
fi
