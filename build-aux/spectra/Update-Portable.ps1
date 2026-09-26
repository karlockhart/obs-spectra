<#
.SYNOPSIS
Updates a portable OBS-Spectra folder, keeping its settings: to the latest release or release
candidate from GitHub, or to the unzipped version this script came with.

.DESCRIPTION
Ships in the root of every portable OBS-Spectra zip, and stays in the folder it updates.

  Double-click Update-Portable.cmd in your OBS-Spectra folder to update it from GitHub: you choose
  the latest release or the latest release candidate.

  Double-click it in a freshly unzipped new version to put that version into your existing folder.

The folder keeps its `config` folder (profiles, scenes, plugin settings, logs and downloaded speech
models) and whether it runs in portable mode. Everything else in it is replaced. Lucida's chat log,
screenshots and loop recordings live outside the folder and are not touched. OBS-Spectra must be
closed while it runs.

.PARAMETER Target
The OBS-Spectra folder to update. Without it: the folder this script is in if that's an install,
else the running portable OBS-Spectra or the portable folders next to this one.

.PARAMETER Channel
Download from GitHub: `release` is the latest full release, `rc` the newest release or release
candidate, whichever is newer.

.PARAMETER Zip
Apply this OBS-Spectra zip (or unzipped folder) instead.

.PARAMETER Yes
Don't ask before replacing the files.

.PARAMETER Force
Update even when the folder already has that version.

.PARAMETER Shortcuts
Only (re)make the OBS-Spectra and Lucida Viewer shortcuts in the folder, e.g. after moving it.
Updating makes them too.

.EXAMPLE
Update-Portable.ps1 -Channel rc -Target 'C:\bin\OBS-Spectra'
.EXAMPLE
Update-Portable.ps1 -Zip 'Downloads\OBS-Spectra-32.2.2-spectra.3-Windows-x64-Portable.zip'
#>
param(
	[string]$Target,
	[ValidateSet('release', 'rc')]
	[string]$Channel,
	[string]$Zip,
	[switch]$Yes,
	[switch]$Force,
	[switch]$Shortcuts
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Windows PowerShell's progress bar makes downloads crawl
$Repo = 'karlockhart/obs-spectra'
$Staging = '.spectra-update'
$Exe = 'bin\64bit\obs-spectra.exe'
$VersionFile = 'spectra-version.txt' # written after an update
$Here = $PSScriptRoot
# Kept in memory: the copy on disk may be replaced or removed while this runs
$SelfScript = Get-Content -LiteralPath $PSCommandPath -Raw
$SelfCmd = "@echo off & powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Update-Portable.ps1`" %* & echo. & pause & exit /b`r`n"

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
	# The newest log names the full version ("OBS 32.2.2-spectra.3 (64-bit, windows)"), unless
	# this script updated the folder since it last ran
	$logs = Join-Path $dir 'config\obs-studio\logs'
	$log = Get-ChildItem -LiteralPath $logs -Filter *.txt -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
	$marker = Get-Item -LiteralPath (Join-Path $dir $VersionFile) -ErrorAction SilentlyContinue
	if ($marker -and (-not $log -or $marker.LastWriteTime -gt $log.LastWriteTime)) {
		return (Get-Content -LiteralPath $marker.FullName -Raw).Trim()
	}
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

# Shortcuts in the folder's root to its programs. A shortcut holds an absolute path, so they're
# made here, where the folder's real location is known, and remade after every update.
function Set-Shortcuts([string]$dir) {
	$shell = New-Object -ComObject WScript.Shell
	$made = @()
	foreach ($s in @(@{ Name = 'OBS-Spectra'; Exe = 'bin\64bit\obs-spectra.exe'; What = 'Start OBS-Spectra' },
			@{ Name = 'Lucida Viewer'; Exe = 'bin\64bit\lucida-viewer.exe'; What = "Browse Lucida's chat log" })) {
		$exe = Join-Path $dir $s.Exe
		$path = Join-Path $dir "$($s.Name).lnk"
		if (-not (Test-Path -LiteralPath $exe)) {
			Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
			continue
		}
		$link = $shell.CreateShortcut($path)
		$link.TargetPath = $exe
		$link.WorkingDirectory = Split-Path $exe -Parent
		$link.IconLocation = "$exe,0"
		$link.Description = $s.What
		$link.Save()
		$made += $s.Name
	}
	return $made
}

function Get-Running([string]$dir) {
	$prefix = (Join-Path $dir '').ToLowerInvariant()
	return @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
		$_.Path -and $_.Path.ToLowerInvariant().StartsWith($prefix)
	})
}

function Find-Candidates([string]$besides) {
	$found = @()
	# the portable OBS-Spectra that is running
	foreach ($p in Get-Process obs-spectra, lucida-viewer -ErrorAction SilentlyContinue) {
		if ($p.Path) { $found += Get-Root $p.Path }
	}
	# portable folders next to this one (and zips extracted into a folder of their own name)
	foreach ($dir in Get-ChildItem -LiteralPath (Split-Path $besides -Parent) -Directory -ErrorAction SilentlyContinue) {
		$found += Resolve-Install $dir.FullName
	}
	$self = (Resolve-Path -LiteralPath $besides).Path
	return @($found | Where-Object { (Test-Install $_) -and (Test-Path -LiteralPath (Join-Path $_ 'portable_mode.txt')) } |
		ForEach-Object { (Resolve-Path -LiteralPath $_).Path } | Where-Object { $_ -ne $self } | Sort-Object -Unique)
}

function Confirm([string]$question) {
	if ($Yes) { return $true }
	$answer = Read-Host "$question [y/N]"
	return $answer -match '^(y|yes)$'
}

function Choose([string]$question, [string[]]$options) {
	for ($i = 0; $i -lt $options.Count; $i++) {
		Write-Host ("  {0}. {1}" -f ($i + 1), $options[$i])
	}
	$pick = Read-Host $question
	if ($pick -notmatch '^\d+$' -or [int]$pick -lt 1 -or [int]$pick -gt $options.Count) {
		throw 'Nothing chosen; nothing changed.'
	}
	return [int]$pick - 1
}

# --- GitHub ---------------------------------------------------------------------------------------

function Get-Releases {
	[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
	$headers = @{ 'User-Agent' = 'OBS-Spectra-Update-Portable'; 'Accept' = 'application/vnd.github+json' }
	$all = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases?per_page=30" -Headers $headers
	# newest first, with a portable zip to download
	return @($all | Where-Object { -not $_.draft -and ($_.assets | Where-Object { $_.name -like '*-Windows-x64-Portable.zip' }) } |
		Sort-Object { [datetime]$_.published_at } -Descending)
}

function Select-Release([object[]]$releases, [string]$channel) {
	if ($channel -eq 'release') {
		return $releases | Where-Object { -not $_.prerelease } | Select-Object -First 1
	}
	return $releases | Select-Object -First 1
}

function Get-Release([object]$release) {
	$asset = $release.assets | Where-Object { $_.name -like '*-Windows-x64-Portable.zip' } | Select-Object -First 1
	$sum = $release.assets | Where-Object { $_.name -eq "$($asset.name).sha256" } | Select-Object -First 1
	$dir = Join-Path ([IO.Path]::GetTempPath()) "obs-spectra-update\$($release.tag_name)"
	if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
	New-Item -ItemType Directory -Path $dir | Out-Null
	$zipPath = Join-Path $dir $asset.name
	$headers = @{ 'User-Agent' = 'OBS-Spectra-Update-Portable' }
	Write-Host ("Downloading {0} ({1:N0} MB)..." -f $asset.name, ($asset.size / 1MB))
	Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath -Headers $headers -UseBasicParsing
	if ($sum) {
		$content = (Invoke-WebRequest -Uri $sum.browser_download_url -Headers $headers -UseBasicParsing).Content
		if ($content -is [byte[]]) { $content = [Text.Encoding]::ASCII.GetString($content) }
		$expected = ($content.Trim() -split '\s+')[0]
		$actual = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
		if ($actual -ne $expected.Trim()) {
			throw "The download does not match its checksum ($($sum.name)). Nothing changed; try again."
		}
		Write-Host 'Checksum verified.'
	} else {
		Write-Host 'This release has no checksum file; the download was not verified.'
	}
	return Expand-Package $zipPath $dir
}

function Expand-Package([string]$zipPath, [string]$into) {
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	# named after the zip, which names the version
	$out = Join-Path $into ([IO.Path]::GetFileNameWithoutExtension($zipPath))
	if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
	Write-Host 'Unzipping...'
	[IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $out)
	$root = Resolve-Install $out
	if (-not (Test-Install $root)) { throw "$zipPath is not an OBS-Spectra zip (no $Exe in it). Nothing changed." }
	return $root
}

# --- what to update, and with what ----------------------------------------------------------------

$hereIsInstall = (Test-Install $Here) -and (Test-Path -LiteralPath (Join-Path $Here 'config'))
$hereIsPackage = (Test-Install $Here) -and -not $hereIsInstall
$cleanup = $null

if (-not $Target) {
	if ($hereIsInstall) {
		$Target = $Here
	} elseif ($hereIsPackage -and -not $Channel -and -not $Zip) {
		$candidates = Find-Candidates $Here
		if ($candidates.Count -eq 1) {
			$Target = $candidates[0]
		} elseif ($candidates.Count -gt 1) {
			Write-Host 'Portable OBS-Spectra folders found:'
			$Target = $candidates[(Choose 'Which one to update? (number)' ($candidates | ForEach-Object { "$_  ($(Get-Version $_))" }))]
		}
	}
	if (-not $Target) {
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

if ($Shortcuts) {
	$made = Set-Shortcuts $Target
	Write-Host "Shortcuts in $Target`: $($made -join ', ')"
	exit 0
}

$running = Get-Running $Target
if ($running.Count) {
	$names = ($running | ForEach-Object { "$($_.ProcessName) ($($_.Id))" }) -join ', '
	throw "Close OBS-Spectra first: $names is running from $Target. Nothing changed."
}
$current = Get-Version $Target

if ($Zip) {
	$Zip = $Zip.Trim('"', ' ')
	if (Test-Path -LiteralPath $Zip -PathType Container) {
		$Source = Resolve-Install (Resolve-Path -LiteralPath $Zip).Path
	} else {
		$dir = Join-Path ([IO.Path]::GetTempPath()) 'obs-spectra-update\zip'
		New-Item -ItemType Directory -Path $dir -Force | Out-Null
		$Source = Expand-Package (Resolve-Path -LiteralPath $Zip).Path $dir
		$cleanup = $dir
	}
} elseif (-not $Channel -and $hereIsPackage -and (Resolve-Path -LiteralPath $Here).Path -ne $Target) {
	$Source = $Here
} else {
	Write-Host "Checking GitHub for new versions of OBS-Spectra ($Repo)..."
	$releases = Get-Releases
	$stable = Select-Release $releases 'release'
	$newest = Select-Release $releases 'rc'
	if (-not $newest) { throw 'No OBS-Spectra release with a portable zip was found on GitHub.' }
	if (-not $Channel) {
		Write-Host ''
		Write-Host "This folder has $current. Update it to:"
		$labels = @()
		$channels = @()
		if ($stable) { $labels += "the latest release, $($stable.tag_name)"; $channels += 'release' }
		if ($newest -and (-not $stable -or $newest.tag_name -ne $stable.tag_name)) {
			$labels += "the latest release candidate, $($newest.tag_name) (newer, less tested)"; $channels += 'rc'
		}
		$Channel = $channels[(Choose 'Which one? (number)' $labels)]
	}
	$release = Select-Release $releases $Channel
	if (-not $release) { throw "There is no $Channel release with a portable zip on GitHub." }
	if ($release.tag_name -eq $current -and -not $Force) {
		Write-Host "$Target already has $current. Nothing to do (use -Force to reinstall it)."
		exit 0
	}
	$Source = Get-Release $release
	$cleanup = Join-Path ([IO.Path]::GetTempPath()) "obs-spectra-update\$($release.tag_name)"
}

if ((Resolve-Path -LiteralPath $Source).Path -eq $Target) {
	throw 'That is the folder being updated. Pick another folder, or use -Channel to update it from GitHub.'
}
$newVersion = Get-Version $Source
if ($Channel) { $newVersion = $release.tag_name }

# --- confirm --------------------------------------------------------------------------------------

$portable = Test-Path -LiteralPath (Join-Path $Target 'portable_mode.txt')
$config = Join-Path $Target 'config'
$configSize = 0
if (Test-Path -LiteralPath $config) {
	$configSize = (Get-ChildItem -LiteralPath $config -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
}
Write-Host ''
Write-Host "Update  $Target"
Write-Host "  from  $current"
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

# Versions from before the updater shipped don't have it: keep it in the folder for next time
if (-not (Test-Path -LiteralPath (Join-Path $Target 'Update-Portable.ps1'))) {
	[IO.File]::WriteAllText((Join-Path $Target 'Update-Portable.ps1'), $SelfScript, (New-Object Text.UTF8Encoding $true))
	[IO.File]::WriteAllText((Join-Path $Target 'Update-Portable.cmd'), $SelfCmd, [Text.Encoding]::ASCII)
}
[IO.File]::WriteAllText((Join-Path $Target $VersionFile), "$newVersion`r`n", [Text.Encoding]::ASCII)
$made = Set-Shortcuts $Target
if ($cleanup -and (Test-Path -LiteralPath $cleanup)) {
	Remove-Item -LiteralPath $cleanup -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "Updated to $newVersion. The folder keeps its name."
if ($made) { Write-Host "Shortcuts in the folder: $($made -join ', ')." }
$exePath = Join-Path $Target $Exe
if (-not $Yes -and (Confirm 'Start OBS-Spectra now?')) {
	Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath -Parent)
}
