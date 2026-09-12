<#
    Activates an ESP-IDF environment in the CURRENT PowerShell session.

    Dot-source it, do not run it:   . scripts\idf-env.ps1

    WHY THIS EXISTS. ESP-IDF now ships two installers whose layouts are INCOMPATIBLE, and
    the .bat scripts in this directory only understand one of them.

      install.bat        puts the Python virtual environment in
                         %IDF_TOOLS_PATH%\python_env\idf<x.y>_py<a.b>_env
                         which is the ONLY place export.bat looks.

      EIM (the newer      puts it in  %IDF_TOOLS_PATH%\python\<x.y>\venv
      ESP-IDF Installa-   and ships its own PowerShell activation script instead of
      tion Manager)       relying on export.bat.

    So on an EIM install export.bat aborts with

        ERROR: ESP-IDF Python virtual environment
        "C:\Espressif\tools\python_env\idf6.1_py3.13_env\Scripts\python.exe" not found.

    and every .bat here fails right after it with "'idf.py' is not recognized" - measured on
    a v6.1 EIM install with IDF_TOOLS_PATH=C:\Espressif\tools. Nothing is actually missing;
    the venv is simply somewhere else. Rather than duplicating a second copy of it just to
    satisfy export.bat, this helper uses whichever activation the installer provided.

    Resolution order, first hit wins:
      1. $env:IDF_ACTIVATE   - explicit path to an activation script, overrides everything
      2. EIM's eim_idf.json  - one entry per installed version, each naming its own script
      3. export.ps1 under $env:IDF_WIN (or the default path) - the install.bat layout

    Set $env:IDF_WIN to pick a specific version when several are installed; it is matched
    against the "path" field of the EIM entries and used directly for the export.ps1 route.
#>

if (Get-Command idf.py -ErrorAction SilentlyContinue) {
    # Already active - do not stack a second activation onto PATH.
    return
}

$activate = $null

if ($env:IDF_ACTIVATE) {
    if (-not (Test-Path $env:IDF_ACTIVATE)) {
        throw "IDF_ACTIVATE points at $($env:IDF_ACTIVATE), which does not exist"
    }
    $activate = $env:IDF_ACTIVATE
}

if (-not $activate) {
    # EIM records every installation in this file, together with the activation script it
    # generated for it. Look where it is normally written; IDF_TOOLS_PATH first, because a
    # user who set that variable meant it.
    $registries = @()
    if ($env:IDF_TOOLS_PATH) { $registries += (Join-Path $env:IDF_TOOLS_PATH 'eim_idf.json') }
    $registries += 'C:\Espressif\tools\eim_idf.json'
    $registries += (Join-Path $env:USERPROFILE '.espressif\tools\eim_idf.json')

    foreach ($reg in $registries) {
        if (-not (Test-Path $reg)) { continue }

        $eim = Get-Content $reg -Raw | ConvertFrom-Json
        $installs = @($eim.idfInstalled)
        if ($installs.Count -eq 0) { continue }

        $pick = $null
        if ($env:IDF_WIN) {
            $pick = $installs | Where-Object { $_.path -eq $env:IDF_WIN } | Select-Object -First 1
            if (-not $pick) {
                Write-Warning "IDF_WIN=$($env:IDF_WIN) is not in $reg - falling back to the selected install"
            }
        }
        if (-not $pick -and $eim.idfSelectedId) {
            $pick = $installs | Where-Object { $_.id -eq $eim.idfSelectedId } | Select-Object -First 1
        }
        if (-not $pick) { $pick = $installs[0] }

        if ($pick.activationScript -and (Test-Path $pick.activationScript)) {
            $activate = $pick.activationScript
            break
        }
    }
}

if (-not $activate) {
    # Classic install.bat layout. Keep the same default as the .bat scripts so that both
    # routes disagree about nothing.
    $idfDir = $env:IDF_WIN
    if (-not $idfDir) { $idfDir = Join-Path $env:USERPROFILE 'esp\v5.5.1\esp-idf' }

    $exportPs1 = Join-Path $idfDir 'export.ps1'
    if (-not (Test-Path $exportPs1)) {
        throw @"
No ESP-IDF environment found. Tried, in order:
  IDF_ACTIVATE      : $($env:IDF_ACTIVATE)
  EIM registry      : $($registries -join ', ')
  export.ps1        : $exportPs1

Point one of them at your installation, e.g.
  `$env:IDF_WIN = 'C:\esp\v6.1\esp-idf'
  `$env:IDF_ACTIVATE = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
"@
    }
    $activate = $exportPs1
}

. $activate

# idf.py refuses to print non-ASCII on a console that is not UTF-8 and spends a paragraph
# telling you so on every invocation. Setting this is the fix it recommends.
if (-not $env:PYTHONUTF8) { $env:PYTHONUTF8 = '1' }

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "activation script $activate ran but idf.py is still not on PATH"
}
