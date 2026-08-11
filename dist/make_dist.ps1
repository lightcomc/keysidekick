# KeySidekick distribution packager.
# Builds the binaries (must succeed), then assembles dist\KeySidekick-$Version.zip
# with the runtime files + docs, and writes dist\KeySidekick-$Version.zip.sha256.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File dist\make_dist.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File dist\make_dist.ps1 -Version 0.9.0
#
# Version resolution: -Version > APP_VERSION in src\sidekick.cpp > "0.9.0".

[CmdletBinding()]
param(
    [string]$Version = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$RepoRoot = Split-Path -Parent $PSScriptRoot   # ...\dist -> repo root
$SrcDir = Join-Path $RepoRoot 'src'
$DistDir = Join-Path $RepoRoot 'dist'

# --- 1. Resolve version -------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Version)) {
    $cpp = Join-Path $SrcDir 'sidekick.cpp'
    $m = $null
    if (Test-Path $cpp) {
        $m = Select-String -Path $cpp -Pattern 'APP_VERSION\s*=\s*"([^"]+)"' |
             Select-Object -First 1
        if (-not $m) {
            $m = Select-String -Path $cpp -Pattern '#define\s+APP_VERSION\s+"([^"]+)"' |
                 Select-Object -First 1
        }
    }
    if ($m) { $Version = $m.Matches[0].Groups[1].Value }
    if ([string]::IsNullOrWhiteSpace($Version)) { $Version = '0.9.0' }
}
$Version = $Version.Trim()
Write-Host "Packaging KeySidekick v$Version"

# --- 2. Snapshot source mtimes (stale-dashboard / torn-edit guard) ------------
$watchPaths = @(
    (Join-Path $SrcDir 'sidekick.cpp'),
    (Join-Path $SrcDir 'resources.rc')
) + @(Get-ChildItem (Join-Path $RepoRoot 'web') -File | ForEach-Object { $_.FullName })
$mtimeBefore = @{}
foreach ($p in $watchPaths) {
    if (Test-Path $p) { $mtimeBefore[$p] = (Get-Item $p).LastWriteTimeUtc }
}

# --- 3. Build (run from src\ so relative outputs land next to sources) --------
function Invoke-Build {
    Push-Location $SrcDir
    try {
        & .\build.bat
        if ($LASTEXITCODE -ne 0) { throw "src\build.bat failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
}
Invoke-Build

# If any watched source changed while we were building (concurrent edit),
# rebuild once so the binary embeds the freshest dashboard/code.
$stale = $false
foreach ($p in $watchPaths) {
    if ((Test-Path $p) -and $mtimeBefore.ContainsKey($p) -and
        (Get-Item $p).LastWriteTimeUtc -gt $mtimeBefore[$p]) {
        $stale = $true
    }
}
if ($stale) {
    Write-Warning 'Source files changed during the build; rebuilding once so the zip embeds the freshest binary.'
    Invoke-Build
}

# --- 4. Assemble the file list (warn + skip anything missing) -----------------
$entries = @(
    @{ Src = Join-Path $SrcDir  'sidekick.exe';                 Rel = 'sidekick.exe' },
    @{ Src = Join-Path $SrcDir  'probe_device.exe';             Rel = 'probe_device.exe' },
    @{ Src = Join-Path $SrcDir  'config.example.ini';           Rel = 'config.example.ini' },
    @{ Src = Join-Path $SrcDir  'run.bat';                      Rel = 'run.bat' },
    @{ Src = Join-Path $RepoRoot 'README.md';                   Rel = 'README.md' },
    @{ Src = Join-Path $RepoRoot 'LICENSE';                     Rel = 'LICENSE' },
    @{ Src = Join-Path $RepoRoot 'ZADIG_INSTRUCTIONS.md';       Rel = 'ZADIG_INSTRUCTIONS.md' },
    @{ Src = Join-Path $RepoRoot 'docs\FAQ.md';                 Rel = 'docs\FAQ.md' },
    @{ Src = Join-Path $RepoRoot 'docs\HID-USAGE-TABLE.md';     Rel = 'docs\HID-USAGE-TABLE.md' },
    @{ Src = Join-Path $RepoRoot 'docs\PROBLEM-AND-SOLUTION.md'; Rel = 'docs\PROBLEM-AND-SOLUTION.md' }
)
foreach ($e in $entries) {
    if (-not (Test-Path $e.Src)) {
        Write-Warning "MISSING (skipped from zip): $($e.Src)"
    }
}
$present = @($entries | Where-Object { Test-Path $_.Src })
if ($present.Count -eq 0) {
    throw 'No distributable files found - nothing to package.'
}

# --- 5. Stage, zip, checksum --------------------------------------------------
$stage = Join-Path $DistDir ".stage-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
    foreach ($e in $present) {
        $dest = Join-Path $stage $e.Rel
        $destDir = Split-Path -Parent $dest
        if ($destDir -and -not (Test-Path $destDir)) {
            New-Item -ItemType Directory -Force -Path $destDir | Out-Null
        }
        Copy-Item -LiteralPath $e.Src -Destination $dest -Force
    }

    $zipName = "KeySidekick-$Version.zip"
    $zipPath = Join-Path $DistDir $zipName
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal

    $hash = Get-FileHash -Algorithm SHA256 -Path $zipPath
    $shaPath = "$zipPath.sha256"
    Set-Content -LiteralPath $shaPath -Encoding Ascii -Value ("{0}  {1}" -f $hash.Hash.ToLowerInvariant(), $zipName)

    Write-Host ''
    Write-Host "ZIP: $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1MB, 2)) MB)"
    Write-Host "SHA256: $($hash.Hash.ToLowerInvariant())"
    Write-Host "SHA256 file: $shaPath"
    Write-Host 'Contents:'
    Get-ChildItem -Recurse -File $stage | ForEach-Object {
        Write-Host ("  {0}  ({1} bytes)" -f $_.FullName.Substring($stage.Length + 1), $_.Length)
    }
} finally {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
}

exit 0
