<#
    Flashes a built image. PowerShell counterpart of flash-win.bat, using
    scripts\idf-env.ps1 so that it also works on an EIM installation.

      scripts\flash-win.ps1 COM5 esp32s3 s3pad
      scripts\flash-win.ps1 COM7 esp32s3 s3input
      scripts\flash-win.ps1 COM6 esp32c3
      scripts\flash-win.ps1 COM7 esp32s3 s3input -Uart

    -Uart is for flashing through a plain USB-UART adapter (CP2102 and friends) wired to
    UART0. Such an adapter usually has DTR/RTS going nowhere, and on an ESP32-S3 SuperMini
    neither EN nor GPIO0 is brought out to the 18-pin header anyway, so esptool cannot put
    the chip into download mode or reset it afterwards. The switch tells it not to try:
    hold BOOT, tap RESET, run this, then tap RESET again to leave the bootloader.

    Without the switch esptool uses its normal sequence, which is what you want when
    flashing over the board's own USB-C - there the ROM's USB Serial/JTAG handles reset.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Port    = 'COM6',
    [Parameter(Position = 1)][string]$Target  = 'esp32c3',
    [Parameter(Position = 2)][string]$Variant = '',
    [int]$Baud = 921600,
    [switch]$Uart
)

$ErrorActionPreference = 'Stop'

$firmware = (Resolve-Path (Join-Path $PSScriptRoot '..\firmware')).Path
$suffix   = if ($Variant) { ".$Variant" } else { '' }

# Two build directories can coexist: build.<target> from WSL and build.win.<target> from a
# native Windows build. Picking one by a fixed preference silently flashes a stale image
# whenever the other is newer, which cost real debugging time once - so flash the NEWER one.
# @( ) is load-bearing: with exactly one build present the pipeline returns a bare string,
# and $candidates[0] would then index into it and yield "F" - the first character of the path.
$candidates = @(
    @("build.$Target$suffix", "build.win.$Target$suffix") |
        ForEach-Object { Join-Path $firmware $_ } |
        Where-Object { Test-Path (Join-Path $_ 'flash_args') } |
        Sort-Object { (Get-Item (Join-Path $_ 'flash_args')).LastWriteTime } -Descending
)

if ($candidates.Count -eq 0) {
    throw "no build found for target $Target$suffix - run scripts\build-native-win.ps1 $Target $Variant first"
}
$buildDir = $candidates[0]
if ($candidates.Count -gt 1) {
    Write-Host "== two builds present, flashing the newer one: $(Split-Path $buildDir -Leaf)"
}

. (Join-Path $PSScriptRoot 'idf-env.ps1')

# esptool v5 (shipped with IDF 6.x) renamed the subcommands to kebab-case.
$esptoolArgs = @('--chip', $Target, '--port', $Port, '--baud', $Baud)
if ($Uart) { $esptoolArgs += @('--before', 'no-reset', '--after', 'no-reset') }
$esptoolArgs += @('write-flash', '@flash_args')

Push-Location $buildDir
try {
    Write-Host "== python -m esptool $($esptoolArgs -join ' ')"
    python -m esptool @esptoolArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
