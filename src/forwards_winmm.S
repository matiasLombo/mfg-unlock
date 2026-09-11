# Generado por tools/gen_forwards.py -- no editar a mano.
# Cada export de winmm.dll: guarda su indice en eax y salta al resolvedor
# comun, que llama a forward_resolve(idx) preservando los registros de
# argumentos y salta a la direccion real. rax es scratch en x64, asi que
# usarlo para el indice no pisa ningun argumento.
    .text
    .p2align 4
forward_common:
    push %rcx
    push %rdx
    push %r8
    push %r9
    sub $0x68, %rsp                 # shadow (0x20) + 4 xmm (0x40) + relleno: queda alineado a 16
    movdqu %xmm0, 0x20(%rsp)
    movdqu %xmm1, 0x30(%rsp)
    movdqu %xmm2, 0x40(%rsp)
    movdqu %xmm3, 0x50(%rsp)
    mov %eax, %ecx
    call forward_resolve
    movdqu 0x20(%rsp), %xmm0
    movdqu 0x30(%rsp), %xmm1
    movdqu 0x40(%rsp), %xmm2
    movdqu 0x50(%rsp), %xmm3
    add $0x68, %rsp
    pop %r9
    pop %r8
    pop %rdx
    pop %rcx
    jmp *%rax

    .globl CloseDriver
    .def CloseDriver; .scl 2; .type 32; .endef
CloseDriver:
    mov $0, %eax
    jmp forward_common

    .globl DefDriverProc
    .def DefDriverProc; .scl 2; .type 32; .endef
DefDriverProc:
    mov $1, %eax
    jmp forward_common

    .globl DriverCallback
    .def DriverCallback; .scl 2; .type 32; .endef
DriverCallback:
    mov $2, %eax
    jmp forward_common

    .globl DrvGetModuleHandle
    .def DrvGetModuleHandle; .scl 2; .type 32; .endef
DrvGetModuleHandle:
    mov $3, %eax
    jmp forward_common

    .globl GetDriverModuleHandle
    .def GetDriverModuleHandle; .scl 2; .type 32; .endef
GetDriverModuleHandle:
    mov $4, %eax
    jmp forward_common

    .globl OpenDriver
    .def OpenDriver; .scl 2; .type 32; .endef
OpenDriver:
    mov $5, %eax
    jmp forward_common

    .globl PlaySound
    .def PlaySound; .scl 2; .type 32; .endef
PlaySound:
    mov $6, %eax
    jmp forward_common

    .globl PlaySoundA
    .def PlaySoundA; .scl 2; .type 32; .endef
PlaySoundA:
    mov $7, %eax
    jmp forward_common

    .globl PlaySoundW
    .def PlaySoundW; .scl 2; .type 32; .endef
PlaySoundW:
    mov $8, %eax
    jmp forward_common

    .globl SendDriverMessage
    .def SendDriverMessage; .scl 2; .type 32; .endef
SendDriverMessage:
    mov $9, %eax
    jmp forward_common

    .globl WOWAppExit
    .def WOWAppExit; .scl 2; .type 32; .endef
WOWAppExit:
    mov $10, %eax
    jmp forward_common

    .globl auxGetDevCapsA
    .def auxGetDevCapsA; .scl 2; .type 32; .endef
auxGetDevCapsA:
    mov $11, %eax
    jmp forward_common

    .globl auxGetDevCapsW
    .def auxGetDevCapsW; .scl 2; .type 32; .endef
auxGetDevCapsW:
    mov $12, %eax
    jmp forward_common

    .globl auxGetNumDevs
    .def auxGetNumDevs; .scl 2; .type 32; .endef
auxGetNumDevs:
    mov $13, %eax
    jmp forward_common

    .globl auxGetVolume
    .def auxGetVolume; .scl 2; .type 32; .endef
auxGetVolume:
    mov $14, %eax
    jmp forward_common

    .globl auxOutMessage
    .def auxOutMessage; .scl 2; .type 32; .endef
auxOutMessage:
    mov $15, %eax
    jmp forward_common

    .globl auxSetVolume
    .def auxSetVolume; .scl 2; .type 32; .endef
auxSetVolume:
    mov $16, %eax
    jmp forward_common

    .globl joyConfigChanged
    .def joyConfigChanged; .scl 2; .type 32; .endef
joyConfigChanged:
    mov $17, %eax
    jmp forward_common

    .globl joyGetDevCapsA
    .def joyGetDevCapsA; .scl 2; .type 32; .endef
joyGetDevCapsA:
    mov $18, %eax
    jmp forward_common

    .globl joyGetDevCapsW
    .def joyGetDevCapsW; .scl 2; .type 32; .endef
joyGetDevCapsW:
    mov $19, %eax
    jmp forward_common

    .globl joyGetNumDevs
    .def joyGetNumDevs; .scl 2; .type 32; .endef
joyGetNumDevs:
    mov $20, %eax
    jmp forward_common

    .globl joyGetPos
    .def joyGetPos; .scl 2; .type 32; .endef
joyGetPos:
    mov $21, %eax
    jmp forward_common

    .globl joyGetPosEx
    .def joyGetPosEx; .scl 2; .type 32; .endef
joyGetPosEx:
    mov $22, %eax
    jmp forward_common

    .globl joyGetThreshold
    .def joyGetThreshold; .scl 2; .type 32; .endef
joyGetThreshold:
    mov $23, %eax
    jmp forward_common

    .globl joyReleaseCapture
    .def joyReleaseCapture; .scl 2; .type 32; .endef
joyReleaseCapture:
    mov $24, %eax
    jmp forward_common

    .globl joySetCapture
    .def joySetCapture; .scl 2; .type 32; .endef
joySetCapture:
    mov $25, %eax
    jmp forward_common

    .globl joySetThreshold
    .def joySetThreshold; .scl 2; .type 32; .endef
joySetThreshold:
    mov $26, %eax
    jmp forward_common

    .globl mciDriverNotify
    .def mciDriverNotify; .scl 2; .type 32; .endef
mciDriverNotify:
    mov $27, %eax
    jmp forward_common

    .globl mciDriverYield
    .def mciDriverYield; .scl 2; .type 32; .endef
mciDriverYield:
    mov $28, %eax
    jmp forward_common

    .globl mciExecute
    .def mciExecute; .scl 2; .type 32; .endef
mciExecute:
    mov $29, %eax
    jmp forward_common

    .globl mciFreeCommandResource
    .def mciFreeCommandResource; .scl 2; .type 32; .endef
mciFreeCommandResource:
    mov $30, %eax
    jmp forward_common

    .globl mciGetCreatorTask
    .def mciGetCreatorTask; .scl 2; .type 32; .endef
mciGetCreatorTask:
    mov $31, %eax
    jmp forward_common

    .globl mciGetDeviceIDA
    .def mciGetDeviceIDA; .scl 2; .type 32; .endef
mciGetDeviceIDA:
    mov $32, %eax
    jmp forward_common

    .globl mciGetDeviceIDFromElementIDA
    .def mciGetDeviceIDFromElementIDA; .scl 2; .type 32; .endef
mciGetDeviceIDFromElementIDA:
    mov $33, %eax
    jmp forward_common

    .globl mciGetDeviceIDFromElementIDW
    .def mciGetDeviceIDFromElementIDW; .scl 2; .type 32; .endef
mciGetDeviceIDFromElementIDW:
    mov $34, %eax
    jmp forward_common

    .globl mciGetDeviceIDW
    .def mciGetDeviceIDW; .scl 2; .type 32; .endef
mciGetDeviceIDW:
    mov $35, %eax
    jmp forward_common

    .globl mciGetDriverData
    .def mciGetDriverData; .scl 2; .type 32; .endef
mciGetDriverData:
    mov $36, %eax
    jmp forward_common

    .globl mciGetErrorStringA
    .def mciGetErrorStringA; .scl 2; .type 32; .endef
mciGetErrorStringA:
    mov $37, %eax
    jmp forward_common

    .globl mciGetErrorStringW
    .def mciGetErrorStringW; .scl 2; .type 32; .endef
mciGetErrorStringW:
    mov $38, %eax
    jmp forward_common

    .globl mciGetYieldProc
    .def mciGetYieldProc; .scl 2; .type 32; .endef
mciGetYieldProc:
    mov $39, %eax
    jmp forward_common

    .globl mciLoadCommandResource
    .def mciLoadCommandResource; .scl 2; .type 32; .endef
mciLoadCommandResource:
    mov $40, %eax
    jmp forward_common

    .globl mciSendCommandA
    .def mciSendCommandA; .scl 2; .type 32; .endef
mciSendCommandA:
    mov $41, %eax
    jmp forward_common

    .globl mciSendCommandW
    .def mciSendCommandW; .scl 2; .type 32; .endef
mciSendCommandW:
    mov $42, %eax
    jmp forward_common

    .globl mciSendStringA
    .def mciSendStringA; .scl 2; .type 32; .endef
mciSendStringA:
    mov $43, %eax
    jmp forward_common

    .globl mciSendStringW
    .def mciSendStringW; .scl 2; .type 32; .endef
mciSendStringW:
    mov $44, %eax
    jmp forward_common

    .globl mciSetDriverData
    .def mciSetDriverData; .scl 2; .type 32; .endef
mciSetDriverData:
    mov $45, %eax
    jmp forward_common

    .globl mciSetYieldProc
    .def mciSetYieldProc; .scl 2; .type 32; .endef
mciSetYieldProc:
    mov $46, %eax
    jmp forward_common

    .globl midiConnect
    .def midiConnect; .scl 2; .type 32; .endef
midiConnect:
    mov $47, %eax
    jmp forward_common

    .globl midiDisconnect
    .def midiDisconnect; .scl 2; .type 32; .endef
midiDisconnect:
    mov $48, %eax
    jmp forward_common

    .globl midiInAddBuffer
    .def midiInAddBuffer; .scl 2; .type 32; .endef
midiInAddBuffer:
    mov $49, %eax
    jmp forward_common

    .globl midiInClose
    .def midiInClose; .scl 2; .type 32; .endef
midiInClose:
    mov $50, %eax
    jmp forward_common

    .globl midiInGetDevCapsA
    .def midiInGetDevCapsA; .scl 2; .type 32; .endef
midiInGetDevCapsA:
    mov $51, %eax
    jmp forward_common

    .globl midiInGetDevCapsW
    .def midiInGetDevCapsW; .scl 2; .type 32; .endef
midiInGetDevCapsW:
    mov $52, %eax
    jmp forward_common

    .globl midiInGetErrorTextA
    .def midiInGetErrorTextA; .scl 2; .type 32; .endef
midiInGetErrorTextA:
    mov $53, %eax
    jmp forward_common

    .globl midiInGetErrorTextW
    .def midiInGetErrorTextW; .scl 2; .type 32; .endef
midiInGetErrorTextW:
    mov $54, %eax
    jmp forward_common

    .globl midiInGetID
    .def midiInGetID; .scl 2; .type 32; .endef
midiInGetID:
    mov $55, %eax
    jmp forward_common

    .globl midiInGetNumDevs
    .def midiInGetNumDevs; .scl 2; .type 32; .endef
midiInGetNumDevs:
    mov $56, %eax
    jmp forward_common

    .globl midiInMessage
    .def midiInMessage; .scl 2; .type 32; .endef
midiInMessage:
    mov $57, %eax
    jmp forward_common

    .globl midiInOpen
    .def midiInOpen; .scl 2; .type 32; .endef
midiInOpen:
    mov $58, %eax
    jmp forward_common

    .globl midiInPrepareHeader
    .def midiInPrepareHeader; .scl 2; .type 32; .endef
midiInPrepareHeader:
    mov $59, %eax
    jmp forward_common

    .globl midiInReset
    .def midiInReset; .scl 2; .type 32; .endef
midiInReset:
    mov $60, %eax
    jmp forward_common

    .globl midiInStart
    .def midiInStart; .scl 2; .type 32; .endef
midiInStart:
    mov $61, %eax
    jmp forward_common

    .globl midiInStop
    .def midiInStop; .scl 2; .type 32; .endef
midiInStop:
    mov $62, %eax
    jmp forward_common

    .globl midiInUnprepareHeader
    .def midiInUnprepareHeader; .scl 2; .type 32; .endef
midiInUnprepareHeader:
    mov $63, %eax
    jmp forward_common

    .globl midiOutCacheDrumPatches
    .def midiOutCacheDrumPatches; .scl 2; .type 32; .endef
midiOutCacheDrumPatches:
    mov $64, %eax
    jmp forward_common

    .globl midiOutCachePatches
    .def midiOutCachePatches; .scl 2; .type 32; .endef
midiOutCachePatches:
    mov $65, %eax
    jmp forward_common

    .globl midiOutClose
    .def midiOutClose; .scl 2; .type 32; .endef
midiOutClose:
    mov $66, %eax
    jmp forward_common

    .globl midiOutGetDevCapsA
    .def midiOutGetDevCapsA; .scl 2; .type 32; .endef
midiOutGetDevCapsA:
    mov $67, %eax
    jmp forward_common

    .globl midiOutGetDevCapsW
    .def midiOutGetDevCapsW; .scl 2; .type 32; .endef
midiOutGetDevCapsW:
    mov $68, %eax
    jmp forward_common

    .globl midiOutGetErrorTextA
    .def midiOutGetErrorTextA; .scl 2; .type 32; .endef
midiOutGetErrorTextA:
    mov $69, %eax
    jmp forward_common

    .globl midiOutGetErrorTextW
    .def midiOutGetErrorTextW; .scl 2; .type 32; .endef
midiOutGetErrorTextW:
    mov $70, %eax
    jmp forward_common

    .globl midiOutGetID
    .def midiOutGetID; .scl 2; .type 32; .endef
midiOutGetID:
    mov $71, %eax
    jmp forward_common

    .globl midiOutGetNumDevs
    .def midiOutGetNumDevs; .scl 2; .type 32; .endef
midiOutGetNumDevs:
    mov $72, %eax
    jmp forward_common

    .globl midiOutGetVolume
    .def midiOutGetVolume; .scl 2; .type 32; .endef
midiOutGetVolume:
    mov $73, %eax
    jmp forward_common

    .globl midiOutLongMsg
    .def midiOutLongMsg; .scl 2; .type 32; .endef
midiOutLongMsg:
    mov $74, %eax
    jmp forward_common

    .globl midiOutMessage
    .def midiOutMessage; .scl 2; .type 32; .endef
midiOutMessage:
    mov $75, %eax
    jmp forward_common

    .globl midiOutOpen
    .def midiOutOpen; .scl 2; .type 32; .endef
midiOutOpen:
    mov $76, %eax
    jmp forward_common

    .globl midiOutPrepareHeader
    .def midiOutPrepareHeader; .scl 2; .type 32; .endef
midiOutPrepareHeader:
    mov $77, %eax
    jmp forward_common

    .globl midiOutReset
    .def midiOutReset; .scl 2; .type 32; .endef
midiOutReset:
    mov $78, %eax
    jmp forward_common

    .globl midiOutSetVolume
    .def midiOutSetVolume; .scl 2; .type 32; .endef
midiOutSetVolume:
    mov $79, %eax
    jmp forward_common

    .globl midiOutShortMsg
    .def midiOutShortMsg; .scl 2; .type 32; .endef
midiOutShortMsg:
    mov $80, %eax
    jmp forward_common

    .globl midiOutUnprepareHeader
    .def midiOutUnprepareHeader; .scl 2; .type 32; .endef
midiOutUnprepareHeader:
    mov $81, %eax
    jmp forward_common

    .globl midiStreamClose
    .def midiStreamClose; .scl 2; .type 32; .endef
midiStreamClose:
    mov $82, %eax
    jmp forward_common

    .globl midiStreamOpen
    .def midiStreamOpen; .scl 2; .type 32; .endef
midiStreamOpen:
    mov $83, %eax
    jmp forward_common

    .globl midiStreamOut
    .def midiStreamOut; .scl 2; .type 32; .endef
midiStreamOut:
    mov $84, %eax
    jmp forward_common

    .globl midiStreamPause
    .def midiStreamPause; .scl 2; .type 32; .endef
midiStreamPause:
    mov $85, %eax
    jmp forward_common

    .globl midiStreamPosition
    .def midiStreamPosition; .scl 2; .type 32; .endef
midiStreamPosition:
    mov $86, %eax
    jmp forward_common

    .globl midiStreamProperty
    .def midiStreamProperty; .scl 2; .type 32; .endef
midiStreamProperty:
    mov $87, %eax
    jmp forward_common

    .globl midiStreamRestart
    .def midiStreamRestart; .scl 2; .type 32; .endef
midiStreamRestart:
    mov $88, %eax
    jmp forward_common

    .globl midiStreamStop
    .def midiStreamStop; .scl 2; .type 32; .endef
midiStreamStop:
    mov $89, %eax
    jmp forward_common

    .globl mixerClose
    .def mixerClose; .scl 2; .type 32; .endef
mixerClose:
    mov $90, %eax
    jmp forward_common

    .globl mixerGetControlDetailsA
    .def mixerGetControlDetailsA; .scl 2; .type 32; .endef
mixerGetControlDetailsA:
    mov $91, %eax
    jmp forward_common

    .globl mixerGetControlDetailsW
    .def mixerGetControlDetailsW; .scl 2; .type 32; .endef
mixerGetControlDetailsW:
    mov $92, %eax
    jmp forward_common

    .globl mixerGetDevCapsA
    .def mixerGetDevCapsA; .scl 2; .type 32; .endef
mixerGetDevCapsA:
    mov $93, %eax
    jmp forward_common

    .globl mixerGetDevCapsW
    .def mixerGetDevCapsW; .scl 2; .type 32; .endef
mixerGetDevCapsW:
    mov $94, %eax
    jmp forward_common

    .globl mixerGetID
    .def mixerGetID; .scl 2; .type 32; .endef
mixerGetID:
    mov $95, %eax
    jmp forward_common

    .globl mixerGetLineControlsA
    .def mixerGetLineControlsA; .scl 2; .type 32; .endef
mixerGetLineControlsA:
    mov $96, %eax
    jmp forward_common

    .globl mixerGetLineControlsW
    .def mixerGetLineControlsW; .scl 2; .type 32; .endef
mixerGetLineControlsW:
    mov $97, %eax
    jmp forward_common

    .globl mixerGetLineInfoA
    .def mixerGetLineInfoA; .scl 2; .type 32; .endef
mixerGetLineInfoA:
    mov $98, %eax
    jmp forward_common

    .globl mixerGetLineInfoW
    .def mixerGetLineInfoW; .scl 2; .type 32; .endef
mixerGetLineInfoW:
    mov $99, %eax
    jmp forward_common

    .globl mixerGetNumDevs
    .def mixerGetNumDevs; .scl 2; .type 32; .endef
mixerGetNumDevs:
    mov $100, %eax
    jmp forward_common

    .globl mixerMessage
    .def mixerMessage; .scl 2; .type 32; .endef
mixerMessage:
    mov $101, %eax
    jmp forward_common

    .globl mixerOpen
    .def mixerOpen; .scl 2; .type 32; .endef
mixerOpen:
    mov $102, %eax
    jmp forward_common

    .globl mixerSetControlDetails
    .def mixerSetControlDetails; .scl 2; .type 32; .endef
mixerSetControlDetails:
    mov $103, %eax
    jmp forward_common

    .globl mmDrvInstall
    .def mmDrvInstall; .scl 2; .type 32; .endef
mmDrvInstall:
    mov $104, %eax
    jmp forward_common

    .globl mmGetCurrentTask
    .def mmGetCurrentTask; .scl 2; .type 32; .endef
mmGetCurrentTask:
    mov $105, %eax
    jmp forward_common

    .globl mmTaskBlock
    .def mmTaskBlock; .scl 2; .type 32; .endef
mmTaskBlock:
    mov $106, %eax
    jmp forward_common

    .globl mmTaskCreate
    .def mmTaskCreate; .scl 2; .type 32; .endef
mmTaskCreate:
    mov $107, %eax
    jmp forward_common

    .globl mmTaskSignal
    .def mmTaskSignal; .scl 2; .type 32; .endef
mmTaskSignal:
    mov $108, %eax
    jmp forward_common

    .globl mmTaskYield
    .def mmTaskYield; .scl 2; .type 32; .endef
mmTaskYield:
    mov $109, %eax
    jmp forward_common

    .globl mmioAdvance
    .def mmioAdvance; .scl 2; .type 32; .endef
mmioAdvance:
    mov $110, %eax
    jmp forward_common

    .globl mmioAscend
    .def mmioAscend; .scl 2; .type 32; .endef
mmioAscend:
    mov $111, %eax
    jmp forward_common

    .globl mmioClose
    .def mmioClose; .scl 2; .type 32; .endef
mmioClose:
    mov $112, %eax
    jmp forward_common

    .globl mmioCreateChunk
    .def mmioCreateChunk; .scl 2; .type 32; .endef
mmioCreateChunk:
    mov $113, %eax
    jmp forward_common

    .globl mmioDescend
    .def mmioDescend; .scl 2; .type 32; .endef
mmioDescend:
    mov $114, %eax
    jmp forward_common

    .globl mmioFlush
    .def mmioFlush; .scl 2; .type 32; .endef
mmioFlush:
    mov $115, %eax
    jmp forward_common

    .globl mmioGetInfo
    .def mmioGetInfo; .scl 2; .type 32; .endef
mmioGetInfo:
    mov $116, %eax
    jmp forward_common

    .globl mmioInstallIOProcA
    .def mmioInstallIOProcA; .scl 2; .type 32; .endef
mmioInstallIOProcA:
    mov $117, %eax
    jmp forward_common

    .globl mmioInstallIOProcW
    .def mmioInstallIOProcW; .scl 2; .type 32; .endef
mmioInstallIOProcW:
    mov $118, %eax
    jmp forward_common

    .globl mmioOpenA
    .def mmioOpenA; .scl 2; .type 32; .endef
mmioOpenA:
    mov $119, %eax
    jmp forward_common

    .globl mmioOpenW
    .def mmioOpenW; .scl 2; .type 32; .endef
mmioOpenW:
    mov $120, %eax
    jmp forward_common

    .globl mmioRead
    .def mmioRead; .scl 2; .type 32; .endef
mmioRead:
    mov $121, %eax
    jmp forward_common

    .globl mmioRenameA
    .def mmioRenameA; .scl 2; .type 32; .endef
mmioRenameA:
    mov $122, %eax
    jmp forward_common

    .globl mmioRenameW
    .def mmioRenameW; .scl 2; .type 32; .endef
mmioRenameW:
    mov $123, %eax
    jmp forward_common

    .globl mmioSeek
    .def mmioSeek; .scl 2; .type 32; .endef
mmioSeek:
    mov $124, %eax
    jmp forward_common

    .globl mmioSendMessage
    .def mmioSendMessage; .scl 2; .type 32; .endef
mmioSendMessage:
    mov $125, %eax
    jmp forward_common

    .globl mmioSetBuffer
    .def mmioSetBuffer; .scl 2; .type 32; .endef
mmioSetBuffer:
    mov $126, %eax
    jmp forward_common

    .globl mmioSetInfo
    .def mmioSetInfo; .scl 2; .type 32; .endef
mmioSetInfo:
    mov $127, %eax
    jmp forward_common

    .globl mmioStringToFOURCCA
    .def mmioStringToFOURCCA; .scl 2; .type 32; .endef
mmioStringToFOURCCA:
    mov $128, %eax
    jmp forward_common

    .globl mmioStringToFOURCCW
    .def mmioStringToFOURCCW; .scl 2; .type 32; .endef
mmioStringToFOURCCW:
    mov $129, %eax
    jmp forward_common

    .globl mmioWrite
    .def mmioWrite; .scl 2; .type 32; .endef
mmioWrite:
    mov $130, %eax
    jmp forward_common

    .globl mmsystemGetVersion
    .def mmsystemGetVersion; .scl 2; .type 32; .endef
mmsystemGetVersion:
    mov $131, %eax
    jmp forward_common

    .globl sndPlaySoundA
    .def sndPlaySoundA; .scl 2; .type 32; .endef
sndPlaySoundA:
    mov $132, %eax
    jmp forward_common

    .globl sndPlaySoundW
    .def sndPlaySoundW; .scl 2; .type 32; .endef
sndPlaySoundW:
    mov $133, %eax
    jmp forward_common

    .globl timeBeginPeriod
    .def timeBeginPeriod; .scl 2; .type 32; .endef
timeBeginPeriod:
    mov $134, %eax
    jmp forward_common

    .globl timeEndPeriod
    .def timeEndPeriod; .scl 2; .type 32; .endef
timeEndPeriod:
    mov $135, %eax
    jmp forward_common

    .globl timeGetDevCaps
    .def timeGetDevCaps; .scl 2; .type 32; .endef
timeGetDevCaps:
    mov $136, %eax
    jmp forward_common

    .globl timeGetSystemTime
    .def timeGetSystemTime; .scl 2; .type 32; .endef
timeGetSystemTime:
    mov $137, %eax
    jmp forward_common

    .globl timeGetTime
    .def timeGetTime; .scl 2; .type 32; .endef
timeGetTime:
    mov $138, %eax
    jmp forward_common

    .globl timeKillEvent
    .def timeKillEvent; .scl 2; .type 32; .endef
timeKillEvent:
    mov $139, %eax
    jmp forward_common

    .globl timeSetEvent
    .def timeSetEvent; .scl 2; .type 32; .endef
timeSetEvent:
    mov $140, %eax
    jmp forward_common

    .globl waveInAddBuffer
    .def waveInAddBuffer; .scl 2; .type 32; .endef
waveInAddBuffer:
    mov $141, %eax
    jmp forward_common

    .globl waveInClose
    .def waveInClose; .scl 2; .type 32; .endef
waveInClose:
    mov $142, %eax
    jmp forward_common

    .globl waveInGetDevCapsA
    .def waveInGetDevCapsA; .scl 2; .type 32; .endef
waveInGetDevCapsA:
    mov $143, %eax
    jmp forward_common

    .globl waveInGetDevCapsW
    .def waveInGetDevCapsW; .scl 2; .type 32; .endef
waveInGetDevCapsW:
    mov $144, %eax
    jmp forward_common

    .globl waveInGetErrorTextA
    .def waveInGetErrorTextA; .scl 2; .type 32; .endef
waveInGetErrorTextA:
    mov $145, %eax
    jmp forward_common

    .globl waveInGetErrorTextW
    .def waveInGetErrorTextW; .scl 2; .type 32; .endef
waveInGetErrorTextW:
    mov $146, %eax
    jmp forward_common

    .globl waveInGetID
    .def waveInGetID; .scl 2; .type 32; .endef
waveInGetID:
    mov $147, %eax
    jmp forward_common

    .globl waveInGetNumDevs
    .def waveInGetNumDevs; .scl 2; .type 32; .endef
waveInGetNumDevs:
    mov $148, %eax
    jmp forward_common

    .globl waveInGetPosition
    .def waveInGetPosition; .scl 2; .type 32; .endef
waveInGetPosition:
    mov $149, %eax
    jmp forward_common

    .globl waveInMessage
    .def waveInMessage; .scl 2; .type 32; .endef
waveInMessage:
    mov $150, %eax
    jmp forward_common

    .globl waveInOpen
    .def waveInOpen; .scl 2; .type 32; .endef
waveInOpen:
    mov $151, %eax
    jmp forward_common

    .globl waveInPrepareHeader
    .def waveInPrepareHeader; .scl 2; .type 32; .endef
waveInPrepareHeader:
    mov $152, %eax
    jmp forward_common

    .globl waveInReset
    .def waveInReset; .scl 2; .type 32; .endef
waveInReset:
    mov $153, %eax
    jmp forward_common

    .globl waveInStart
    .def waveInStart; .scl 2; .type 32; .endef
waveInStart:
    mov $154, %eax
    jmp forward_common

    .globl waveInStop
    .def waveInStop; .scl 2; .type 32; .endef
waveInStop:
    mov $155, %eax
    jmp forward_common

    .globl waveInUnprepareHeader
    .def waveInUnprepareHeader; .scl 2; .type 32; .endef
waveInUnprepareHeader:
    mov $156, %eax
    jmp forward_common

    .globl waveOutBreakLoop
    .def waveOutBreakLoop; .scl 2; .type 32; .endef
waveOutBreakLoop:
    mov $157, %eax
    jmp forward_common

    .globl waveOutClose
    .def waveOutClose; .scl 2; .type 32; .endef
waveOutClose:
    mov $158, %eax
    jmp forward_common

    .globl waveOutGetDevCapsA
    .def waveOutGetDevCapsA; .scl 2; .type 32; .endef
waveOutGetDevCapsA:
    mov $159, %eax
    jmp forward_common

    .globl waveOutGetDevCapsW
    .def waveOutGetDevCapsW; .scl 2; .type 32; .endef
waveOutGetDevCapsW:
    mov $160, %eax
    jmp forward_common

    .globl waveOutGetErrorTextA
    .def waveOutGetErrorTextA; .scl 2; .type 32; .endef
waveOutGetErrorTextA:
    mov $161, %eax
    jmp forward_common

    .globl waveOutGetErrorTextW
    .def waveOutGetErrorTextW; .scl 2; .type 32; .endef
waveOutGetErrorTextW:
    mov $162, %eax
    jmp forward_common

    .globl waveOutGetID
    .def waveOutGetID; .scl 2; .type 32; .endef
waveOutGetID:
    mov $163, %eax
    jmp forward_common

    .globl waveOutGetNumDevs
    .def waveOutGetNumDevs; .scl 2; .type 32; .endef
waveOutGetNumDevs:
    mov $164, %eax
    jmp forward_common

    .globl waveOutGetPitch
    .def waveOutGetPitch; .scl 2; .type 32; .endef
waveOutGetPitch:
    mov $165, %eax
    jmp forward_common

    .globl waveOutGetPlaybackRate
    .def waveOutGetPlaybackRate; .scl 2; .type 32; .endef
waveOutGetPlaybackRate:
    mov $166, %eax
    jmp forward_common

    .globl waveOutGetPosition
    .def waveOutGetPosition; .scl 2; .type 32; .endef
waveOutGetPosition:
    mov $167, %eax
    jmp forward_common

    .globl waveOutGetVolume
    .def waveOutGetVolume; .scl 2; .type 32; .endef
waveOutGetVolume:
    mov $168, %eax
    jmp forward_common

    .globl waveOutMessage
    .def waveOutMessage; .scl 2; .type 32; .endef
waveOutMessage:
    mov $169, %eax
    jmp forward_common

    .globl waveOutOpen
    .def waveOutOpen; .scl 2; .type 32; .endef
waveOutOpen:
    mov $170, %eax
    jmp forward_common

    .globl waveOutPause
    .def waveOutPause; .scl 2; .type 32; .endef
waveOutPause:
    mov $171, %eax
    jmp forward_common

    .globl waveOutPrepareHeader
    .def waveOutPrepareHeader; .scl 2; .type 32; .endef
waveOutPrepareHeader:
    mov $172, %eax
    jmp forward_common

    .globl waveOutReset
    .def waveOutReset; .scl 2; .type 32; .endef
waveOutReset:
    mov $173, %eax
    jmp forward_common

    .globl waveOutRestart
    .def waveOutRestart; .scl 2; .type 32; .endef
waveOutRestart:
    mov $174, %eax
    jmp forward_common

    .globl waveOutSetPitch
    .def waveOutSetPitch; .scl 2; .type 32; .endef
waveOutSetPitch:
    mov $175, %eax
    jmp forward_common

    .globl waveOutSetPlaybackRate
    .def waveOutSetPlaybackRate; .scl 2; .type 32; .endef
waveOutSetPlaybackRate:
    mov $176, %eax
    jmp forward_common

    .globl waveOutSetVolume
    .def waveOutSetVolume; .scl 2; .type 32; .endef
waveOutSetVolume:
    mov $177, %eax
    jmp forward_common

    .globl waveOutUnprepareHeader
    .def waveOutUnprepareHeader; .scl 2; .type 32; .endef
waveOutUnprepareHeader:
    mov $178, %eax
    jmp forward_common

    .globl waveOutWrite
    .def waveOutWrite; .scl 2; .type 32; .endef
waveOutWrite:
    mov $179, %eax
    jmp forward_common
