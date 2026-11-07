# =============================================================================
# deploy.ps1 - builds QuickReports and assembles the portable deployment package.
#
# Usage:  powershell -ExecutionPolicy Bypass -File deploy.ps1 [-Config Release]
#                  [-QtPrefixPath C:/Qt/6.10.2/msvc2022_64] [-SkipConfigure]
# ProjectDir/BuildDir/DeploymentDir default to folders next to the script;
# Config picks the CMake configuration; SkipConfigure reuses the existing CMake cache.
# =============================================================================
[CmdletBinding()]
param(
    [string]$ProjectDir = $PSScriptRoot,
    [string]$BuildDir = (Join-Path $PSScriptRoot "build"),
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [string]$DeploymentDir = (Join-Path $PSScriptRoot "Deployment"),
    [string]$QtPrefixPath = "C:/Qt/6.10.2/msvc2022_64",
    [switch]$SkipConfigure
)

$ErrorActionPreference = "Stop"

# Finds cmake.exe: PATH first, then the copy bundled with Visual Studio.
function Resolve-CMake {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) { return $cmake.Source }

    $vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsPath) { return $vsPath }

    throw "Could not find 'cmake'. Install CMake or add it to PATH."
}

# Finds windeployqt.exe (Qt tool that copies the Qt DLLs/plugins an app needs):
# first inside the given Qt install, then on PATH.
function Resolve-WindeployQt {
    param([string]$QtRoot)

    $candidates = @()
    if ($QtRoot) {
        $candidates += (Join-Path $QtRoot "bin\windeployqt.exe")
    }

    $cmd = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($cmd) {
        $candidates += $cmd.Source
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    throw "Could not find 'windeployqt.exe'. Set -QtPrefixPath or add windeployqt to PATH."
}

# Copies the Visual C++ runtime DLLs (VCRUNTIME140, MSVCP140, ...) next to the app
# so it runs on PCs without the redistributable. Source order: VCToolsRedistDir,
# vswhere-located Visual Studio, a scan of VS folders, then System32 as a last resort.
function Copy-MsvcRuntime {
    param(
        [string]$DestinationDir
    )

    $toolsRoot = $env:VCToolsRedistDir
    $crtSource = $null

    if ($toolsRoot) {
        $candidate = Join-Path $toolsRoot "x64\Microsoft.VC143.CRT"
        if (Test-Path $candidate) {
            $crtSource = $candidate
        }
    }

    if (-not $crtSource) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -eq 0 -and $installPath) {
                $redistRoot = Join-Path $installPath "VC\Redist\MSVC"
                if (Test-Path $redistRoot) {
                    $latest = Get-ChildItem -Path $redistRoot -Directory |
                        Sort-Object Name -Descending |
                        Select-Object -First 1
                    if ($latest) {
                        $candidate = Join-Path $latest.FullName "x64\Microsoft.VC143.CRT"
                        if (Test-Path $candidate) {
                            $crtSource = $candidate
                        }
                    }
                }
            }
        }
    }

    if (-not $crtSource) {
        $vsRoots = @(
            "$env:ProgramFiles\Microsoft Visual Studio",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
        )

        foreach ($root in $vsRoots) {
            if (-not (Test-Path $root)) { continue }
            $found = Get-ChildItem -Path $root -Recurse -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -like "*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" } |
                Sort-Object FullName -Descending |
                Select-Object -First 1
            if ($found) {
                $crtSource = $found.FullName
                break
            }
        }
    }

    if ($crtSource) {
        Write-Host "Copying MSVC runtime from: $crtSource"
        Copy-Item -Path (Join-Path $crtSource "*.dll") -Destination $DestinationDir -Force
        return
    }

    # Last fallback: use already-installed runtime from System32 on build machine.
    $system32 = Join-Path $env:WINDIR "System32"
    $fallbackPatterns = @(
        "VCRUNTIME140*.dll",
        "MSVCP140*.dll",
        "CONCRT140.dll"
    )
    $copied = 0
    foreach ($pattern in $fallbackPatterns) {
        $matches = Get-ChildItem -Path $system32 -Filter $pattern -ErrorAction SilentlyContinue
        foreach ($m in $matches) {
            Copy-Item -Path $m.FullName -Destination $DestinationDir -Force
            $copied++
        }
    }

    if ($copied -gt 0) {
        Write-Warning "Used System32 fallback for MSVC runtime ($copied files). Prefer VC redist folder when available."
        return
    }

    Write-Warning "Could not locate MSVC runtime DLLs. Install VC++ build tools or ship vc_redist.x64.exe."
}

# Locates vc_redist.x64.exe (same search order as above); returns $null if not found.
# It is shipped as a fallback installer for target machines.
function Find-VcRedistInstaller {
    $toolsRoot = $env:VCToolsRedistDir
    if ($toolsRoot) {
        $candidate = Join-Path $toolsRoot "x64\vc_redist.x64.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $redistRoot = Join-Path $installPath "VC\Redist\MSVC"
            if (Test-Path $redistRoot) {
                $latest = Get-ChildItem -Path $redistRoot -Directory |
                    Sort-Object Name -Descending |
                    Select-Object -First 1
                if ($latest) {
                    $candidate = Join-Path $latest.FullName "vc_redist.x64.exe"
                    if (Test-Path $candidate) {
                        return $candidate
                    }
                }
            }
        }
    }

    $vsRoots = @(
        "$env:ProgramFiles\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    )

    foreach ($root in $vsRoots) {
        if (-not (Test-Path $root)) { continue }
        $found = Get-ChildItem -Path $root -Recurse -File -Filter "vc_redist.x64.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }

    return $null
}

# Writes qt.conf so Qt looks for plugins next to the exe (portable layout).
function Write-QtConf {
    param(
        [string]$DistDir
    )

    $qtConfPath = Join-Path $DistDir "qt.conf"
    @"
[Paths]
Plugins=.
Translations=translations
"@ | Set-Content -Path $qtConfPath -Encoding ASCII
}

# Sanity check: throws if any file required to run the portable package is missing.
function Test-PortableRuntime {
    param(
        [string]$DistDir
    )

    $required = @(
        "QuickReports.exe",
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Widgets.dll",
        "platforms\qwindows.dll",
        "VCRUNTIME140.dll",
        "VCRUNTIME140_1.dll",
        "MSVCP140.dll",
        "MSVCP140_1.dll"
    )

    $missing = @()
    foreach ($item in $required) {
        $p = Join-Path $DistDir $item
        if (-not (Test-Path $p)) {
            $missing += $item
        }
    }

    if ($missing.Count -gt 0) {
        throw "Portable package verification failed. Missing: $($missing -join ', ')"
    }
}

# Kills running QuickReports/QuickReportsLauncher processes that lock files in the
# deployment folder. Returns $true if something was stopped.
function Stop-QuickReportsProcesses {
    param(
        [string]$ProjectRoot
    )

    $names = @("QuickReports", "QuickReportsLauncher")
    $stoppedAny = $false
    foreach ($name in $names) {
        $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
        foreach ($p in $procs) {
            $path = $null
            try { $path = $p.Path } catch { $path = $null }

            if ($path -and $path.StartsWith($ProjectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                Write-Host "Stopping running process: $($p.ProcessName) (PID $($p.Id))"
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                $stoppedAny = $true
                continue
            }

            # Path can be unreadable for elevated/system-owned processes; still stop by name.
            Write-Host "Stopping running process by name: $($p.ProcessName) (PID $($p.Id))"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $stoppedAny = $true
        }
    }
    return $stoppedAny
}

# Deletes the old deployment folder with retries (clears read-only flags, stops
# locking processes between attempts).
function Remove-DeploymentDirectorySafe {
    param(
        [string]$DeploymentPath,
        [string]$ProjectRoot
    )

    if (-not (Test-Path $DeploymentPath)) {
        return
    }

    Write-Host "Removing old deployment directory..."
    $maxAttempts = 3
    for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
        try {
            # Clear readonly flags if any copied files carried them.
            Get-ChildItem -Path $DeploymentPath -Recurse -Force -ErrorAction SilentlyContinue |
                ForEach-Object { $_.Attributes = ($_.Attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly)) }

            Remove-Item -Path $DeploymentPath -Recurse -Force -ErrorAction Stop
            return
        } catch {
            $stopped = Stop-QuickReportsProcesses -ProjectRoot $ProjectRoot
            Start-Sleep -Milliseconds 500
            if ($attempt -eq $maxAttempts) {
                throw "Failed to remove deployment directory after $maxAttempts attempts. Close QuickReports/Explorer windows in '$DeploymentPath' and retry. Last error: $($_.Exception.Message)"
            }
            if ($stopped) {
                Write-Warning "Retrying cleanup after stopping running QuickReports processes..."
            } else {
                Write-Warning "Retrying cleanup; a file is still locked..."
            }
        }
    }
}

# ---- Main flow ----
# 1 clean old Deployment  2 configure + build both targets  3 windeployqt + CRT
# 4 copy runtime into QuickReportsDist  5 qt.conf, vc_redist  6 launcher as root exe
# 7 verify. Result: Deployment\QuickReports.exe + Deployment\QuickReportsDist
Write-Host "ProjectDir    : $ProjectDir"
Write-Host "BuildDir      : $BuildDir"
Write-Host "Config        : $Config"
Write-Host "DeploymentDir : $DeploymentDir"

# Resolve tools and output locations.
$cmakeExe = Resolve-CMake
$windeployqtExe = Resolve-WindeployQt -QtRoot $QtPrefixPath

$appTarget = "QuickReports"
$launcherTarget = "QuickReportsLauncher"
$appExeName = "$appTarget.exe"
$launcherExeName = "$launcherTarget.exe"

$configBuildDir = Join-Path $BuildDir $Config
$appExe = Join-Path $configBuildDir $appExeName
$launcherExe = Join-Path $configBuildDir $launcherExeName

# Clean the previous deployment; if it stays locked, fall back to a time-stamped folder.
$effectiveDeploymentDir = $DeploymentDir
try {
    Remove-DeploymentDirectorySafe -DeploymentPath $effectiveDeploymentDir -ProjectRoot $ProjectDir
} catch {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $effectiveDeploymentDir = Join-Path $ProjectDir ("Deployment_" + $stamp)
    Write-Warning "Default deployment folder is locked. Using fallback deployment folder: $effectiveDeploymentDir"
}

$distDir = Join-Path $effectiveDeploymentDir "QuickReportsDist"
$rootExe = Join-Path $effectiveDeploymentDir "QuickReports.exe"

New-Item -ItemType Directory -Path $distDir -Force | Out-Null

# Configure (unless -SkipConfigure and a cache already exists), then build both targets.
$cachePath = Join-Path $BuildDir "CMakeCache.txt"
if (-not $SkipConfigure -or -not (Test-Path $cachePath)) {
    Write-Host "Configuring CMake project..."
    & $cmakeExe -S $ProjectDir -B $BuildDir "-DCMAKE_PREFIX_PATH=$QtPrefixPath"
}

Write-Host "Building targets ($Config)..."
& $cmakeExe --build $BuildDir --config $Config --target $appTarget --target $launcherTarget

if (-not (Test-Path $appExe)) {
    throw "App executable not found: $appExe"
}
if (-not (Test-Path $launcherExe)) {
    throw "Launcher executable not found: $launcherExe"
}

# Collect the Qt runtime next to the built exe.
Write-Host "Running windeployqt for app runtime..."
# windeployqt writes benign warnings (e.g. missing optional dxcompiler.dll) to stderr;
# under $ErrorActionPreference = "Stop" those get promoted to terminating errors, so
# relax it for this one native call and check the real exit code instead.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $windeployqtExe --verbose 0 --compiler-runtime $appExe
$ErrorActionPreference = $prevEap
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

# windeployqt can miss VC runtime files on some setups; enforce CRT copy.
Copy-MsvcRuntime -DestinationDir $configBuildDir

Write-Host "Copying app runtime into QuickReportsDist..."
Copy-Item -Path (Join-Path $configBuildDir "*") -Destination $distDir -Recurse -Force

# Keep distribution clean: launcher belongs only at root.
$launcherInDist = Join-Path $distDir $launcherExeName
if (Test-Path $launcherInDist) {
    Remove-Item -Path $launcherInDist -Force
}

Write-Host "Writing qt.conf for portable plugin lookup..."
Write-QtConf -DistDir $distDir

Write-Host "Copying VC++ redistributable installer (fallback)..."
$vcRedist = Find-VcRedistInstaller
if ($vcRedist) {
    Copy-Item -Path $vcRedist -Destination (Join-Path $distDir "vc_redist.x64.exe") -Force
} else {
    Write-Warning "vc_redist.x64.exe not found in Visual Studio redist folders. Skipping."
}

Write-Host "Copying launcher to deployment root as QuickReports.exe..."
Copy-Item -Path $launcherExe -Destination $rootExe -Force

Write-Host "Verifying portable runtime files..."
Test-PortableRuntime -DistDir $distDir

Write-Host "Deployment complete."
Write-Host "Root EXE      : $rootExe"
Write-Host "Dist folder   : $distDir"
