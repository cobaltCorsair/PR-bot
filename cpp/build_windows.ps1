[CmdletBinding()]
param(
    [string]$QtRoot = $env:QT_ROOT_DIR
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $IsWindows) {
    throw 'Run this script in PowerShell 7 on Windows.'
}
if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    throw 'Set QT_ROOT_DIR or pass -QtRoot with the path to Qt MSVC 2022 x64.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$qtDirectory = (Resolve-Path -LiteralPath $QtRoot).Path
$qtBin = Join-Path $qtDirectory 'bin'
$buildDirectory = Join-Path $PSScriptRoot 'build-windows'
$packageDirectory = Join-Path $PSScriptRoot 'dist/PR-Bot-windows-x64'
$templates = Join-Path $PSScriptRoot 'packaging/windows'
$deployTool = Join-Path $qtBin 'windeployqt.exe'

if (-not (Test-Path -LiteralPath $deployTool -PathType Leaf)) {
    throw "Qt deployment tool was not found: $deployTool"
}

# PowerShell does not throw for a failed native command on every version.
# Stop immediately so a failed build cannot publish a package from an old run.
function Invoke-BuildCommand {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

Invoke-BuildCommand -Program 'cmake' -Arguments @(
    '-S', $PSScriptRoot, '-B', $buildDirectory,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$qtDirectory", '-DBUILD_TESTING=OFF'
)
Invoke-BuildCommand -Program 'cmake' -Arguments @(
    '--build', $buildDirectory, '--config', 'Release', '--target', 'prbot', '--parallel', '4'
)

# Only this generated directory is cleaned. User settings from the repository
# are never copied: the distributable starts with the dedicated empty template.
if (Test-Path -LiteralPath $packageDirectory) {
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDirectory 'Release/prbot.exe') -Destination $packageDirectory

# Use the official Microsoft runtime installer below instead of copying
# compiler DLLs from the developer machine into the distributable.
Invoke-BuildCommand -Program $deployTool -Arguments @(
    '--release', '--no-translations', '--no-compiler-runtime',
    (Join-Path $packageDirectory 'prbot.exe')
)

# Explicitly include Windows' native TLS backend for the driver updater's
# HTTPS requests; the application then needs no separate OpenSSL installation.
$tlsDirectory = Join-Path $packageDirectory 'tls'
New-Item -ItemType Directory -Path $tlsDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $qtDirectory 'plugins/tls/qschannelbackend.dll') -Destination $tlsDirectory

Copy-Item -Path (Join-Path $templates '*') -Destination $packageDirectory -Recurse
$iconDirectory = Join-Path $packageDirectory 'src'
New-Item -ItemType Directory -Path $iconDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'src/icon.png') -Destination $iconDirectory

$runtimePath = Join-Path $packageDirectory 'vc_redist.x64.exe'
Invoke-WebRequest -Uri 'https://aka.ms/vc14/vc_redist.x64.exe' -OutFile $runtimePath
$signature = Get-AuthenticodeSignature -LiteralPath $runtimePath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)') {
    throw 'The downloaded Visual C++ Redistributable has no valid Microsoft signature.'
}

# Keep Qt notices and the license texts with the dynamically linked libraries.
$licenseDirectory = Join-Path $packageDirectory 'licenses'
New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
$qtVersion = (& (Join-Path $qtBin 'qmake.exe') -query QT_VERSION).Trim()
if ($LASTEXITCODE -ne 0 -or $qtVersion -notmatch '^6\.\d+\.\d+$') {
    throw 'Could not determine the Qt version for the package notices.'
}
foreach ($license in @('LGPL-3.0-only.txt', 'GPL-3.0-only.txt')) {
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/qt/qtbase/v$qtVersion/LICENSES/$license" `
        -OutFile (Join-Path $licenseDirectory $license)
}
# Qt's own deployment may include notices for bundled third-party components.
foreach ($directory in @('licenses', 'LICENSES')) {
    $source = Join-Path $qtDirectory $directory
    if (Test-Path -LiteralPath $source -PathType Container) {
        Copy-Item -Path (Join-Path $source '*') -Destination $licenseDirectory -Recurse -Force
        break
    }
}
@"
PR-Bot uses the dynamically linked Qt $qtVersion libraries.
Copyright (C) The Qt Company Ltd. and other Qt contributors.
Qt Core, Gui, Widgets and Network are used under GNU LGPL version 3.
The LGPL and GPL license texts are provided in this directory.
Qt source code and third-party notices:
https://code.qt.io/cgit/qt/qtbase.git/tree/?h=v$qtVersion
https://doc.qt.io/qt-$($qtVersion.Split('.')[0..1] -join '.')/licenses-used-in-qt.html
The Qt DLLs can be replaced with a compatible build.
Microsoft Visual C++ Redistributable is provided as its unmodified official installer.
"@ | Set-Content -LiteralPath (Join-Path $licenseDirectory 'THIRD_PARTY_NOTICES.txt') -Encoding utf8

# These are packaging requirements, not application/browser test runs.
foreach ($requiredFile in @(
    'prbot.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
    'platforms/qwindows.dll', 'tls/qschannelbackend.dll',
    'settings.json', 'forums.txt', 'README.txt', 'vc_redist.x64.exe'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageDirectory $requiredFile) -PathType Leaf)) {
        throw "The Windows package is missing $requiredFile."
    }
}

$revision = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { 'local build' }
@"
PR-Bot Windows x64
Commit: $revision
Qt: $qtVersion (MSVC 2022 x64)
Built UTC: $([DateTime]::UtcNow.ToString('o'))
"@ | Set-Content -LiteralPath (Join-Path $packageDirectory 'build-info.txt') -Encoding utf8

Write-Host "Windows package prepared: $packageDirectory"
