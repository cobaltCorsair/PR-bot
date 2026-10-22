[CmdletBinding()]
param([string]$QtRoot = $env:QT_ROOT_DIR)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows -or [string]::IsNullOrWhiteSpace($QtRoot)) {
    throw 'Use PowerShell 7 on Windows with QT_ROOT_DIR set to Qt MSVC 2022 x64.'
}
$buildDirectory = Join-Path $PSScriptRoot 'build-windows'
$toolsDirectory = Join-Path $buildDirectory 'tools'
New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null
$chrome = @(
    (Join-Path $env:ProgramFiles 'Google/Chrome/Application/chrome.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Google/Chrome/Application/chrome.exe')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $chrome) { throw 'Google Chrome is required for the local browser fixtures.' }
$version = (Get-Item -LiteralPath $chrome).VersionInfo.ProductVersion
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)\.\d+') { throw "Unrecognized Chrome version: $version" }
$build = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
$major = $Matches[1]
# Follow Google's build/milestone selection for the browser already on the runner.
# https://developer.chrome.com/docs/chromedriver/downloads/version-selection
$metadata = Invoke-RestMethod 'https://googlechromelabs.github.io/chrome-for-testing/latest-patch-versions-per-build-with-downloads.json'
$property = $metadata.builds.PSObject.Properties[$build]
$release = if ($property) { $property.Value } else { $null }
if (-not $release) {
    $metadata = Invoke-RestMethod 'https://googlechromelabs.github.io/chrome-for-testing/latest-versions-per-milestone-with-downloads.json'
    $property = $metadata.milestones.PSObject.Properties[$major]
    if (-not $property) { throw "No ChromeDriver available for Chrome $version" }
    $release = $property.Value
}
$download = $release.downloads.chromedriver | Where-Object platform -EQ 'win64' | Select-Object -First 1
$uri = [Uri]$download.url
if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'storage.googleapis.com' -or
    -not $uri.AbsolutePath.StartsWith('/chrome-for-testing-public/')) {
    throw 'Unexpected ChromeDriver download URL.'
}
$archive = Join-Path $toolsDirectory 'chromedriver.zip'
Invoke-WebRequest -Uri $uri -OutFile $archive
Expand-Archive -LiteralPath $archive -DestinationPath $toolsDirectory -Force
$driver = Join-Path $toolsDirectory 'chromedriver-win64/chromedriver.exe'
function Invoke-TestCommand {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Invoke-TestCommand -Program $driver -Arguments @('--version')
Write-Host "Chrome $version; ChromeDriver $($release.version)"
$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
Invoke-TestCommand -Program 'cmake' -Arguments @(
    '-S', $PSScriptRoot, '-B', $buildDirectory, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$QtRoot", '-DBUILD_TESTING=ON', '-DPRBOT_BROWSER_TESTS=ON',
    "-DPRBOT_TEST_CHROMEDRIVER=$driver"
)
Invoke-TestCommand -Program 'cmake' -Arguments @('--build', $buildDirectory, '--config', 'Release', '--parallel', '4')
# All browser tests use an ephemeral local HTTP server. No real forum is visited.
Invoke-TestCommand -Program 'ctest' -Arguments @(
    '--test-dir', $buildDirectory, '-C', 'Release', '--output-on-failure',
    '--output-junit', (Join-Path $buildDirectory 'test-results.xml')
)
