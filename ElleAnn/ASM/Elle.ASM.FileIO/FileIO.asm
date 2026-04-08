; =============================================================================
; Elle.ASM.FileIO.asm — File I/O DLL
; x64 MASM, Windows x64 ABI, strict register preservation
;
; Exports:
;   ElleWriteFile(LPCWSTR path, LPCWSTR content) -> BOOL
;       RCX = path, RDX = content
;       Creates or overwrites the file at path with content as UTF-16 LE.
;       Writes BOM (FF FE) at the start of new files.
;
;   ElleReadFile(LPCWSTR path, LPWSTR outBuffer, DWORD bufferChars) -> BOOL
;       RCX = path, RDX = outBuffer, R8 = bufferChars
;       Reads the file and writes its content (minus BOM) to outBuffer.
;
;   ElleFileExists(LPCWSTR path) -> BOOL
;       RCX = path
;       Returns TRUE if the file exists and is accessible.
;
;   ElleDeleteFile(LPCWSTR path) -> BOOL
;       RCX = path
;
; All file operations use the Windows kernel file API directly.
; No CRT dependency.
; =============================================================================

OPTION CASEMAP:NONE

EXTERN  CreateFileW:PROC
EXTERN  WriteFile:PROC
EXTERN  ReadFile:PROC
EXTERN  CloseHandle:PROC
EXTERN  DeleteFileW:PROC
EXTERN  GetFileAttributesW:PROC
EXTERN  lstrlenW:PROC
EXTERN  RtlZeroMemory:PROC

; Windows constants
GENERIC_READ            EQU 80000000h
GENERIC_WRITE           EQU 40000000h
FILE_SHARE_READ         EQU 00000001h
CREATE_ALWAYS           EQU 2
CREATE_NEW              EQU 1
OPEN_EXISTING           EQU 3
FILE_ATTRIBUTE_NORMAL   EQU 00000080h
INVALID_HANDLE_VALUE_64 EQU -1
INVALID_FILE_ATTRIBUTES EQU 0FFFFFFFFh

.DATA
    ALIGN 4
    ; UTF-16 LE BOM: FF FE
    g_BOM           DW 0FEFFh
    g_BOMWritten    DWORD 0     ; Bytes written by WriteFile for BOM
    g_BytesWritten  DWORD 0     ; Bytes written by WriteFile for content
    g_BytesRead     DWORD 0     ; Bytes read by ReadFile

.CODE

DllMain PROC
    MOV     RAX, 1
    RET
DllMain ENDP

; =============================================================================
; ElleWriteFile
;
; RCX = LPCWSTR path
; RDX = LPCWSTR content
; Returns: RAX = 1 (success), 0 (failure)
;
; Opens/creates the file with CREATE_ALWAYS (truncates if exists),
; writes UTF-16 LE BOM (FF FE), then writes the content as UTF-16 LE.
; Closes the handle before returning regardless of outcome.
; =============================================================================
ElleWriteFile PROC
    PUSH    RBX
    PUSH    RDI
    PUSH    RSI
    PUSH    R12
    PUSH    R13
    SUB     RSP, 32

    TEST    RCX, RCX
    JZ      EWF_Fail
    TEST    RDX, RDX
    JZ      EWF_Fail

    MOV     R12, RCX               ; Save path
    MOV     R13, RDX               ; Save content

    ; CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
    MOV     RCX,  R12              ; lpFileName
    MOV     EDX,  GENERIC_WRITE    ; dwDesiredAccess
    XOR     R8D,  R8D              ; dwShareMode = 0 (exclusive write)
    XOR     R9D,  R9D              ; lpSecurityAttributes = NULL
    SUB     RSP, 32
    MOV     QWORD PTR [RSP+32], CREATE_ALWAYS          ; dwCreationDisposition
    MOV     QWORD PTR [RSP+40], FILE_ATTRIBUTE_NORMAL  ; dwFlagsAndAttributes
    MOV     QWORD PTR [RSP+48], 0                      ; hTemplateFile = NULL
    CALL    CreateFileW
    ADD     RSP, 32

    CMP     RAX, INVALID_HANDLE_VALUE_64
    JE      EWF_Fail

    MOV     RBX, RAX               ; Save hFile

    ; Write BOM (FF FE) — 2 bytes
    MOV     RCX, RBX               ; hFile
    LEA     RDX, g_BOM             ; lpBuffer
    MOV     R8D, 2                 ; nNumberOfBytesToWrite
    LEA     R9, g_BOMWritten       ; lpNumberOfBytesWritten
    SUB     RSP, 32
    MOV     QWORD PTR [RSP+32], 0  ; lpOverlapped = NULL
    CALL    WriteFile
    ADD     RSP, 32
    TEST    EAX, EAX
    JZ      EWF_CloseAndFail

    ; Compute content byte length: lstrlenW(content) * 2
    MOV     RCX, R13
    CALL    lstrlenW
    ; EAX = wchar count, multiply by 2 for byte count
    SHL     EAX, 1
    MOV     RDI, RAX               ; Save byte count in RDI

    TEST    RDI, RDI               ; If content is empty string, skip write
    JZ      EWF_Success

    ; WriteFile(hFile, content, byteCount, &bytesWritten, NULL)
    MOV     RCX, RBX               ; hFile
    MOV     RDX, R13               ; lpBuffer = content
    MOV     R8,  RDI               ; nNumberOfBytesToWrite
    LEA     R9,  g_BytesWritten    ; lpNumberOfBytesWritten
    SUB     RSP, 32
    MOV     QWORD PTR [RSP+32], 0  ; lpOverlapped = NULL
    CALL    WriteFile
    ADD     RSP, 32
    TEST    EAX, EAX
    JZ      EWF_CloseAndFail

EWF_Success:
    MOV     RCX, RBX
    CALL    CloseHandle
    MOV     RAX, 1
    JMP     EWF_Exit

EWF_CloseAndFail:
    MOV     RCX, RBX
    CALL    CloseHandle

EWF_Fail:
    XOR     RAX, RAX

EWF_Exit:
    ADD     RSP, 32
    POP     R13
    POP     R12
    POP     RSI
    POP     RDI
    POP     RBX
    RET
ElleWriteFile ENDP

; =============================================================================
; ElleReadFile
;
; RCX = LPCWSTR path
; RDX = LPWSTR outBuffer
; R8  = DWORD bufferChars
; Returns: RAX = 1 (success, content in outBuffer), 0 (failure)
;
; Opens the file with OPEN_EXISTING, reads into outBuffer as raw bytes,
; null-terminates, and returns. Skips the BOM (first 2 bytes) if present.
; Maximum read is (bufferChars - 1) * 2 bytes to leave room for null terminator.
; =============================================================================
ElleReadFile PROC
    PUSH    RBX
    PUSH    RDI
    PUSH    RSI
    PUSH    R12
    PUSH    R13
    PUSH    R14
    SUB     RSP, 32

    TEST    RCX, RCX
    JZ      ERF_Fail
    TEST    RDX, RDX
    JZ      ERF_Fail
    TEST    R8D, R8D
    JZ      ERF_Fail

    MOV     R12, RCX               ; Save path
    MOV     R13, RDX               ; Save outBuffer
    MOV     R14D, R8D              ; Save bufferChars

    ; Null-terminate outBuffer at position 0 as a safety measure
    MOV     WORD PTR [R13], 0

    ; CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
    MOV     RCX,  R12
    MOV     EDX,  GENERIC_READ
    MOV     R8D,  FILE_SHARE_READ
    XOR     R9D,  R9D
    SUB     RSP, 32
    MOV     QWORD PTR [RSP+32], OPEN_EXISTING
    MOV     QWORD PTR [RSP+40], FILE_ATTRIBUTE_NORMAL
    MOV     QWORD PTR [RSP+48], 0
    CALL    CreateFileW
    ADD     RSP, 32

    CMP     RAX, INVALID_HANDLE_VALUE_64
    JE      ERF_Fail

    MOV     RBX, RAX               ; Save hFile

    ; Read into outBuffer. Max bytes = (bufferChars - 1) * 2
    MOV     EAX, R14D
    DEC     EAX                    ; bufferChars - 1
    SHL     EAX, 1                 ; * 2 (bytes)
    MOV     RDI, RAX               ; RDI = maxBytesToRead

    ; ReadFile(hFile, outBuffer, maxBytesToRead, &bytesRead, NULL)
    MOV     RCX, RBX               ; hFile
    MOV     RDX, R13               ; lpBuffer
    MOV     R8,  RDI               ; nNumberOfBytesToRead
    LEA     R9,  g_BytesRead       ; lpNumberOfBytesRead
    SUB     RSP, 32
    MOV     QWORD PTR [RSP+32], 0
    CALL    ReadFile
    ADD     RSP, 32
    TEST    EAX, EAX
    JZ      ERF_CloseAndFail

    ; Null-terminate — bytesRead / 2 = wchar count, write null at that position
    MOV     EAX, DWORD PTR [g_BytesRead]
    SHR     EAX, 1                 ; / 2 = wchar position
    ; outBuffer[wcharPos] = 0
    MOV     RSI, R13
    MOVZX   RAX, AX
    MOV     WORD PTR [RSI + RAX*2], 0

    ; Skip BOM if present (first wchar == 0xFEFF)
    MOV     AX, WORD PTR [R13]
    CMP     AX, 0FEFFh
    JNE     ERF_Success            ; No BOM — buffer is fine as-is

    ; BOM present — move buffer content left by 2 bytes to strip it
    ; We do this by adjusting the return pointer the caller sees would require
    ; a different interface; instead we shift in-place using REP MOVSW
    ; Source = outBuffer + 1 (skip BOM wchar), Dest = outBuffer
    LEA     RSI, [R13 + 2]         ; Source: skip first wchar (BOM)
    MOV     RDI, R13               ; Dest: start of buffer
    MOV     ECX, DWORD PTR [g_BytesRead]
    SHR     ECX, 1                 ; Convert bytes to wchars
    DEC     ECX                    ; One less because we skipped the BOM
    REP     MOVSW                  ; Copy wchars left by 1 position
    MOV     WORD PTR [RDI], 0      ; Null-terminate after the moved content

ERF_Success:
    MOV     RCX, RBX
    CALL    CloseHandle
    MOV     RAX, 1
    JMP     ERF_Exit

ERF_CloseAndFail:
    MOV     RCX, RBX
    CALL    CloseHandle

ERF_Fail:
    XOR     RAX, RAX

ERF_Exit:
    ADD     RSP, 32
    POP     R14
    POP     R13
    POP     R12
    POP     RSI
    POP     RDI
    POP     RBX
    RET
ElleReadFile ENDP

; =============================================================================
; ElleFileExists
;
; RCX = LPCWSTR path
; Returns: RAX = 1 (file exists), 0 (does not exist or error)
;
; Uses GetFileAttributesW — if it returns INVALID_FILE_ATTRIBUTES the file
; doesn't exist. If it returns a value with FILE_ATTRIBUTE_DIRECTORY set,
; it's a directory, not a file (return 0).
; =============================================================================
ElleFileExists PROC
    PUSH    RBX
    SUB     RSP, 32

    TEST    RCX, RCX
    JZ      EFE_Fail

    ; GetFileAttributesW(path)
    CALL    GetFileAttributesW
    CMP     EAX, INVALID_FILE_ATTRIBUTES
    JE      EFE_Fail               ; File not found or access denied

    ; Check if it's a directory (FILE_ATTRIBUTE_DIRECTORY = 0x10)
    TEST    EAX, 10h
    JNZ     EFE_Fail               ; It's a directory, not a file

    MOV     RAX, 1
    JMP     EFE_Exit

EFE_Fail:
    XOR     RAX, RAX

EFE_Exit:
    ADD     RSP, 32
    POP     RBX
    RET
ElleFileExists ENDP

; =============================================================================
; ElleDeleteFile
;
; RCX = LPCWSTR path
; Returns: RAX = 1 (success), 0 (failure)
; =============================================================================
ElleDeleteFile PROC
    PUSH    RBX
    SUB     RSP, 32

    TEST    RCX, RCX
    JZ      EDF_Fail

    ; DeleteFileW(path) — returns nonzero on success
    CALL    DeleteFileW
    TEST    EAX, EAX
    JZ      EDF_Fail

    MOV     RAX, 1
    JMP     EDF_Exit

EDF_Fail:
    XOR     RAX, RAX

EDF_Exit:
    ADD     RSP, 32
    POP     RBX
    RET
ElleDeleteFile ENDP

END
