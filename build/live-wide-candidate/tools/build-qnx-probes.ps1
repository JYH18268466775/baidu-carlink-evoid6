param(
    [Parameter(Mandatory = $true)]
    [string] $Toolchain
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$clang = Join-Path $Toolchain 'clang.exe'
$lld = Join-Path $Toolchain 'lld.exe'
$build = Join-Path $project 'build'
$work = Join-Path $build 'obj'
$null = New-Item -ItemType Directory -Force -Path $build, $work

function Invoke-Checked {
    param([string] $Program, [string[]] $Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program exited with code $LASTEXITCODE"
    }
}

Push-Location -LiteralPath $project
try {
    # The copied QNX libc is suitable for export checks, but its section
    # layout is not understood by LLD. This stub supplies link-time symbols
    # only. It is never copied to the head unit or linked into the executable.
    $stubHeader = @'
.syntax unified
.arm
.text
.macro symbol name
.global \name
.type \name, %function
\name:
    udf #0
.size \name, . - \name
.endm
'@
    $symbols = @(
        '_init_libc', 'exit', 'strcmp', 'strncmp', 'printf', 'fflush',
        'snprintf', 'getpid', '__get_errno_ptr', 'dlopen', 'dlsym',
        'dlclose', 'dlerror', 'usleep', 'name_open', 'name_close',
        'TimerTimeout', 'MsgSendvs_r', 'open', 'close', 'devctl',
        'read', 'write', 'alarm', 'memcmp', 'memcpy', 'memset', 'system',
        'pthread_create', 'pthread_join', 'getenv'
    )
    $stub = Join-Path $work 'libc-link-only.S'
    @($stubHeader; $symbols | ForEach-Object { "symbol $_" }) |
        Set-Content -LiteralPath $stub -Encoding Ascii
    $stubObject = Join-Path $work 'libc-link-only.o'
    $stubLibrary = Join-Path $work 'libc-link-only.so'
    $target = '--target=armv7-none-eabi'
    Invoke-Checked $clang @($target, '-c', $stub, '-o', $stubObject)
    Invoke-Checked $lld @('-flavor', 'gnu', '-m', 'armelf', '-shared',
        '--hash-style=sysv', '-soname', 'libc.so.3', '-o', $stubLibrary, $stubObject)

    $layout = Join-Path $work 'probe.ld'
    $legacyLayout = Get-Content -Raw -LiteralPath 'src/legacy/carlife_qnx_dyn_probe.ld'
    $legacyLayout = $legacyLayout.Replace('*(.text) }',
        '*(.text .text.*) *(.rodata .rodata.*) }')
    $legacyLayout.Replace('/DISCARD/ : { *(.note.GNU-stack) }',
        '/DISCARD/ : { *(.note.GNU-stack) *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }') |
        Set-Content -LiteralPath $layout -Encoding Ascii

    $cflags = @($target, '-marm', '-std=c11', '-ffreestanding', '-fno-unwind-tables',
        '-fno-asynchronous-unwind-tables', '-fno-stack-protector',
        '-Wall', '-Wextra', '-Werror', '-Os')
    $link = @('-flavor', 'gnu', '-m', 'armelf', '-z', 'max-page-size=4096',
        '--hash-style=sysv', '--dynamic-linker', '/usr/lib/ldqnx.so.2', '-T', $layout)

    $usbStart = Join-Path $work 'usb-start.o'
    $usbStatus = Join-Path $work 'usb-status.o'
    $usbOutput = Join-Path $build 'carlife_qnx_usb_status.elf'
    Invoke-Checked $clang @($target, '-DCARLINK_PROBE_ENTRY=carlink_usb_status',
        '-c', 'src/carlife_qnx_mmr_start.S', '-o', $usbStart)
    Invoke-Checked $clang ($cflags + @('-c', 'src/carlife_qnx_usb_status.c', '-o', $usbStatus))
    Invoke-Checked $lld ($link + @('-o', $usbOutput, $usbStart, $usbStatus, $stubLibrary))

    $captureStart = Join-Path $work 'usb-capture-start.o'
    $capture = Join-Path $work 'usb-capture.o'
    $captureOutput = Join-Path $build 'carlife_qnx_usb_capture.elf'
    Invoke-Checked $clang @($target, '-DCARLINK_PROBE_ENTRY=carlink_usb_capture',
        '-c', 'src/carlife_qnx_mmr_start.S', '-o', $captureStart)
    Invoke-Checked $clang ($cflags + @('-c', 'src/carlife_qnx_usb_capture.c', '-o', $capture))
    Invoke-Checked $lld ($link + @('-o', $captureOutput, $captureStart, $capture, $stubLibrary))

    $gop = Join-Path $work 'gop.S'
    @'
.section .rodata
.global carlink_gop_start, carlink_gop_end
carlink_gop_start:
.incbin "samples/carlife-mdev-first-gop.bin"
carlink_gop_end:
'@ | Set-Content -LiteralPath $gop -Encoding Ascii
    $mmrStart = Join-Path $work 'mmr-start.o'
    $mmr = Join-Path $work 'mmr-replay-output.o'
    $shortReplay = Join-Path $work 'short-replay.o'
    $mdev = Join-Path $work 'mdev.o'
    $gopObject = Join-Path $work 'gop.o'
    $replayOutput = Join-Path $build 'carlife_qnx_mmr_replay.elf'
    Invoke-Checked $clang @($target, '-c', 'src/carlife_qnx_mmr_start.S', '-o', $mmrStart)
    Invoke-Checked $clang ($cflags + @('-DCARLINK_ENABLE_SHORT_REPLAY',
        '-c', 'src/carlife_qnx_mmr_connect.c', '-o', $mmr))
    Invoke-Checked $clang ($cflags + @('-c', 'src/carlife_qnx_short_replay.c', '-o', $shortReplay))
    Invoke-Checked $clang @($target, '-c', 'src/legacy/carlife_qnx_mdev_arm.S', '-o', $mdev)
    Invoke-Checked $clang @($target, '-c', $gop, '-o', $gopObject)
    Invoke-Checked $lld ($link + @('--allow-shlib-undefined', '-o', $replayOutput,
        $mmrStart, $mmr, $shortReplay, $mdev, $gopObject,
        $stubLibrary, 'references/qnx/libscreen.so.1'))

    Get-FileHash -Algorithm SHA256 -LiteralPath $usbOutput, $captureOutput, $replayOutput
}
finally {
    Pop-Location
}
