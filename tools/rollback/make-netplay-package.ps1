# Builds the netplay test package from the fork's Release build.
#
#   .\tools\rollback\make-netplay-package.ps1 -StateFile <start state .p2s> [-ReferenceIso <iso>] [-ReferenceBios <bin>]
#
# Output: dist\FUC-Netplay\ and dist\FUC-Netplay-<date>.zip. The package contains the emulator, sanitized
# settings, the launcher, the tester guide, licenses and the fork's source patch. It never contains a
# BIOS or a disc image; the reference files are only used to compute SHA-256 hashes for the launcher.
param(
    [string]$StateFile = '',
    [string]$ReferenceIso = '<path to your disc image>',
    [string]$ReferenceBios = '<path to your BIOS file>',
    [string]$VcRedistDir = 'F:\VisualStudio\2022\Community\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT',
    [string]$RustDir = '<Rust toolchain folder>'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$bin = Join-Path $repo 'bin'
$dist = Join-Path $repo 'dist'
$out = Join-Path $dist 'FUC-Netplay'
$template = Join-Path $PSScriptRoot 'package'

if (-not (Test-Path (Join-Path $bin 'pcsx2-qtx64-clang.exe'))) { throw 'Release build missing (bin\pcsx2-qtx64-clang.exe).' }
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

# ---- Emulator files (no debug symbols, import libraries or the Devel build) -------------------------
Get-ChildItem $bin -File | Where-Object {
    ($_.Extension -in '.dll', '.conf') -or ($_.Name -in 'pcsx2-qtx64-clang.exe', 'portable.txt')
} | Copy-Item -Destination $out
foreach ($dir in 'QtPlugins', 'D3D12', 'resources', 'translations', 'docs') {
    Copy-Item (Join-Path $bin $dir) -Destination (Join-Path $out $dir) -Recurse
}
Get-ChildItem $out -Recurse -Include *.pdb, *.lib | Remove-Item -Force

# Visual C++ runtime, app-local (allowed by Microsoft's redistribution terms).
Get-ChildItem $VcRedistDir -Filter *.dll | Copy-Item -Destination $out

foreach ($dir in 'bios', 'game', 'memcards', 'sstates', 'logs', 'inis', 'source') {
    New-Item -ItemType Directory -Force (Join-Path $out $dir) | Out-Null
}
# Zip archives drop empty folders, so the folders the tester fills get a note.
'Hier die eigene PS2-BIOS-Datei (4 MB, meist .bin) hineinkopieren.' | Set-Content (Join-Path $out 'bios\BIOS_HIER_HINEIN.txt') -Encoding UTF8
'Hier das eigene ISO von Fate/unlimited codes (SLPM-55108) hineinkopieren.' | Set-Content (Join-Path $out 'game\ISO_HIER_HINEIN.txt') -Encoding UTF8
foreach ($dir in 'memcards', 'sstates', 'logs') {
    'PCSX2 data folder.' | Set-Content (Join-Path $out "$dir\README.txt") -Encoding UTF8
}

# ---- Settings: copy of the fork's settings without personal paths or test switches ------------------
# Returns the line to write, or $null to drop it.
function Convert-SettingsLine([string]$section, [string]$line) {
    if ($section -eq 'GameListTableView') { return $null }
    if ($line -match '^\[|^$') { return $line }
    switch ($section) {
        'UI' { if ($line -match '^MainWindow(Geometry|State) = ') { return $null } }
        'Folders' { if ($line -match '^Bios = ') { return 'Bios = bios' } }
        'Filenames' { if ($line -match '^BIOS = ') { return 'BIOS = ' } }
        'GameList' { if ($line -match '^RecursivePaths = ') { return $null } }
        'SPU2/Output' { if ($line -match '^OutputMuted = ') { return 'OutputMuted = false' } }
        'Hotkeys' { if ($line -match '^RollbackSelfTest') { return $null } }
        'EmuCore/Rollback' { if ($line -notmatch '^(DeterministicEEBlocks|DeterministicIOPBlocks) = ') { return $null } }
        'Pad2' { if ($line -match '^Type = ') { return 'Type = DualShock2' } }
    }
    return $line
}

$section = ''
$settings = foreach ($l in Get-Content (Join-Path $bin 'inis\PCSX2.ini')) {
    if ($l -match '^\[(.+)\]$') { $section = $Matches[1] }
    $converted = Convert-SettingsLine $section $l
    if ($null -ne $converted) { $converted }
}
$settings | Set-Content (Join-Path $out 'inis\PCSX2.ini') -Encoding UTF8
$check = Get-Content (Join-Path $out 'inis\PCSX2.ini') -Raw
if ($check -match '[A-Z]:\\') { throw 'Sanitized settings still contain an absolute path.' }

# ---- Launcher with reference hashes, guide -----------------------------------------------------------
$isoHash = (Get-FileHash $ReferenceIso -Algorithm SHA256).Hash
$biosHash = (Get-FileHash $ReferenceBios -Algorithm SHA256).Hash
(Get-Content (Join-Path $template 'netplay.ps1') -Raw) `
    -replace '__ISO_SHA256__', $isoHash `
    -replace '__BIOS_SHA256__', $biosHash `
    -replace '__BIOS_NAME__', (Split-Path $ReferenceBios -Leaf) |
    Set-Content (Join-Path $out 'netplay.ps1') -Encoding UTF8
foreach ($f in 'Host.bat', 'Join.bat', 'Einstellungen.bat', 'LIESMICH.txt') {
    Copy-Item (Join-Path $template $f) -Destination $out
}

# ---- Start state --------------------------------------------------------------------------------------
if ($StateFile) {
    Copy-Item $StateFile -Destination (Join-Path $out 'start.p2s')
}
else {
    Write-Warning 'No -StateFile given: the package has no start.p2s yet.'
}

# ---- Licenses and source (GPL: the changes to PCSX2 v2.8.2 as a patch) -------------------------------
$ggrs = Get-ChildItem (Join-Path $RustDir 'cargo\registry\src') -Directory -Recurse -Filter 'ggrs-0.13.0' | Select-Object -First 1
if ($ggrs) {
    New-Item -ItemType Directory -Force (Join-Path $out 'docs\ggrs') | Out-Null
    Get-ChildItem $ggrs.FullName -File -Filter 'LICENSE*' | Copy-Item -Destination (Join-Path $out 'docs\ggrs')
}
Push-Location $repo
try {
    $head = (git rev-parse --short HEAD).Trim()
    git diff v2.8.2..HEAD --binary | Set-Content (Join-Path $out "source\pcsx2-rollback-fork-$head.diff") -Encoding UTF8
    @(
        'PCSX2 rollback fork, source of the binaries in this package.',
        '',
        'Base: PCSX2 v2.8.2, https://github.com/PCSX2/pcsx2 (tag v2.8.2).',
        "Changes: pcsx2-rollback-fork-$head.diff (git diff v2.8.2..$head), apply with 'git apply'.",
        'Build: see .github/workflows/scripts/windows/build-dependencies.bat, then',
        '       msbuild PCSX2_qt.sln "/p:Configuration=Release Clang" /p:Platform=x64',
        '       (needs the Rust toolchain for 3rdparty/ggrs-capi).'
    ) | Set-Content (Join-Path $out 'source\SOURCE.txt') -Encoding UTF8
}
finally { Pop-Location }

# ---- Zip ----------------------------------------------------------------------------------------------
$zip = Join-Path $dist ("FUC-Netplay-{0}.zip" -f (Get-Date -Format 'yyyyMMdd'))
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal
$size = (Get-Item $zip).Length / 1MB
Write-Host ("Package: {0} ({1:N0} MB)" -f $zip, $size)
