# Fate/unlimited codes rollback netplay launcher (PCSX2 rollback fork).
# Started by Host.bat / Join.bat / Training.bat. Finds the BIOS and the disc image, checks them against
# the reference hashes, writes the BIOS into the settings and starts PCSX2 with the netplay session. The
# game boots from the player's own disc image straight to the "Vs. 2P" character select (boot shortcut);
# there is no shared savestate. Training: offline, straight to the "Practice" character select.
param(
    [Parameter(Mandatory = $true)][ValidateSet('host', 'join', 'setup', 'practice')][string]$Mode,
    [string]$HostAddress = '',   # join: host address instead of asking
    [switch]$NoChecks            # testing: skip the confirmation prompts
)

# ---- Settings: both players should use the same values -------------------------------------------
$Port = 7000      # UDP port of the host (forward it in the host's router)
$Delay = 1        # input delay in frames
$Rollback = 2     # maximum rollback depth in frames

# Reference hashes of the organizer's files (only hashes, no content).
$ExpectedBiosSha256 = '__BIOS_SHA256__'
$ExpectedBiosName = '__BIOS_NAME__'
# ---------------------------------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root 'pcsx2-qtx64-clang.exe'
$ini = Join-Path $root 'inis\PCSX2.ini'

function Stop-WithMessage([string]$text) {
    Write-Host ''
    Write-Host $text -ForegroundColor Red
    Write-Host ''
    exit 1
}

# SHA-256 through .NET (works in every Windows PowerShell version, independent of modules).
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function Confirm-Continue([string]$text) {
    Write-Host $text -ForegroundColor Yellow
    if ($NoChecks) { return }
    $answer = Read-Host 'Trotzdem fortfahren? (j/n)'
    if ($answer -notmatch '^[jJyY]') { exit 1 }
}

# ---- BIOS -------------------------------------------------------------------------------------------
$bios = Get-ChildItem (Join-Path $root 'bios') -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -eq 4194304 } | Select-Object -First 1
if (-not $bios) {
    Stop-WithMessage 'Kein BIOS gefunden. Bitte die eigene BIOS-Datei (4 MB, meist .bin) in den Ordner "bios" kopieren.'
}
Write-Host "BIOS: $($bios.Name)"
$biosHash = Get-Sha256 $bios.FullName
# Only matters for netplay (both players boot their own BIOS).
if ($biosHash -ne $ExpectedBiosSha256 -and $Mode -ne 'practice') {
    Confirm-Continue "Achtung: Dieses BIOS ist eine andere Version als beim Organisator ($ExpectedBiosName). Das kann zu Desyncs fuehren."
}

# Write the BIOS file name into the settings ([Filenames] BIOS = ...).
$lines = Get-Content $ini
$section = ''
$found = $false
$out = foreach ($l in $lines) {
    if ($l -match '^\[(.+)\]$') { $section = $Matches[1] }
    if ($section -eq 'Filenames' -and $l -match '^BIOS = ') { $found = $true; "BIOS = $($bios.Name)" } else { $l }
}
if (-not $found) {
    $out = foreach ($l in $out) { $l; if ($l -eq '[Filenames]') { "BIOS = $($bios.Name)" } }
}
# Run-ahead (1 frame, faster reaction) unless the settings already say otherwise.
if (-not ($out | Where-Object { $_ -match '^RunAhead = ' })) {
    if ($out -contains '[EmuCore/Rollback]') {
        $out = foreach ($l in $out) { $l; if ($l -eq '[EmuCore/Rollback]') { 'RunAhead = 1' } }
    }
    else {
        $out = @($out) + @('', '[EmuCore/Rollback]', 'RunAhead = 1')
    }
}
$out | Set-Content $ini -Encoding UTF8

if ($Mode -eq 'setup') {
    Write-Host 'PCSX2 startet ohne Netplay. Hier kannst du Controller-Port 1 belegen (Einstellungen > Controller).'
    Write-Host 'Bitte keine anderen Einstellungen aendern: beide Spieler muessen dieselben benutzen.'
    & $exe | Out-Null
    exit 0
}

# ---- Disc image -------------------------------------------------------------------------------------
# Any dump of the game is accepted (no hash check): dumps made with different tools can differ in
# padding while the game data is the same. If a dump really differs, desync detection reports it.
$iso = Get-ChildItem (Join-Path $root 'game') -File -Include *.iso, *.chd, *.cso, *.zso, *.cue -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $iso) {
    Stop-WithMessage 'Kein Spiel gefunden. Bitte das eigene Abbild von Fate/unlimited codes (SLPM-55108, .iso/.chd/.cso/.cue) in den Ordner "game" kopieren.'
}
Write-Host "Spiel: $($iso.Name)"

# ---- Training (offline) -------------------------------------------------------------------------------
if ($Mode -eq 'practice') {
    Write-Host ''
    Write-Host 'Training: Das Spiel startet bis zur Charakterauswahl im Practice-Modus (alle Charaktere frei).'
    Write-Host 'PCSX2 startet. Zum Beenden einfach das Spielfenster schliessen.'
    & $exe -batch -bootshortcut practice -- $iso.FullName | Out-Null
    exit 0
}

# ---- Session ----------------------------------------------------------------------------------------
if ($Mode -eq 'host') {
    $spec = "host=$Port,delay=$Delay,rollback=$Rollback,boot"
    Write-Host ''
    Write-Host "Du bist Host (Spieler 1). Warte auf UDP-Port $Port auf den anderen Spieler."
    Write-Host 'Der andere Spieler braucht deine oeffentliche IP-Adresse.'
    Write-Host 'Das Spiel startet bis zur Charakterauswahl und wartet dort, bis er verbunden ist. Das ist normal.'
}
else {
    $hostAddr = $HostAddress
    if (-not $hostAddr) { $hostAddr = (Read-Host 'IP-Adresse des Hosts (z. B. 203.0.113.5)').Trim() }
    if (-not $hostAddr) { Stop-WithMessage 'Keine Adresse eingegeben.' }
    # Only an IP address (IPv4 or [IPv6]) with an optional port; nothing else reaches the emulator.
    if ($hostAddr -notmatch '^[0-9A-Fa-f.:\[\]]+$') { Stop-WithMessage "Ungueltige Adresse: $hostAddr (bitte nur die IP-Adresse eingeben)" }
    if ($hostAddr -notmatch ':\d+$') { $hostAddr = "${hostAddr}:$Port" }
    $spec = "join=$hostAddr,delay=$Delay,rollback=$Rollback,boot"
    Write-Host ''
    Write-Host "Du bist Spieler 2. Verbinde mit $hostAddr ..."
}

Write-Host 'PCSX2 startet. Zum Beenden einfach das Spielfenster schliessen.'
& $exe -batch -netplay $spec -- $iso.FullName | Out-Null
