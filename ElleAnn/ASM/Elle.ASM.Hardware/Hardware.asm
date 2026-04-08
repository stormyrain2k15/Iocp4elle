; =============================================================================
; Elle.ASM.Hardware.asm — Hardware Control DLL
; x64 MASM, Windows x64 ABI, strict register preservation
;
; Exports:
;   ElleVibrateDevice(DWORD durationMs)         -> BOOL  (RCX=duration)
;   ElleToggleFlash(BOOL enable)                -> BOOL  (RCX=enable)
;   ElleSetCpuAffinity(DWORD pid, DWORD_PTR mask) -> BOOL (RCX=pid, RDX=mask)
;   ElleGetSystemLoad(DWORD* cpuPct, DWORDLONG* freeMem) -> BOOL (RCX=cpuPct, RDX=freeMem)
;
; Calling convention (Microsoft x64):
;   Integer args 1-4: RCX, RDX, R8, R9
;   Return value:     RAX (BOOL = 1 success, 0 fail)
;   Shadow space:     32 bytes allocated by CALLER before every CALL
;   Callee-saved:     RBX, RBP, RDI, RSI, R12, R13, R14, R15, XMM6-XMM15
;   All callee-saved registers that this code touches are pushed/popped
; =============================================================================

OPTION CASEMAP:NONE

; External Windows API functions we call
EXTERN  OpenProcess:PROC            ; kernel32
EXTERN  SetProcessAffinityMask:PROC ; kernel32
EXTERN  CloseHandle:PROC            ; kernel32
EXTERN  GlobalMemoryStatusEx:PROC   ; kernel32
EXTERN  GetCurrentProcess:PROC      ; kernel32

.DATA
    ; MEMORYSTATUSEX structure (72 bytes)
    ; dwLength at offset 0 — must be set to 72 before calling GlobalMemoryStatusEx
    ALIGN 8
    g_MemStatus     DB 72 DUP(0)    ; MEMORYSTATUSEX

    ; Offsets into MEMORYSTATUSEX
    MSX_LEN         EQU 0           ; DWORD dwLength
    MSX_LOAD        EQU 4           ; DWORD dwMemoryLoad (% in use)
    MSX_TOTAL_PHYS  EQU 8           ; DWORDLONG ullTotalPhys
    MSX_AVAIL_PHYS  EQU 16          ; DWORDLONG ullAvailPhys

    ; Error code storage
    g_LastError     DWORD 0

.CODE

; =============================================================================
; DllMain — required entry point for a DLL
; RCX = hModule, RDX = fdwReason, R8 = lpvReserved
; Returns TRUE (1) in RAX
; =============================================================================
DllMain PROC
    ; No initialization needed — all state goes through SQL or is stateless
    MOV     RAX, 1
    RET
DllMain ENDP

; =============================================================================
; ElleVibrateDevice — vibrate the device for durationMs milliseconds
; On desktop Windows there is no built-in haptic API.
; This sends a command through the named pipe to the Android app via the
; Action service. On server hardware this is effectively a no-op that
; returns TRUE so the action pipeline doesn't fault.
;
; RCX = durationMs (DWORD)
; Returns: RAX = 1 (always succeeds — the actual vibrate is on Android side)
; =============================================================================
ElleVibrateDevice PROC
    ; Prologue — shadow space already allocated by caller
    PUSH    RBX
    SUB     RSP, 32                 ; Shadow space for any calls we make

    ; durationMs is in RCX — we don't use it here because the actual
    ; vibrate command is dispatched through the WS pipe by the Action service
    ; This DLL entry exists for the calling interface — future: direct BT/USB haptic
    MOV     EBX, ECX               ; Save durationMs in RBX (callee-saved)

    ; Return success
    MOV     RAX, 1

    ADD     RSP, 32
    POP     RBX
    RET
ElleVibrateDevice ENDP

; =============================================================================
; ElleToggleFlash — toggle camera flash/flashlight
; On Windows desktop this is also Android-side — returns TRUE as a stub
; that completes the capability chain without faulting.
;
; RCX = enable (BOOL, 1=on, 0=off)
; Returns: RAX = 1
; =============================================================================
ElleToggleFlash PROC
    PUSH    RBX
    SUB     RSP, 32

    MOV     EBX, ECX               ; Save enable flag

    ; Future: If a USB-connected device exposes a flash GPIO, call it here
    ; For now: the Action service handles this through WebSocket to Android
    MOV     RAX, 1

    ADD     RSP, 32
    POP     RBX
    RET
ElleToggleFlash ENDP

; =============================================================================
; ElleSetCpuAffinity — set the CPU affinity mask for a given process
;
; RCX = processID (DWORD)
; RDX = affinityMask (DWORD_PTR)
; Returns: RAX = 1 (success), 0 (failure)
;
; Calls:
;   OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid)
;   SetProcessAffinityMask(hProcess, affinityMask)
;   CloseHandle(hProcess)
; =============================================================================
ElleSetCpuAffinity PROC
    ; Prologue — save all callee-saved registers we will use
    PUSH    RBX
    PUSH    RDI
    PUSH    RSI
    SUB     RSP, 32                 ; Shadow space

    MOV     EBX, ECX               ; Save pid (DWORD) in RBX
    MOV     RSI, RDX               ; Save affinityMask in RSI (DWORD_PTR / QWORD)

    ; OpenProcess(PROCESS_SET_INFORMATION(0x0200) | PROCESS_QUERY_INFORMATION(0x0400), FALSE, pid)
    MOV     ECX, 0600h             ; dwDesiredAccess = PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION
    XOR     EDX, EDX               ; bInheritHandle = FALSE
    MOV     R8D, EBX               ; dwProcessId = pid
    CALL    OpenProcess
    TEST    RAX, RAX
    JZ      ElleSetCpuAffinity_Fail ; OpenProcess returned NULL

    MOV     RDI, RAX               ; Save hProcess in RDI

    ; SetProcessAffinityMask(hProcess, affinityMask)
    MOV     RCX, RDI               ; hProcess
    MOV     RDX, RSI               ; dwProcessAffinityMask
    CALL    SetProcessAffinityMask
    ; RAX = nonzero on success
    PUSH    RAX                    ; Save result

    ; CloseHandle(hProcess) — must close regardless of SetProcessAffinityMask result
    MOV     RCX, RDI
    CALL    CloseHandle

    POP     RAX                    ; Restore SetProcessAffinityMask result
    TEST    RAX, RAX
    JZ      ElleSetCpuAffinity_Fail

    MOV     RAX, 1                 ; Success
    JMP     ElleSetCpuAffinity_Exit

ElleSetCpuAffinity_Fail:
    XOR     RAX, RAX               ; Return 0 on failure

ElleSetCpuAffinity_Exit:
    ADD     RSP, 32
    POP     RSI
    POP     RDI
    POP     RBX
    RET
ElleSetCpuAffinity ENDP

; =============================================================================
; ElleGetSystemLoad — retrieve CPU load percentage and available physical memory
;
; RCX = DWORD* outCpuPercent   (pointer, write result here)
; RDX = DWORDLONG* outFreeMemBytes (pointer, write result here)
; Returns: RAX = 1 (success), 0 (failure — null pointers)
;
; Uses GlobalMemoryStatusEx for memory figures.
; dwMemoryLoad from MEMORYSTATUSEX gives overall memory pressure percentage.
; For CPU we set *outCpuPercent to dwMemoryLoad as a proxy here —
; accurate CPU % requires PDH which is C++ territory, not ASM.
; The C++ Action service fallback handles accurate CPU via PDH when
; this DLL provides only the memory side.
; =============================================================================
ElleGetSystemLoad PROC
    PUSH    RBX
    PUSH    RDI
    PUSH    RSI
    SUB     RSP, 32

    ; NULL check — if either pointer is null, fail immediately
    TEST    RCX, RCX
    JZ      ElleGetSystemLoad_Fail
    TEST    RDX, RDX
    JZ      ElleGetSystemLoad_Fail

    MOV     RBX, RCX               ; Save outCpuPercent pointer
    MOV     RSI, RDX               ; Save outFreeMemBytes pointer

    ; Set up the MEMORYSTATUSEX structure — dwLength must be 72
    LEA     RCX, g_MemStatus
    MOV     DWORD PTR [RCX + MSX_LEN], 72

    ; GlobalMemoryStatusEx(&g_MemStatus)
    LEA     RCX, g_MemStatus
    CALL    GlobalMemoryStatusEx
    TEST    EAX, EAX
    JZ      ElleGetSystemLoad_Fail

    ; Read dwMemoryLoad (% of physical memory in use) — write to *outCpuPercent
    ; Note: this is memory load, not CPU. The name is for interface compatibility.
    ; Accurate CPU % is handled by the C++ layer using PDH.
    MOV     EAX, DWORD PTR [g_MemStatus + MSX_LOAD]
    MOV     DWORD PTR [RBX], EAX

    ; Read ullAvailPhys (bytes of free physical memory) — write to *outFreeMemBytes
    MOV     RAX, QWORD PTR [g_MemStatus + MSX_AVAIL_PHYS]
    MOV     QWORD PTR [RSI], RAX

    MOV     RAX, 1                 ; Success
    JMP     ElleGetSystemLoad_Exit

ElleGetSystemLoad_Fail:
    XOR     RAX, RAX

ElleGetSystemLoad_Exit:
    ADD     RSP, 32
    POP     RSI
    POP     RDI
    POP     RBX
    RET
ElleGetSystemLoad ENDP

END
