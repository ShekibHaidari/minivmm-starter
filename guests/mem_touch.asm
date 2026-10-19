; =============================================================================
; mem_touch.asm — touches (writes to) every page of guest RAM, slowly.
;
; The hypervisor mmap()s guest RAM lazily: the host kernel gives it real
; physical pages only when a page is touched for the first time (demand
; paging). This guest writes one byte into every 4 KiB page from 1 MiB up to
; the end of RAM, printing its progress every 16 MiB and pausing a little so
; you can watch the VM process's RSS grow in /proc/<pid>/status or htop.
;
; Note: these first-touch faults are EPT/NPT violations that KVM handles
; *inside the kernel*; they never show up as exits in minivmm's own table.
;
; After touching everything it keeps the memory resident and spins until it
; is stopped (vmctl stop <name>). Only vCPU 0 works; other vCPUs halt.
;
; Guest ABI: see hello.asm. EDI = vCPU id, ESI = RAM size in bytes,
;            ESP = private stack (used by call/push below).
; Build: nasm -f bin -o mem_touch.bin mem_touch.asm
; =============================================================================
bits 32
org 0x1000

COM1_DATA   equ 0x3f8
PAGE        equ 4096
TOUCH_START equ 0x100000        ; 1 MiB: well above our code and below the stacks
REPORT_MASK equ 0xffffff        ; report every 16 MiB
%ifndef DELAY_LOOPS
%define DELAY_LOOPS 0x08000000    ; busy-wait between reports (~0.1-0.3 s)
%endif

start:
    test    edi, edi
    jnz     halt_forever

    mov     ebp, esi                ; ebp = RAM size (keep it; esi is reused)

    mov     esi, msg_start
    call    print_str
    mov     eax, ebp
    shr     eax, 20
    call    print_dec               ; "<N>"
    mov     esi, msg_mib
    call    print_str

    mov     ebx, TOUCH_START
.touch:
    cmp     ebx, ebp
    jae     .done
    mov     byte [ebx], 0xAA        ; first write => host allocates a real page
    add     ebx, PAGE
    test    ebx, REPORT_MASK        ; crossed a 16 MiB boundary?
    jnz     .touch

    mov     esi, msg_progress       ; "  touched up to <N> MiB"
    call    print_str
    mov     eax, ebx
    shr     eax, 20
    call    print_dec
    mov     esi, msg_mib
    call    print_str

    mov     ecx, DELAY_LOOPS        ; slow down so humans can watch RSS grow
.delay:
    pause
    dec     ecx
    jnz     .delay
    jmp     .touch

.done:
    mov     esi, msg_done
    call    print_str
.hold:                              ; keep the memory, wait to be stopped
    pause
    jmp     .hold

halt_forever:
    hlt
    jmp     halt_forever

; ---- print_str: print zero-terminated string at ESI on COM1 ----------------
print_str:
    push    eax
    push    edx
    mov     dx, COM1_DATA
.n: lodsb
    test    al, al
    jz      .e
    out     dx, al
    jmp     .n
.e: pop     edx
    pop     eax
    ret

; ---- print_dec: print unsigned EAX in decimal on COM1 ----------------------
print_dec:
    push    ebx
    push    ecx
    push    edx
    mov     ebx, 10
    xor     ecx, ecx
.div:
    xor     edx, edx
    div     ebx                     ; eax = eax / 10, edx = digit
    push    edx
    inc     ecx
    test    eax, eax
    jnz     .div
    mov     dx, COM1_DATA
.out:
    pop     eax
    add     al, '0'
    out     dx, al
    loop    .out
    pop     edx
    pop     ecx
    pop     ebx
    ret

msg_start:    db "mem_touch: guest RAM = ", 0
msg_progress: db "  touched up to ", 0
msg_mib:      db " MiB", 10, 0
msg_done:     db "mem_touch: every page touched; holding memory until stopped", 10, 0
