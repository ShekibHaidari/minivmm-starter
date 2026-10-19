; =============================================================================
; hello.asm — the "Hello, World" of virtualization.
;
; Prints a string on the (emulated) serial port COM1 and then executes HLT.
; There is no operating system and no BIOS here: these bytes are the very
; first and only instructions the virtual CPU ever executes.
;
; Every `out dx, al` below is a *sensitive* instruction: the CPU cannot let a
; guest talk to real I/O ports, so it stops the guest (VM exit), hands control
; to the hypervisor (KVM -> minivmm), which emulates the port and resumes it.
;
; Guest ABI (set up by minivmm before the first KVM_RUN):
;   32-bit protected mode, flat segments, paging off, interrupts disabled
;   loaded at guest-physical 0x1000, execution starts at 0x1000
;   EDI = vCPU id   ESI = RAM size (bytes)   EDX = number of vCPUs
;   ESP = top of a private stack for this vCPU
;
; Build: nasm -f bin -o hello.bin hello.asm
; =============================================================================
bits 32
org 0x1000

COM1_DATA   equ 0x3f8

start:
    test    edi, edi            ; only vCPU 0 talks; the others halt at once
    jnz     .halt

    mov     esi, msg
    mov     dx, COM1_DATA
.next:
    lodsb                       ; al = *esi++
    test    al, al
    jz      .halt
    out     dx, al              ; <-- trap! (KVM_EXIT_IO, port 0x3f8)
    jmp     .next

.halt:
    hlt                         ; <-- trap! (KVM_EXIT_HLT): "I am done"
    jmp     .halt               ; never resume past HLT

msg:
    db      "Hello from the guest! I am running with no OS at all.", 10, 0
