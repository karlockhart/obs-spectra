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
    [switch] $InnoCommand
)

<#
.SYNOPSIS
Signs OBS-Spectra's binaries with the Azure Artifact Signing certificate.

.DESCRIPTION
Signs every .exe and .dll under the given paths that has no valid Authenticode
signature yet (third-party files Microsoft or others signed keep their own),
with SHA-256 and an RFC 3161 timestamp, then checks each one.

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
$BatchSize = 64

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
        Get-ChildItem -LiteralPath $item -Recurse -File -Include *.exe, *.dll
    } elseif (Test-Path -LiteralPath $item -PathType Leaf) {
        Get-Item -LiteralPath $item
    } else {
        throw "$item does not exist"
    }
}
$toSign = @($files | Where-Object {
        $All -or (Get-AuthenticodeSignature -FilePath $_.FullName).Status -ne 'Valid'
    } | ForEach-Object { $_.FullName })

Write-Host "Signing $($toSign.Count) of $(@($files).Count) files with $AccountName/$ProfileName"
for ($i = 0; $i -lt $toSign.Count; $i += $BatchSize) {
    $batch = $toSign[$i..([Math]::Min($i + $BatchSize, $toSign.Count) - 1)]
    & $signTool @signArgs @batch
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed ($LASTEXITCODE)"
    }
}

$bad = @($toSign | Where-Object { (Get-AuthenticodeSignature -FilePath $_).Status -ne 'Valid' })
if ($bad.Count -gt 0) {
    throw "These files are not validly signed: $($bad -join ', ')"
}
Write-Host "Signed and verified $($toSign.Count) files"
