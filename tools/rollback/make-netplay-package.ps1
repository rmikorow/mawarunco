# Builds the netplay test package from the fork's Release build.
#
#   .\tools\rollback\make-netplay-package.ps1 -ReferenceBios <bios .bin>
#
# Output: dist\Mawarunco-v<version>.zip (version from the git tag of HEAD), built in dist\.staging\ (private to this script, deleted and
# recreated on every run; to test the package, unzip it somewhere else). The package contains the emulator, sanitized
# settings with the start window switched on, the guide, licenses and the fork's source patch. It never
# contains a BIOS or a disc image; the reference BIOS is only used to compute a hash for the start window.
# The disc image is not checked: any dump of the game is accepted. There is no start savestate: both
# players boot their own copy with the boot shortcut straight to the chosen mode.
param(
    # BIOS the start window compares against (only its MD5 and file name go into the package).
    [Parameter(Mandatory = $true)][string]$ReferenceBios,
    # Microsoft.VC143.CRT folder; found through vswhere when empty.
    [string]$VcRedistDir = '',
    # Folder of the cargo home (for the GGRS license files); derived when empty.
    [string]$CargoHome = '',
    # Also build a patch for players who already have the package: program, guide and source only, no
    # settings (their controller mapping, button config, delay and host address stay).
    [switch]$Update,
    # Also build the installer dist\Mawarunco-Setup-v<version>.exe (Inno Setup 6). ISCC.exe comes from
    # -IsccPath, the MAWARUNCO_ISCC environment variable, the PATH or the usual install folders.
    [switch]$Installer,
    [string]$IsccPath = ''
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

if (-not $VcRedistDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -property installationPath
    $VcRedistDir = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'x64\Microsoft.VC143.CRT') } |
        Sort-Object Name -Descending | Select-Object -First 1 |
        ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' }
    if (-not $VcRedistDir) { throw 'Visual C++ runtime not found, pass -VcRedistDir.' }
}
if (-not $CargoHome) {
    # Same lookup as the build: RollbackRustDir from the local props file, then CARGO_HOME, then ~\.cargo.
    $localProps = Join-Path $repo '3rdparty\ggrs-capi\ggrs-capi.local.props'
    if ((Test-Path $localProps) -and ((Get-Content $localProps -Raw) -match '<RollbackRustDir[^>]*>([^<]+)<')) {
        $CargoHome = Join-Path $Matches[1] 'cargo'
    }
    elseif ($env:CARGO_HOME) { $CargoHome = $env:CARGO_HOME }
    else { $CargoHome = Join-Path $env:USERPROFILE '.cargo' }
}
$bin = Join-Path $repo 'bin'
$dist = Join-Path $repo 'dist'
# Only this staging folder is ever deleted. Never point it at a folder someone uses (it may hold their
# BIOS and disc image).
$staging = Join-Path $dist '.staging'
$out = Join-Path $staging 'Mawarunco'
$template = Join-Path $PSScriptRoot 'package'
# The build is named like upstream (pcsx2-qtx64-clang.exe); players get Mawarunco.exe. Nothing in the
# program depends on its file name.
$exeName = 'Mawarunco.exe'

# Version for the file names: the release tag on HEAD (v0.2.2 -> 0.2.2), otherwise git describe
# (0.2.1-3-g1234abcd), so a test build never looks like a release.
Push-Location $repo
try { $version = ((git describe --tags --match 'v[0-9]*' HEAD) | Out-String).Trim() -replace '^v', '' }
finally { Pop-Location }
if (-not $version) { throw 'No version tag (v<major>.<minor>.<patch>) found for HEAD.' }

if (-not (Test-Path (Join-Path $bin 'pcsx2-qtx64-clang.exe'))) { throw 'Release build missing (bin\pcsx2-qtx64-clang.exe).' }
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

# ---- Emulator files (no debug symbols, import libraries or the Devel build) -------------------------
Get-ChildItem $bin -File | Where-Object {
    ($_.Extension -in '.dll', '.conf') -or ($_.Name -eq 'portable.txt')
} | Copy-Item -Destination $out
Copy-Item (Join-Path $bin 'pcsx2-qtx64-clang.exe') (Join-Path $out $exeName)
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
'Copy your own PS2 BIOS file (4 MB, usually .bin) into this folder.' | Set-Content (Join-Path $out 'bios\PUT_BIOS_HERE.txt') -Encoding UTF8
'Copy your own disc image of Fate/unlimited codes (SLPM-55108) into this folder.' | Set-Content (Join-Path $out 'game\PUT_GAME_HERE.txt') -Encoding UTF8
foreach ($dir in 'memcards', 'sstates', 'logs') {
    'PCSX2 data folder.' | Set-Content (Join-Path $out "$dir\README.txt") -Encoding UTF8
}

# ---- Settings: copy of the fork's settings without personal paths or test switches ------------------
# Returns the line to write, or $null to drop it.
function Convert-SettingsLine([string]$section, [string]$line) {
    # The launcher section is written fresh below (the organizer's last host address stays private).
    if ($section -in 'GameListTableView', 'Launcher') { return $null }
    if ($line -match '^\[|^$') { return $line }
    switch ($section) {
        'UI' { if ($line -match '^(MainWindow(Geometry|State)|RollbackLauncher|RenderToSeparateWindow|HideMainWindowWhenRunning) = ') { return $null } }
        'Folders' { if ($line -match '^Bios = ') { return 'Bios = bios' } }
        'Filenames' { if ($line -match '^BIOS = ') { return 'BIOS = ' } }
        'GameList' { if ($line -match '^RecursivePaths = ') { return $null } }
        'SPU2/Output' { if ($line -match '^OutputMuted = ') { return 'OutputMuted = false' } }
        'Hotkeys' { if ($line -match '^RollbackSelfTest') { return $null } }
        'EmuCore/Rollback' { if ($line -notmatch '^(DeterministicEEBlocks|DeterministicIOPBlocks) = ') { return $null } }
        'Pad2' { if ($line -match '^Type = ') { return 'Type = DualShock2' } }
        # PINE (external memory access) stays off for players: a tool writing memory would desync netplay.
        'EmuCore' { if ($line -match '^EnablePINE = ') { return 'EnablePINE = false' } }
    }
    return $line
}

$section = ''
$settings = foreach ($l in Get-Content (Join-Path $bin 'inis\PCSX2.ini')) {
    if ($l -match '^\[(.+)\]$') { $section = $Matches[1] }
    $converted = Convert-SettingsLine $section $l
    if ($null -ne $converted) { $converted }
    # Run-ahead (1 frame) is on in the package: ~10 ms less input latency, local only (cannot desync),
    # switches itself off on a PC that is too slow.
    if ($l -eq '[EmuCore/Rollback]') { 'RunAhead = 1' }
    # The start window (pcsx2-qt/RollbackLauncher) replaces the main window; the game gets its own window.
    if ($l -eq '[UI]') { 'RollbackLauncher = true'; 'RenderToSeparateWindow = true'; 'HideMainWindowWhenRunning = true' }
}
# Launcher defaults and the organizer's BIOS (only its MD5 and file name, to warn about another version).
$settings = @($settings) + @(
    '',
    '[Launcher]',
    'Delay = 1',
    'Rollback = 2',
    'Port = 7000',
    "ReferenceBios = $(Split-Path $ReferenceBios -Leaf)",
    "ReferenceBiosMD5 = $((Get-FileHash $ReferenceBios -Algorithm MD5).Hash)"
)
$settings | Set-Content (Join-Path $out 'inis\PCSX2.ini') -Encoding UTF8
$check = Get-Content (Join-Path $out 'inis\PCSX2.ini') -Raw
if ($check -match '[A-Z]:\\') { throw 'Sanitized settings still contain an absolute path.' }
if ($check -notmatch '(?m)^RollbackLauncher = true') { throw 'Settings have no [UI] section for the launcher.' }

# ---- Guide ------------------------------------------------------------------------------------------------
# Rewritten line by line so the package gets Windows line endings (CRLF) whatever the checkout uses.
Get-Content (Join-Path $template 'README.txt') | Set-Content (Join-Path $out 'README.txt') -Encoding ascii

# ---- Licenses and source (GPL: the changes to PCSX2 v2.8.2 as a patch) -------------------------------
# Rust crates linked into the program (3rdparty/ggrs-capi, from Cargo.lock): each crate's license files go
# to docs\rust-licenses\<crate>-<version>, plus an overview with the license of each crate.
$registry = Join-Path $CargoHome 'registry\src'
$licenseDir = Join-Path $out 'docs\rust-licenses'
New-Item -ItemType Directory -Force $licenseDir | Out-Null
$overview = @('Rust crates linked into Mawarunco (3rdparty/ggrs-capi), with their licenses.', '')
$lock = Get-Content (Join-Path $repo '3rdparty\ggrs-capi\Cargo.lock') -Raw
foreach ($m in [regex]::Matches($lock, '(?ms)^\[\[package\]\]\s*name = "([^"]+)"\s*version = "([^"]+)"(.*?)(?=^\[\[package\]\]|\z)')) {
    $name = $m.Groups[1].Value; $version = $m.Groups[2].Value
    if ($m.Groups[3].Value -notmatch 'source = "registry') { continue } # the wrapper itself
    $src = Get-ChildItem $registry -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "$name-$version" } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $src) { Write-Warning "Sources of $name $version not found under $registry (license missing)."; continue }
    $license = (Select-String -Path (Join-Path $src 'Cargo.toml') -Pattern '^license\s*=\s*"([^"]+)"' |
        Select-Object -First 1).Matches.Groups[1].Value
    $overview += ('{0} {1}: {2}' -f $name, $version, $(if ($license) { $license } else { 'see files' }))
    $files = Get-ChildItem $src -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE|UNLICENSE|COPYRIGHT)' }
    if ($files) {
        $dest = Join-Path $licenseDir "$name-$version"
        New-Item -ItemType Directory -Force $dest | Out-Null
        $files | Copy-Item -Destination $dest
    }
}
$overview | Set-Content (Join-Path $licenseDir 'README.txt') -Encoding UTF8
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
# Only the newest package (and patch) stays in dist, so it is always clear which one to send. Only zip
# files and their checksum files are removed, never a folder (dist may hold a player's unpacked copy).
Get-ChildItem $dist -File | Where-Object { $_.Name -match '^(FUC-Netplay|Mawarunco)-((Patch-)?(\d{8}|v[\w.-]+)\.zip|Setup-(v[\w.-]+|[\d.]+)\.exe)(\.sha256\.txt)?$' } |
    Remove-Item -Force
$zip = Join-Path $dist "Mawarunco-v$version.zip"
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal
$size = (Get-Item $zip).Length / 1MB
# SHA-256 next to the zip, so players can check that their copy is the one the organizer built.
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256.txt" -Encoding ascii
Write-Host ("Package: {0} ({1:N0} MB)" -f $zip, $size)
Write-Host "SHA-256: $hash"

# ---- Patch for existing installations --------------------------------------------------------------------
if ($Update) {
    $upd = Join-Path $staging 'update'
    New-Item -ItemType Directory -Force (Join-Path $upd 'source') | Out-Null
    Copy-Item (Join-Path $out $exeName), (Join-Path $out 'README.txt') -Destination $upd
    Copy-Item (Join-Path $out 'source\*') -Destination (Join-Path $upd 'source')
    @(
        'Patch for an existing Mawarunco (Fate/unlimited codes netplay) folder.',
        '',
        'Close the program, then copy the contents of this zip into your folder and replace the files.',
        'Your settings (controller, button config, delay, host address) stay as they are.',
        'The program is now called Mawarunco.exe. You can delete the old pcsx2-qtx64-clang.exe and must',
        'update shortcuts you made to it. Windows Firewall asks once more on the first online game: "Allow".',
        'Old files from earlier versions that are no longer needed: Host.bat, Join.bat, Training.bat,',
        'Einstellungen.bat, netplay.ps1, start.p2s, LIESMICH.txt (you can delete them).'
    ) | Set-Content (Join-Path $upd 'PATCH_README.txt') -Encoding ascii
    $uzip = Join-Path $dist "Mawarunco-Patch-v$version.zip"
    if (Test-Path $uzip) { Remove-Item $uzip -Force }
    Compress-Archive -Path (Join-Path $upd '*') -DestinationPath $uzip -CompressionLevel Optimal
    $uhash = (Get-FileHash $uzip -Algorithm SHA256).Hash
    "$uhash  $(Split-Path $uzip -Leaf)" | Set-Content "$uzip.sha256.txt" -Encoding ascii
    Write-Host ("Patch:   {0} ({1:N0} MB)" -f $uzip, ((Get-Item $uzip).Length / 1MB))
    Write-Host "SHA-256: $uhash"
}

# ---- Installer ----------------------------------------------------------------------------------------------
if ($Installer) {
    if (-not $IsccPath) {
        $IsccPath = @($env:MAWARUNCO_ISCC, (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')) |
            Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    }
    if (-not $IsccPath) { throw 'Inno Setup compiler (ISCC.exe) not found, pass -IsccPath.' }
    $iss = Join-Path $PSScriptRoot 'installer\mawarunco.iss'
    & $IsccPath /Q "/DSourceDir=$out" "/DOutputDir=$dist" "/DAppVersion=$version" "/DRepoDir=$repo" $iss
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)." }
    $setup = Join-Path $dist "Mawarunco-Setup-v$version.exe"
    $shash = (Get-FileHash $setup -Algorithm SHA256).Hash
    "$shash  $(Split-Path $setup -Leaf)" | Set-Content "$setup.sha256.txt" -Encoding ascii
    Write-Host ("Setup:   {0} ({1:N0} MB)" -f $setup, ((Get-Item $setup).Length / 1MB))
    Write-Host "SHA-256: $shash"
}
