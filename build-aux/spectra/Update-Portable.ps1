<#
.SYNOPSIS
Updates a portable OBS-Spectra folder to the version this script came with, keeping its settings.

.DESCRIPTION
Ships in the root of every portable OBS-Spectra zip. Unzip the new version anywhere, then run
Update-Portable.cmd from it (or this script with -Target). The folder you pick keeps its `config`
folder: profiles, scenes, plugin settings, logs and downloaded speech models. Everything else in it
is replaced with this version's files. Lucida's chat log, screenshots and loop recordings live
outside the folder and are not touched.

Without -Target, the script offers the portable OBS-Spectra that is running (close it first) and
portable folders next to this one.

.EXAMPLE
Update-Portable.ps1
.EXAMPLE
Update-Portable.ps1 -Target 'C:\bin\OBS-Spectra' -Yes
#>
param(
	[string]$Target,
	# Don't ask before replacing the files, or offer to start OBS-Spectra afterwards
	[switch]$Yes
)

$ErrorActionPreference = 'Stop'
$Source = $PSScriptRoot
$Staging = '.spectra-update'
$Exe = 'bin\64bit\obs-spectra.exe'

function Test-Install([string]$dir) {
	return $dir -and (Test-Path -LiteralPath (Join-Path $dir $Exe))
}

function Get-Root([string]$exePath) {
	# <root>\bin\64bit\obs-spectra.exe
	return Split-Path (Split-Path (Split-Path $exePath -Parent) -Parent) -Parent
}

# A zip extracted into a folder of its own name nests the install one level down
function Resolve-Install([string]$dir) {
	if (Test-Install $dir) { return $dir }
	$inner = @(Get-ChildItem -LiteralPath $dir -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Install $_.FullName })
	if ($inner.Count -eq 1) { return $inner[0].FullName }
	return $dir
}

function Get-Version([string]$dir) {
	# The newest log names the full version ("OBS 32.2.2-spectra.3 (64-bit, windows)")
	$logs = Join-Path $dir 'config\obs-studio\logs'
	$log = Get-ChildItem -LiteralPath $logs -Filter *.txt -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
	if ($log) {
		$line = Select-String -LiteralPath $log.FullName -Pattern 'OBS (\S+) \(64-bit' | Select-Object -First 1
		if ($line) { return $line.Matches[0].Groups[1].Value }
	}
	foreach ($name in @((Split-Path $dir -Leaf), (Split-Path (Split-Path $dir -Parent) -Leaf))) {
		if ($name -match '^OBS-Spectra-(.+?)-Windows-x64') { return $Matches[1] }
	}
	$product = (Get-Item -LiteralPath (Join-Path $dir $Exe)).VersionInfo.ProductVersion
	if ($product) { return $product }
	return 'unknown version'
}

function Get-Running([string]$dir) {
	$prefix = (Join-Path $dir '').ToLowerInvariant()
	return @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
		$_.Path -and $_.Path.ToLowerInvariant().StartsWith($prefix)
	})
}

function Find-Candidates {
	$found = @()
	# the portable OBS-Spectra that is running
	foreach ($p in Get-Process obs-spectra, lucida-viewer -ErrorAction SilentlyContinue) {
		if ($p.Path) { $found += Get-Root $p.Path }
	}
	# portable folders next to this one (and zips extracted into a folder of their own name)
	$parent = Split-Path $Source -Parent
	foreach ($dir in Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue) {
		$found += Resolve-Install $dir.FullName
	}
	$self = (Resolve-Path -LiteralPath $Source).Path
	return @($found | Where-Object { (Test-Install $_) -and (Test-Path -LiteralPath (Join-Path $_ 'portable_mode.txt')) } |
		ForEach-Object { (Resolve-Path -LiteralPath $_).Path } | Where-Object { $_ -ne $self } | Sort-Object -Unique)
}

function Confirm([string]$question) {
	if ($Yes) { return $true }
	$answer = Read-Host "$question [y/N]"
	return $answer -match '^(y|yes)$'
}

if (-not (Test-Install $Source)) {
	throw "Run this from an unzipped OBS-Spectra package: $Source has no $Exe."
}
$newVersion = Get-Version $Source

# --- which folder ---------------------------------------------------------------------------------
if (-not $Target) {
	$candidates = Find-Candidates
	if ($candidates.Count -eq 1) {
		$Target = $candidates[0]
	} elseif ($candidates.Count -gt 1) {
		Write-Host 'Portable OBS-Spectra folders found:'
		for ($i = 0; $i -lt $candidates.Count; $i++) {
			Write-Host ("  {0}. {1}  ({2})" -f ($i + 1), $candidates[$i], (Get-Version $candidates[$i]))
		}
		$pick = Read-Host 'Which one to update? (number)'
		if ($pick -notmatch '^\d+$' -or [int]$pick -lt 1 -or [int]$pick -gt $candidates.Count) {
			throw 'Nothing chosen; nothing changed.'
		}
		$Target = $candidates[[int]$pick - 1]
	} else {
		$Target = Read-Host 'Folder of the portable OBS-Spectra to update'
	}
}
$Target = $Target.Trim('"', ' ')
if (-not (Test-Path -LiteralPath $Target -PathType Container)) {
	throw "No such folder: $Target"
}
$Target = Resolve-Install (Resolve-Path -LiteralPath $Target).Path
if (-not (Test-Install $Target)) {
	throw "$Target is not an OBS-Spectra folder (no $Exe). Nothing changed."
}
if ($Target -eq (Resolve-Path -LiteralPath $Source).Path) {
	throw 'That is this package itself. Pick the folder you want to update.'
}

$running = Get-Running $Target
if ($running.Count) {
	$names = ($running | ForEach-Object { "$($_.ProcessName) ($($_.Id))" }) -join ', '
	throw "Close OBS-Spectra first: $names is running from $Target. Nothing changed."
}

$portable = Test-Path -LiteralPath (Join-Path $Target 'portable_mode.txt')
$config = Join-Path $Target 'config'
$configSize = 0
if (Test-Path -LiteralPath $config) {
	$configSize = (Get-ChildItem -LiteralPath $config -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
}

Write-Host ''
Write-Host "Update  $Target"
Write-Host "  from  $(Get-Version $Target)"
Write-Host "  to    $newVersion"
Write-Host ("Keeps its config folder ({0:N1} MB): profiles, scenes, plugin settings, logs." -f ($configSize / 1MB))
Write-Host 'Replaces everything else in the folder with this version.'
if (-not $portable) {
	Write-Host 'This install is not in portable mode: its settings are in %APPDATA%\OBS-Spectra and stay there.'
}
Write-Host ''
if (-not (Confirm 'Go ahead?')) {
	Write-Host 'Nothing changed.'
	exit 1
}

# --- copy, then swap ------------------------------------------------------------------------------
# The new files are copied into the folder first, so a failure while copying leaves the old
# version untouched, and a failure while swapping can be finished by running this again.
$stage = Join-Path $Target $Staging
if (Test-Path -LiteralPath $stage) {
	Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null
Write-Host 'Copying the new version...'
foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
	if ($item.Name -in @('config', $Staging)) { continue }
	Copy-Item -LiteralPath $item.FullName -Destination $stage -Recurse -Force
}

Write-Host 'Removing the old version...'
foreach ($item in Get-ChildItem -LiteralPath $Target -Force) {
	if ($item.Name -in @('config', $Staging)) { continue }
	try {
		Remove-Item -LiteralPath $item.FullName -Recurse -Force
	} catch {
		throw "Could not remove $($item.FullName): $($_.Exception.Message)`nYour settings are untouched. Close whatever is using the folder and run this again to finish."
	}
}

foreach ($item in Get-ChildItem -LiteralPath $stage -Force) {
	Move-Item -LiteralPath $item.FullName -Destination $Target -Force
}
Remove-Item -LiteralPath $stage -Recurse -Force

# Keep the install in the mode it was in: portable_mode.txt is what makes it use ./config
$marker = Join-Path $Target 'portable_mode.txt'
if ($portable -and -not (Test-Path -LiteralPath $marker)) {
	New-Item -ItemType File -Path $marker | Out-Null
} elseif (-not $portable -and (Test-Path -LiteralPath $marker)) {
	Remove-Item -LiteralPath $marker -Force
}

Write-Host ''
Write-Host "Updated to $newVersion. The folder keeps its name."
$exePath = Join-Path $Target $Exe
if (-not $Yes -and (Confirm 'Start OBS-Spectra now?')) {
	Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath -Parent)
}
