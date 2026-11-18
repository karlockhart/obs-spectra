[CmdletBinding()]
param(
    # Files, or folders to search for .exe and .dll files
    [Parameter(Mandatory)]
    [string[]] $Path,
    # Azure Artifact Signing (Trusted Signing) account
    [string] $Endpoint = $(if ($env:SPECTRA_SIGNING_ENDPOINT) { $env:SPECTRA_SIGNING_ENDPOINT } else { 'https://wus2.codesigning.azure.net/' }),
    [string] $AccountName = $(if ($env:SPECTRA_SIGNING_ACCOUNT) { $env:SPECTRA_SIGNING_ACCOUNT } else { 'karlockhart' }),
    [string] $ProfileName = $(if ($env:SPECTRA_SIGNING_PROFILE) { $env:SPECTRA_SIGNING_PROFILE } else { 'karlockhartpublic' }),
    # Sign files that already carry a valid signature too (Microsoft's, say)
    [switch] $All,
    # Print the signtool command Inno Setup's SignTool= needs, and sign nothing
    [switch] $InnoCommand,
    # Signed copies by the unsigned file's SHA-256: a file is signed once, and
    # the same file again (another edition's copy) takes the signed copy
    [string] $Cache = $env:SPECTRA_SIGNING_CACHE,
    # The most signatures one release may use, counted in -CountFile across
    # every call; past it, signing stops (Basic accounts include 5,000 a month
    # and bill each one after that)
    [int] $MaxSignatures = $(if ($env:SPECTRA_SIGNING_MAX) { [int]$env:SPECTRA_SIGNING_MAX } else { 1000 }),
    [string] $CountFile = $env:SPECTRA_SIGNING_COUNT_FILE,
    # Batches signed at the same time (PowerShell 7; one at a time on 5.1)
    [int] $Parallel = 4
)

<#
.SYNOPSIS
Signs OBS-Spectra's binaries with the Azure Artifact Signing certificate.

.DESCRIPTION
Signs every .exe and .dll under the given paths that has no valid Authenticode
signature yet (third-party files Microsoft or others signed keep their own),
with SHA-256 and an RFC 3161 timestamp, then checks each one.

When signing can't be done (the -MaxSignatures budget, a refused quota, the
service down after retries), the error starts with SPECTRA-SIGNING-UNAVAILABLE,
which Package-Spectra-Windows.ps1 turns into an unsigned release unless
SPECTRA_SIGNING_FALLBACK is "fail".

Signing goes through signtool and Microsoft's Artifact Signing dlib, which
signs in Azure: no key or certificate is ever on this machine. The dlib signs
in as whoever the Azure CLI is signed in as (az login locally, azure/login
in CI), who needs the "Artifact Signing Certificate Profile Signer" role on
the certificate profile.

.EXAMPLE
.github/scripts/Sign-Spectra.ps1 -Path build_x64/rundir/RelWithDebInfo
#>

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Microsoft.ArtifactSigning.Client: the signtool dlib (pinned; bump on purpose)
$ClientPackage = 'Microsoft.ArtifactSigning.Client'
$ClientVersion = '1.0.151'
$TimestampUrl = 'http://timestamp.acs.microsoft.com'
$BatchSize = 16

function Find-SignTool {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } |
        Sort-Object { [version]($_.Directory.Parent.Name -replace '[^0-9.]', '0') } -Descending |
        Select-Object -First 1
    if (-not $found) {
        throw 'signtool.exe was not found: install the Windows SDK'
    }
    return $found.FullName
}

function Get-Dlib {
    $cache = Join-Path ([IO.Path]::GetTempPath()) "spectra-signing\$ClientPackage.$ClientVersion"
    $dlib = Get-ChildItem -Path $cache -Filter Azure.CodeSigning.Dlib.dll -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } | Select-Object -First 1
    if ($dlib) {
        return $dlib.FullName
    }
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $nupkg = Join-Path $cache 'client.zip'
    $url = "https://api.nuget.org/v3-flatcontainer/$($ClientPackage.ToLower())/$ClientVersion/$($ClientPackage.ToLower()).$ClientVersion.nupkg"
    Write-Host "Downloading $ClientPackage $ClientVersion"
    Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing
    Expand-Archive -Path $nupkg -DestinationPath $cache -Force
    $dlib = Get-ChildItem -Path $cache -Filter Azure.CodeSigning.Dlib.dll -Recurse |
        Where-Object { $_.Directory.Name -eq 'x64' } | Select-Object -First 1
    if (-not $dlib) {
        throw "$ClientPackage $ClientVersion has no x64 Azure.CodeSigning.Dlib.dll"
    }
    $signature = Get-AuthenticodeSignature -FilePath $dlib.FullName
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "$($dlib.FullName) is not signed by Microsoft"
    }
    return $dlib.FullName
}

function New-Metadata {
    $file = Join-Path ([IO.Path]::GetTempPath()) 'spectra-signing\metadata.json'
    New-Item -ItemType Directory -Force -Path (Split-Path $file) | Out-Null
    # Only the Azure CLI's sign-in: the other credentials only slow each file down
    @{
        Endpoint               = $Endpoint
        CodeSigningAccountName = $AccountName
        CertificateProfileName = $ProfileName
        ExcludeCredentials     = @(
            'EnvironmentCredential', 'WorkloadIdentityCredential', 'ManagedIdentityCredential',
            'SharedTokenCacheCredential', 'VisualStudioCredential', 'VisualStudioCodeCredential',
            'AzurePowerShellCredential', 'AzureDeveloperCliCredential', 'InteractiveBrowserCredential'
        )
    } | ConvertTo-Json | ForEach-Object {
        # no byte order mark, which Windows PowerShell's utf8 would add
        [IO.File]::WriteAllText($file, $_, (New-Object Text.UTF8Encoding $false))
    }
    return $file
}

$signTool = Find-SignTool
$dlib = Get-Dlib
$metadata = New-Metadata
$signArgs = @('sign', '/fd', 'SHA256', '/tr', $TimestampUrl, '/td', 'SHA256', '/dlib', $dlib, '/dmdf', $metadata)

if ($InnoCommand) {
    # Inno Setup's /S<name>=<command>: $q is a quote, $f the file to sign
    $quoted = $signArgs | ForEach-Object { if ($_ -match '[\s]') { '$q' + $_ + '$q' } else { $_ } }
    Write-Output ('$q' + $signTool + '$q ' + ($quoted -join ' ') + ' $f')
    return
}

$files = foreach ($item in $Path) {
    if (Test-Path -LiteralPath $item -PathType Container) {
        # not -Include, which Windows PowerShell ignores with -LiteralPath: it
        # would sign every file
        Get-ChildItem -LiteralPath $item -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' }
    } elseif (Test-Path -LiteralPath $item -PathType Leaf) {
        Get-Item -LiteralPath $item
    } else {
        throw "$item does not exist"
    }
}
$toSign = @($files | Where-Object {
        $All -or (Get-AuthenticodeSignature -FilePath $_.FullName).Status -ne 'Valid'
    } | ForEach-Object { $_.FullName })

function Unavailable([string] $why) {
    throw "SPECTRA-SIGNING-UNAVAILABLE: $why"
}

# The same file is signed once: from the cache, or once per call for copies
$cacheDir = $null
if ($Cache) {
    $cacheDir = Join-Path $Cache $ProfileName
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
}
$hashOf = @{}
$leaders = [ordered]@{} # hash -> the file that gets signed
$reused = 0
foreach ($file in $toSign) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant()
    $hashOf[$file] = $hash
    $cached = if ($cacheDir) { Join-Path $cacheDir ($hash + [IO.Path]::GetExtension($file)) } else { $null }
    if ($cached -and (Test-Path -LiteralPath $cached) -and (Get-AuthenticodeSignature -FilePath $cached).Status -eq 'Valid') {
        Copy-Item -LiteralPath $cached -Destination $file -Force
        $reused++
    } elseif (-not $leaders.Contains($hash)) {
        $leaders[$hash] = $file
    }
}
$unique = @($leaders.Values)

# The release's budget
$used = 0
if ($CountFile -and (Test-Path -LiteralPath $CountFile)) {
    $used = [int](Get-Content -LiteralPath $CountFile -Raw)
}
Write-Host ("Signing {0} of {1} files with {2}/{3}: {4} to sign, {5} from earlier signatures, {6} signed so far, at most {7}" -f
    $toSign.Count, @($files).Count, $AccountName, $ProfileName, $unique.Count, $reused, $used, $MaxSignatures)
if ($used + $unique.Count -gt $MaxSignatures) {
    Unavailable ("signing {0} more files would make {1} signatures in this release, over SPECTRA_SIGNING_MAX ({2})" -f
        $unique.Count, ($used + $unique.Count), $MaxSignatures)
}

# Batches, several at once; a batch that fails (throttled, the service busy)
# is tried again for whatever in it isn't signed yet, after a growing wait
$batches = @()
for ($i = 0; $i -lt $unique.Count; $i += $BatchSize) {
    $batches += , @($unique[$i..([Math]::Min($i + $BatchSize, $unique.Count) - 1)])
}
$signBatch = {
    param([string[]] $batch, [string] $tool, [string[]] $toolArgs)
    $requests = 0
    for ($attempt = 1; $attempt -le 4; $attempt++) {
        $left = @($batch | Where-Object { (Get-AuthenticodeSignature -FilePath $_).Status -ne 'Valid' })
        if ($left.Count -eq 0) {
            break
        }
        $requests += $left.Count
        $output = & $tool @toolArgs @left 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Write-Host "signtool failed ($LASTEXITCODE), attempt $attempt of 4:"
            Write-Host $output
            if ($attempt -lt 4) {
                Start-Sleep -Seconds (15 * [Math]::Pow(2, $attempt - 1))
            }
        }
    }
    [pscustomobject]@{ Requests = $requests }
}
if ($batches.Count -gt 0) {
    if ($PSVersionTable.PSVersion.Major -ge 7 -and $Parallel -gt 1) {
        $code = $signBatch.ToString()
        $results = $batches | ForEach-Object -ThrottleLimit $Parallel -Parallel {
            & ([scriptblock]::Create($using:code)) $_ $using:signTool $using:signArgs
        }
    } else {
        $results = foreach ($batch in $batches) { & $signBatch $batch $signTool $signArgs }
    }
    $requests = ($results | Measure-Object -Property Requests -Sum).Sum
    if ($CountFile) {
        Set-Content -LiteralPath $CountFile -Value ($used + $requests) -NoNewline
    }
}

# Copies take their signed file, and the cache keeps it for the next call
foreach ($file in $toSign) {
    $leader = $leaders[$hashOf[$file]]
    if ($leader -and $leader -ne $file -and (Get-AuthenticodeSignature -FilePath $leader).Status -eq 'Valid') {
        Copy-Item -LiteralPath $leader -Destination $file -Force
    }
}
if ($cacheDir) {
    foreach ($hash in $leaders.Keys) {
        $leader = $leaders[$hash]
        if ((Get-AuthenticodeSignature -FilePath $leader).Status -eq 'Valid') {
            Copy-Item -LiteralPath $leader -Destination (Join-Path $cacheDir ($hash + [IO.Path]::GetExtension($leader))) -Force
        }
    }
}

$bad = @($toSign | Where-Object { (Get-AuthenticodeSignature -FilePath $_).Status -ne 'Valid' })
if ($bad.Count -gt 0) {
    Unavailable ("{0} files could not be signed after retries, e.g. {1}" -f $bad.Count, ($bad | Select-Object -First 3) -join ', ')
}
Write-Host ("Signed and verified {0} files ({1} signed now, {2} from earlier signatures)" -f $toSign.Count, $unique.Count, ($toSign.Count - $unique.Count))
