# minivmm — Cloud Computing HW1 (starter code)

A miniature type-2 hypervisor on top of Linux KVM, plus a Python control panel.
The full assignment (in Persian) is `HW1.pdf`, next to this folder; this file is only a quick map.

```
.
├── check_env.sh        # run this first: is /dev/kvm usable on this machine?
├── Makefile            # make | make test | make sweep | make clean
├── core/minivmm.c      # the VMM (C, /dev/kvm ioctls)      -> TODO 1..4
├── panel/vmctl.py      # the control panel (Python, /proc) -> TODO (Part 3..5)
└── guests/             # raw x86 guests (nasm, no OS)
    ├── hello.asm       #   prints a string on COM1 then HLT
    ├── cpu_burn.asm    #   endless loop on every vCPU, heartbeat OUT every 2^HB_SHIFT iterations
    └── mem_touch.asm   #   writes every 4 KiB page of guest RAM, slowly
```

Quick start (on an x86_64 Linux host with KVM):

```bash
./check_env.sh
make
make test                                   # works once TODO 1 and 2 are done
python3 panel/vmctl.py create hello --guest guests/hello.bin --mem 16
python3 panel/vmctl.py run hello --follow
```

Until you complete the TODOs, `make` prints a few "defined but not used"
warnings: they point at the functions you still have to wire up.

Panel state (VM definitions, logs, live stats) is kept in `./.vmctl/`
(override with the `VMCTL_HOME` environment variable).
