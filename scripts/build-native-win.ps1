<#
    Builds on Windows from PowerShell. Same conventions as build-native-win.bat - separate
    build directory and sdkconfig per target and per variant - but it gets its ESP-IDF
    environment through scripts\idf-env.ps1, which also understands EIM installations. See
    the comment at the top of that file for why the .bat route cannot.

      scripts\build-native-win.ps1 esp32s3 s3input
      scripts\build-native-win.ps1 esp32s3 s3pad
      scripts\build-native-win.ps1 esp32s3 s3pad menuconfig
      scripts\build-native-win.ps1 esp32c3                    # no variant
      scripts\build-native-win.ps1 esp32s3 s3pad fullclean

    A VARIANT is the name of an existing firmware\sdkconfig.defaults.<name>. It exists
    because the USB bridge needs two DIFFERENT roles on the SAME target: one S3 is the pad,
    the other is the input host.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Target  = 'esp32c3',
    [Parameter(Position = 1)][string]$Variant = '',
    [Parameter(Position = 2)][string]$Action  = 'build'
)

$ErrorActionPreference = 'Stop'

$firmware = (Resolve-Path (Join-Path $PSScriptRoot '..\firmware')).Path

# Allow "esp32s3 menuconfig" - if the second argument is not a defaults file, it is an action.
if ($Variant -and -not (Test-Path (Join-Path $firmware "sdkconfig.defaults.$Variant"))) {
    if ($Action -ne 'build') {
        throw "no firmware\sdkconfig.defaults.$Variant - variants available: " +
              ((Get-ChildItem $firmware -Filter 'sdkconfig.defaults.*' |
                ForEach-Object { $_.Name -replace '^sdkconfig\.defaults\.', '' }) -join ', ')
    }
    $Action = $Variant
    $Variant = ''
}

. (Join-Path $PSScriptRoot 'idf-env.ps1')

$suffix    = if ($Variant) { ".$Variant" } else { '' }
$buildDir  = "build.win.$Target$suffix"
$sdkconfig = "sdkconfig.win.$Target$suffix"

$defaults = @('sdkconfig.defaults', "sdkconfig.defaults.$Target")
if ($Variant) { $defaults += "sdkconfig.defaults.$Variant" }
# sdkconfig.local is the gitignored place for machine-local overrides and one-off
# diagnostics, mirroring scripts/build.sh.
if (Test-Path (Join-Path $firmware 'sdkconfig.local')) {
    $defaults += 'sdkconfig.local'
    Write-Host '== using overrides from sdkconfig.local'
}
$defaultsArg = $defaults -join ';'

if ($Variant) { Write-Host "== variant $Variant" }
Write-Host "== target $Target, build dir $buildDir"

Push-Location $firmware
try {
    # The first run has to set the target: that is also what creates the sdkconfig from the
    # defaults chain.
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Write-Host "== first build: setting target $Target"
        idf.py -B $buildDir -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaultsArg" `
               set-target $Target
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    # Both -D arguments are repeated on EVERY build, not just the first, and that is
    # load-bearing. Without them a plain rebuild leaves an existing per-variant sdkconfig
    # untouched, so a Kconfig option added after that file was created never appears in it -
    # the build then silently compiles WITHOUT the new feature while reporting success.
    # Measured: adding APP_USB_PASSTHROUGH produced a byte-identical s3input image until the
    # sdkconfig was deleted by hand. Values already present in sdkconfig still win, so this
    # does not undo anything set through menuconfig.
    idf.py -B $buildDir -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaultsArg" $Action
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
