.586
.MODEL flat, C
.STACK 4096
.DATA
.CODE
PUBLIC add

add PROC
    push ebp
    mov ebp, esp
    mov eax, [ebp+8]
    mov edx, [ebp+12]
    add eax, edx
    pop ebp
    ret
add ENDP

END
