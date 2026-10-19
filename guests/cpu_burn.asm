; =============================================================================
; cpu_burn.asm — a CPU-hungry guest for the monitoring experiments.
;
; Every vCPU runs an endless pseudo-random number loop (xorshift32). Pure
; arithmetic is *not* sensitive, so it runs natively in VMX non-root mode at
; full speed without any VM exit.
;
; Once every 2^HB_SHIFT iterations the guest writes a "heartbeat" to port 0x80.
; That OUT is sensitive and causes one KVM_EXIT_IO. By rebuilding the guest
; with different HB_SHIFT values you control how often the guest traps to the
; hypervisor and can measure the cost of a VM exit:
;
;       iterations/second = heartbeat_exits/second * 2^HB_SHIFT
;
; Build: nasm -f bin -DHB_SHIFT=24 -o cpu_burn.bin cpu_burn.asm
;        (or simply: make guests/cpu_burn_hb8.bin)
;
; Guest ABI: see hello.asm. EDI = vCPU id.
; =============================================================================
bits 32
org 0x1000

COM1_DATA       equ 0x3f8
HEARTBEAT_PORT  equ 0x80

%ifndef HB_SHIFT
%define HB_SHIFT 24            ; one heartbeat exit every 2^24 (~16.7M) iterations
%endif
%if HB_SHIFT < 0 || HB_SHIFT > 31
%error "HB_SHIFT must be in [0, 31]"
%endif

start:
    test    edi, edi            ; vCPU 0 prints a banner
    jnz     .setup
    mov     esi, banner
    mov     dx, COM1_DATA
.print:
    lodsb
    test    al, al
    jz      .setup
    out     dx, al
    jmp     .print

.setup:
    lea     ebx, [edi + 0x9e3779b9] ; per-vCPU non-zero seed
    xor     ecx, ecx                ; iteration counter

.loop:
    ; --- xorshift32: the "useful work" -------------------------------------
    mov     eax, ebx
    shl     eax, 13
    xor     ebx, eax
    mov     eax, ebx
    shr     eax, 17
    xor     ebx, eax
    mov     eax, ebx
    shl     eax, 5
    xor     ebx, eax
    ; --- heartbeat every 2^HB_SHIFT iterations ------------------------------
    inc     ecx
    test    ecx, (1 << HB_SHIFT) - 1
    jnz     .loop
    mov     al, bl
    out     HEARTBEAT_PORT, al      ; <-- trap! (KVM_EXIT_IO, port 0x80)
    jmp     .loop

banner:
    db      "cpu_burn: all vCPUs are spinning (stop me with vmctl stop)", 10, 0
