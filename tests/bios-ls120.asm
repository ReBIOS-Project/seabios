; DOS .COM: BIOS A: geometry, boot-sector read, last-sector write/read/restore.
; Use only with disposable test images. No ASPI or filesystem driver involved.
bits 16
org 100h
start:
    push ds
    pop es
    xor dx,dx
    mov ah,08h
    int 13h
    jc failed
    or dl,dl
    jz failed
    and dx,0ff00h
    mov [last_cx],cx
    mov [last_dx],dx
    mov ax,ds
    mov es,ax
    mov bx,saved
    mov ax,0201h
    mov cx,1
    xor dx,dx
    int 13h
    jc failed
    cmp word [saved+510],0aa55h
    jne failed
    cmp word [saved+11],512
    jne failed
    mov ax,0201h
    mov bx,saved
    call last_sector
    jc failed
    mov di,pattern
    mov cx,256
    mov ax,0a55ah
    rep stosw
    mov ax,0301h
    mov bx,pattern
    call last_sector
    jc restore_failed
    mov ax,0201h
    mov bx,readback
    call last_sector
    jc restore_failed
    mov si,pattern
    mov di,readback
    mov cx,256
    repe cmpsw
    jne restore_failed
    mov ax,0301h
    mov bx,saved
    call last_sector
    jc failed
    mov dx,passed
    mov ah,09h
    int 21h
    mov ax,4c00h
    int 21h
restore_failed:
    mov ax,0301h
    mov bx,saved
    call last_sector
failed:
    mov dx,failure
    mov ah,09h
    int 21h
    mov ax,4c01h
    int 21h
last_sector:
    mov cx,[last_cx]
    mov dx,[last_dx]
    int 13h
    ret
last_cx dw 0
last_dx dw 0
passed db 'BIOS LS120 A: read/write/restore passed',13,10,'$'
failure db 'BIOS LS120 A: FAILED',13,10,'$'
align 2
saved times 512 db 0
pattern times 512 db 0
readback times 512 db 0
