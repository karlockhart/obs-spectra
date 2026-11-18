[CmdletBinding()]
param(
    # The build artifact: a zip of the install tree, or the tree itself
    [Parameter(Mandatory)]
    [string] $Build,
    [Parameter(Mandatory)]
    [ValidateSet('Full', 'Lite', 'Ultralight')]
    [string] $Edition,
    # The release tag, e.g. 33.0.1-spectra.4 or 33.0.1-spectra.4-rc.1
    [Parameter(Mandatory)]
    [string] $Version,
    [string] $OutputDir = (Get-Location).Path,
    # Sign the binaries, the installer and its uninstaller (Sign-Spectra.ps1)
    [switch] $Sign,
    # vc_redist.x64.exe to bundle; the installer runs it when the runtime is missing or old
    [string] $Redist,
    [string] $Iscc
)

<#
.SYNOPSIS
Makes one OBS-Spectra edition's release files from a build.

.DESCRIPTION
From the build's install tree (debug symbols and Spectra's test programs
left out; the obs-*-test encoder probes stay, OBS runs them at startup):

  OBS-Spectra-<tag>-Windows-x64[-Lite|-Ultralight].zip            installed-style zip
  OBS-Spectra-<tag>-Windows-x64[-Lite|-Ultralight]-Portable.zip   with portable_mode.txt
  OBS-Spectra-<tag>-Windows-x64[-Lite|-Ultralight]-Setup.exe      the installer

No edition's names may end like another's (see SpectraReleases.cpp): each
edition's updater takes only its own files.
#>

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$suffix = @{ Full = ''; Lite = '-Lite'; Ultralight = '-Ultralight' }[$Edition]
$name = "OBS-Spectra-${Version}-Windows-x64${suffix}"
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path

# 33.0.1-spectra.4-rc.1 -> 33.0.1.4 (Windows file versions are four numbers)
if ($Version -match '^(\d+)\.(\d+)\.(\d+)(?:-spectra\.(\d+))?') {
    $numeric = '{0}.{1}.{2}.{3}' -f $Matches[1], $Matches[2], $Matches[3], $(if ($Matches[4]) { $Matches[4] } else { 0 })
} else {
    throw "$Version is not a release tag"
}

$work = Join-Path ([IO.Path]::GetTempPath()) "spectra-package-$Edition"
if (Test-Path $work) {
    Remove-Item -Recurse -Force $work
}
$tree = Join-Path $work 'tree'
New-Item -ItemType Directory -Force -Path $tree | Out-Null

Write-Host "::group::Unpack $Edition"
if (Test-Path -LiteralPath $Build -PathType Container) {
    Copy-Item -Path (Join-Path $Build '*') -Destination $tree -Recurse
} else {
    & 7z x -y "-o$tree" $Build | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z could not extract $Build" }
}
if (-not (Test-Path (Join-Path $tree 'bin\64bit\obs-spectra.exe'))) {
    throw "$Build is not an OBS-Spectra build (no bin\64bit\obs-spectra.exe)"
}
Get-ChildItem -Path $tree -Recurse -File -Include *.pdb, *-tests.exe | Remove-Item -Force
Get-ChildItem -Path (Join-Path $tree 'data') -Directory -Filter '*-tests' -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
Get-ChildItem -Path (Join-Path $tree '*') -File -Include portable_mode*, obs_portable_mode* | Remove-Item -Force

# The release this is. A build from a pull request compiled in a development
# version (33.0.1-3-g3588d94); OBS-Spectra reports the stamp instead when it
# is a release of the same OBS version (SpectraReleases::StampedVersion), so
# the build must be of the tag's OBS version.
$tagBase = ($Version -split '-')[0]
if ((Split-Path -Leaf $Build) -match '^obs-studio-(?<built>.+)-windows-x64\.zip$') {
    $builtBase = ($Matches['built'] -split '-')[0]
    if ($builtBase -ne $tagBase) {
        throw "The build is of OBS $builtBase, but $Version is a release of OBS $tagBase"
    }
}
Set-Content -Path (Join-Path $tree 'bin\64bit\obs-spectra-release.txt') -Value $Version -NoNewline -Encoding ascii
Write-Host '::endgroup::'

# Signing that can't be done (the release's signature budget, the account's
# quota, the service down) leaves this edition unsigned, and the release notes
# say so (UNSIGNED-<edition>.txt), unless SPECTRA_SIGNING_FALLBACK is "fail"
$signCommand = $null
function Release-Unsigned([string] $why) {
    if ($env:SPECTRA_SIGNING_FALLBACK -eq 'fail') {
        throw $why
    }
    Write-Host "::warning::$Edition is released unsigned: $why"
    Set-Content -Path (Join-Path $OutputDir "UNSIGNED-$Edition.txt") -Value $why -Encoding utf8
}
if ($Sign) {
    Write-Host "::group::Sign $Edition"
    try {
        & (Join-Path $PSScriptRoot 'Sign-Spectra.ps1') -Path $tree
        $signCommand = & (Join-Path $PSScriptRoot 'Sign-Spectra.ps1') -Path $tree -InnoCommand
    } catch {
        if ("$_" -notlike 'SPECTRA-SIGNING-UNAVAILABLE*') {
            throw
        }
        Release-Unsigned "$_"
    }
    Write-Host '::endgroup::'
}

Write-Host "::group::Zip $Edition"
foreach ($zip in "$name.zip", "$name-Portable.zip") {
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $OutputDir $zip)
}
# Compressed once, on every core: the portable zip is the same zip with
# portable_mode.txt added, which 7-Zip appends without compressing the rest
# again (maximum Deflate twice over took most of a release's time)
Push-Location $tree
try {
    & 7z a -tzip -mx=5 -mmt=on (Join-Path $OutputDir "$name.zip") '.\*' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '7z failed' }
} finally {
    Pop-Location
}
Copy-Item (Join-Path $OutputDir "$name.zip") (Join-Path $OutputDir "$name-Portable.zip")
$marker = Join-Path $work 'portable'
New-Item -ItemType Directory -Force -Path $marker | Out-Null
# OBS-Spectra updates itself in place with bin/64bit/spectra-updater.exe
New-Item -ItemType File -Path (Join-Path $marker 'portable_mode.txt') | Out-Null
Push-Location $marker
try {
    & 7z a -tzip (Join-Path $OutputDir "$name-Portable.zip") 'portable_mode.txt' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '7z failed' }
} finally {
    Pop-Location
}
Write-Host '::endgroup::'

Write-Host "::group::Installer $Edition"
if (-not $Iscc) {
    $Iscc = @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $Iscc) {
    throw 'ISCC.exe (Inno Setup 6) was not found'
}
$isccArgs = @(
    '/Qp',
    "/DSourceDir=$tree",
    "/DVersion=$Version",
    "/DNumericVersion=$numeric",
    "/DEdition=$Edition",
    "/DOutputDir=$OutputDir"
)
if ($Redist) {
    $redistSignature = Get-AuthenticodeSignature -FilePath $Redist
    if ($redistSignature.Status -ne 'Valid' -or $redistSignature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "$Redist is not signed by Microsoft"
    }
    $isccArgs += "/DRedist=$((Resolve-Path $Redist).Path)"
}
$script = Join-Path $repo 'frontend\spectra-installer\OBS-Spectra.iss'
$budgetLeft = $true
if ($signCommand -and $env:SPECTRA_SIGNING_COUNT_FILE -and (Test-Path $env:SPECTRA_SIGNING_COUNT_FILE)) {
    $max = if ($env:SPECTRA_SIGNING_MAX) { [int]$env:SPECTRA_SIGNING_MAX } else { 1000 }
    # the installer and its uninstaller
    $budgetLeft = [int](Get-Content $env:SPECTRA_SIGNING_COUNT_FILE -Raw) + 2 -le $max
}
if ($signCommand -and -not $budgetLeft) {
    Release-Unsigned 'the installer would go over SPECTRA_SIGNING_MAX'
    $signCommand = $null
}
if ($signCommand) {
    & $Iscc @isccArgs '/DSign' "/Sspectra=$signCommand" $script
    if ($LASTEXITCODE -eq 0) {
        if ($env:SPECTRA_SIGNING_COUNT_FILE) {
            $count = if (Test-Path $env:SPECTRA_SIGNING_COUNT_FILE) { [int](Get-Content $env:SPECTRA_SIGNING_COUNT_FILE -Raw) } else { 0 }
            Set-Content -Path $env:SPECTRA_SIGNING_COUNT_FILE -Value ($count + 2) -NoNewline
        }
    } else {
        # most likely the signing (Inno Setup already retried it): build it unsigned
        Release-Unsigned "signing the installer failed (ISCC $LASTEXITCODE)"
        $signCommand = $null
    }
}
if (-not $signCommand) {
    & $Iscc @isccArgs $script
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
}
Write-Host '::endgroup::'

Remove-Item -Recurse -Force $work
Get-ChildItem -Path $OutputDir -Filter "$name*" | ForEach-Object { Write-Host "$($_.Name): $([Math]::Round($_.Length / 1MB, 1)) MB" }
