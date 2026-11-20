# installs the vpk into vita3k, copies the game libs in, runs it for a bit
# and prints the tail of port.log.
#   .\tools\vita3k\run.ps1 [-Vita3KExe path\to\Vita3K.exe] [-GameFiles dir] [-Obb dir] [-Play]
# set NBA_VITA3K / NBA_SHACCCG once instead of passing paths every time.
# game files default to gamefiles\ (libs) and gamefiles\obb\ in the repo.
param(
    [string]$Vita3KExe = $env:NBA_VITA3K,
    [string]$Vpk,
    [string]$GameFiles,          # folder with libnba2k16_clean_opt.so + libgnustl_shared.so
    [string]$Obb,                # folder with the .obb file(s), optional
    [string]$Shacccg = $env:NBA_SHACCCG,  # your dumped libshacccg.suprx
    [int]$TimeoutSeconds = 40,
    [int]$TailLines = 80,
    [switch]$Play                # leave it running in a normal window
)
$ErrorActionPreference = 'Stop'
$titleId = 'NBAK00016'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Vpk) { $Vpk = Join-Path $repo 'build\nba2k16_vita.vpk' }
if (-not (Test-Path $Vpk)) { throw "no vpk at $Vpk, build first" }
if (-not $Vita3KExe -or -not (Test-Path $Vita3KExe)) { throw "vita3k not found, pass -Vita3KExe or set NBA_VITA3K" }

# figure out where vita3k keeps its ux0/ur0
$appDir = Split-Path -Parent $Vita3KExe
$cfg = Get-Content (Join-Path $appDir 'config.yml') -Raw
$pref = ([regex]::Match($cfg, '(?m)^pref-path:\s*(.+)$')).Groups[1].Value.Trim()
if (-not $pref) { $pref = Join-Path $appDir '' }
$dataDir = Join-Path $pref 'ux0\data\nba2k16'
New-Item -ItemType Directory -Force -Path $dataDir, (Join-Path $dataDir 'obb'), (Join-Path $pref 'ur0\data') | Out-Null
# tells the port it's running in vita3k (emu-only workarounds). this only
# ever lands in the emulator's profile, a real vita never has it.
Set-Content -Path (Join-Path $dataDir 'vita3k.flag') -Value 'vita3k' -NoNewline

if (-not $GameFiles -and (Test-Path (Join-Path $repo 'gamefiles\libnba2k16_clean_opt.so'))) {
    $GameFiles = Join-Path $repo 'gamefiles'
}
if (-not $Obb -and (Test-Path (Join-Path $repo 'gamefiles\obb'))) { $Obb = Join-Path $repo 'gamefiles\obb' }
if ($GameFiles) {
    foreach ($so in 'libnba2k16_clean_opt.so', 'libgnustl_shared.so') {
        Copy-Item (Join-Path $GameFiles $so) $dataDir -Force
    }
}
if ($Obb) {
    # it's 2GB, only copy when it actually changed
    foreach ($f in Get-ChildItem (Join-Path $Obb '*.obb')) {
        $dst = Join-Path (Join-Path $dataDir 'obb') $f.Name
        if (-not (Test-Path $dst) -or (Get-Item $dst).Length -ne $f.Length) {
            Write-Host "copying $($f.Name)..."
            Copy-Item $f.FullName $dst -Force
        }
    }
}
if ($Shacccg -and (Test-Path $Shacccg) -and -not (Test-Path (Join-Path $pref 'ur0\data\libshacccg.suprx'))) {
    Copy-Item $Shacccg (Join-Path $pref 'ur0\data\libshacccg.suprx')
}

$log = Join-Path $dataDir 'port.log'
Remove-Item $log -Force -ErrorAction SilentlyContinue

# install (vita3k takes the vpk as a positional arg)
$out = Join-Path $env:TEMP 'nba2k16-vita3k-install.log'
$p = Start-Process $Vita3KExe -ArgumentList ('"' + $Vpk + '"') -WorkingDirectory $appDir `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput $out
$deadline = (Get-Date).AddSeconds(30)
$installed = $false
while ((Get-Date) -lt $deadline) {
    if ((Test-Path $out) -and ((Get-Content $out -Raw) -match "\[$titleId\] installed successfully")) { $installed = $true; break }
    if ($p.HasExited) { break }
    Start-Sleep -Milliseconds 250
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
if (-not $installed) { throw "install didn't confirm, check $out" }
Write-Host "installed $titleId"

# run
$stdout = Join-Path $env:TEMP 'nba2k16-vita3k-run.log'
if ($Play) {
    Start-Process $Vita3KExe -ArgumentList @('-r', $titleId) -WorkingDirectory $appDir | Out-Null
    Write-Host "running, log: $log"
    exit 0
}
$p = Start-Process $Vita3KExe -ArgumentList @('-r', $titleId) -WorkingDirectory $appDir `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
$exited = $p.HasExited
if (-not $exited) {
    Stop-Process -Id $p.Id -Force
    # we killed it, maybe mid shader compile. don't let the crash guard
    # blame that shader on the next boot.
    Remove-Item (Join-Path $dataDir 'shadercache\*.compiling') -ErrorAction SilentlyContinue
}

Write-Host "--- vita3k $(if ($exited) { 'exited on its own' } else { 'killed after timeout' }) ---"
# vita3k buffers port.log on the host side and loses the end when killed,
# but every line also goes through sceClibPrintf which lands in its stdout
$lines = Get-Content $stdout | Select-String 'export_sceClibPrintf\]: (.*)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
if ($lines) {
    $lines | Select-Object -Last $TailLines
} elseif (Test-Path $log) {
    Get-Content $log -Tail $TailLines
} else {
    Write-Host "no log lines, vita3k output tail:"
    Get-Content $stdout -Tail 40
}
# emulator-side trouble worth knowing about
Get-Content $stdout | Select-String 'PC is|ending the app|NID .* not found' | Select-Object -Last 5
