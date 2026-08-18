# Signs one file with Azure Artifact Signing (formerly Trusted Signing).
#
#   powershell -File packaging\sign.ps1 <file-to-sign>
#
# Wired into package.bat by setting, before you run it:
#
#     set MACRODECK_SIGN_CMD=powershell -File packaging\sign.ps1
#
# package.bat then calls this once per file it produces (the exe, then the
# installer), passing the file as the one argument. Anything non-zero coming
# back stops packaging, so a failed signature can never end up in a release.
#
# A plain .pfx does NOT need this script - package.bat signs those itself via
# MACRODECK_SIGN_PFX. This exists for the cloud service, where the private key
# never comes near this machine.
#
# NOTE: a near-identical copy lives in the Dental Flow project. Nothing links
# them, so a fix here needs applying there too.
#
# ---------------------------------------------------------------------------
# One-time setup
# ---------------------------------------------------------------------------
#
# 1. Create an Artifact Signing account in the Azure portal and complete
#    identity validation. There is an individual-developer path if you are not
#    signing as a registered company; it has a verifiable-history requirement,
#    so confirm you qualify before paying for anything.
#
# 2. Install the signing client, which supplies the dlib signtool loads. It is
#    a NuGet PACKAGE, not a dotnet tool - "dotnet tool install" will not find
#    it. Since the service was rebranded the package is called
#    Microsoft.ArtifactSigning.Client (it was Microsoft.Trusted.Signing.Client).
#    Before signing, install the matching .NET 8.0 Runtime on the clean signing
#    host, because the Azure.CodeSigning.Dlib.dll and signtool.exe both depend on
#    it at runtime. The runtime architecture must match the selected
#    signtool.exe architecture (for example, x64 signtool requires the x64 .NET
#    8 runtime; arm64 signtool requires the arm64 runtime). If you are setting
#    up a new or clean signing host, install or verify that prerequisite before
#    attempting to sign. Download and unpack the client package - a .nupkg is a
#    .zip:
#
#        $v    = '1.0.128'
#        $url  = "https://api.nuget.org/v3-flatcontainer/microsoft.artifactsigning.client/$v/microsoft.artifactsigning.client.$v.nupkg"
#        $dest = "$env:LOCALAPPDATA\ArtifactSigningClient\$v"
#        Invoke-WebRequest $url -OutFile "$env:TEMP\as.zip"
#        Expand-Archive "$env:TEMP\as.zip" -DestinationPath $dest -Force
#
#    That is one of the places this script looks. Anywhere else is fine too -
#    just point TRUSTED_SIGNING_DLIB at the Azure.CodeSigning.Dlib.dll inside.
#
# 3. Tell this script about the account:
#
#        setx TRUSTED_SIGNING_ENDPOINT https://eus.codesigning.azure.net
#        setx TRUSTED_SIGNING_ACCOUNT  <your account name>
#        setx TRUSTED_SIGNING_PROFILE  <your certificate profile name>
#
# 4. Give it credentials. Either sign in interactively once with `az login`,
#    or set a service principal for unattended use:
#
#        setx AZURE_TENANT_ID     <guid>
#        setx AZURE_CLIENT_ID     <guid>
#        setx AZURE_CLIENT_SECRET <secret>
#
#    Do not put the secret in this file - it would end up in the source zip
#    and on GitHub.
#
# Signing does not make SmartScreen quiet on day one. What it buys is a named
# publisher and reputation that carries from one release to the next, instead
# of every rebuild starting from zero.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Path
)

$ErrorActionPreference = 'Stop'

function Fail($message) {
    Write-Host "  sign.ps1: $message" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $Path)) { Fail "no such file: $Path" }
$target = (Resolve-Path -LiteralPath $Path).Path

# ---- configuration --------------------------------------------------------
$endpoint = $env:TRUSTED_SIGNING_ENDPOINT
$account  = $env:TRUSTED_SIGNING_ACCOUNT
$profile  = $env:TRUSTED_SIGNING_PROFILE

if (-not $endpoint -or -not $account -or -not $profile) {
    Fail ("TRUSTED_SIGNING_ENDPOINT, TRUSTED_SIGNING_ACCOUNT and " +
          "TRUSTED_SIGNING_PROFILE must all be set. See the top of this file.")
}

# ---- signtool -------------------------------------------------------------
# Prefer one already on PATH; otherwise take the newest x64 build out of the
# Windows SDK. Sorting descending by name puts the highest SDK version first.
$signtool = (Get-Command signtool.exe -ErrorAction SilentlyContinue).Source
if (-not $signtool) {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path $kits) {
        $signtool = Get-ChildItem -Path $kits -Recurse -Filter signtool.exe `
                        -ErrorAction SilentlyContinue |
                    Where-Object { $_.FullName -match '\\x64\\' } |
                    Sort-Object FullName -Descending |
                    Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $signtool) {
    Fail ("signtool.exe not found. Install the Windows SDK " +
          "(winget install Microsoft.WindowsSDK).")
}

# ---- the dlib that teaches signtool to talk to the service ---------------
# The file name has survived the rebrand even though the package name did not,
# so it is still Azure.CodeSigning.Dlib.dll inside Microsoft.ArtifactSigning.Client.
$dlib = $env:TRUSTED_SIGNING_DLIB
if (-not $dlib) {
    $candidates = @(
        # Where the unpack in this file's header puts it.
        (Join-Path $env:LOCALAPPDATA 'ArtifactSigningClient'),
        # Where NuGet puts it if the package was restored by a project instead.
        (Join-Path $env:USERPROFILE '.nuget\packages\microsoft.artifactsigning.client'),
        # And the pre-rebrand package, for a machine that already had it.
        (Join-Path $env:USERPROFILE '.nuget\packages\microsoft.trusted.signing.client'),
        (Join-Path ${env:ProgramFiles} 'Microsoft.Trusted.Signing.Client')
    ) | Where-Object { Test-Path $_ }

    foreach ($root in $candidates) {
        # Sorted descending so the newest version wins when several are present.
        $found = Get-ChildItem -Path $root -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' `
                     -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -match '\\x64\\' } |
                 Sort-Object FullName -Descending |
                 Select-Object -First 1 -ExpandProperty FullName
        if ($found) { $dlib = $found; break }
    }
}
if (-not $dlib -or -not (Test-Path -LiteralPath $dlib)) {
    Fail ("Azure.CodeSigning.Dlib.dll not found. It comes from the " +
          "Microsoft.ArtifactSigning.Client NuGet package - see the top of " +
          "this file for how to unpack it - or set TRUSTED_SIGNING_DLIB to " +
          "its full path. Note it is a NuGet package, not a dotnet tool: " +
          "'dotnet tool install' will not find it.")
}

# ---- the metadata file signtool hands to the dlib ------------------------
# Written fresh each run into the temp folder: it is derived entirely from the
# environment, so there is nothing worth keeping and nothing worth committing.
$metadata = Join-Path ([System.IO.Path]::GetTempPath()) `
                      ("trusted-signing-{0}.json" -f [guid]::NewGuid())
$metadataJson = @{
    Endpoint               = $endpoint
    CodeSigningAccountName = $account
    CertificateProfileName = $profile
} | ConvertTo-Json

# Written with .NET rather than Set-Content -Encoding UTF8, which in Windows
# PowerShell prepends a byte-order mark. The signing library parses this file
# with System.Text.Json, which rejects a BOM outright - the error is
# "'0xEF' is an invalid start of a value", nothing about encodings, so it is
# a thoroughly confusing way to fail. $false = no BOM.
[System.IO.File]::WriteAllText($metadata, $metadataJson,
                               (New-Object System.Text.UTF8Encoding $false))

try {
    # /tr timestamps the signature so it stays valid after the short-lived
    # certificate behind this service has expired - without it the signature
    # dies within days.
    & $signtool sign `
        /v `
        /fd SHA256 `
        /td SHA256 `
        /tr 'http://timestamp.acs.microsoft.com' `
        /dlib $dlib `
        /dmdf $metadata `
        $target
    if ($LASTEXITCODE -ne 0) { Fail "signtool exited with $LASTEXITCODE" }

    & $signtool verify /pa /v $target | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Fail "the file was signed but did not verify - refusing to ship it"
    }
    Write-Host "  signed and verified: $(Split-Path -Leaf $target)"
}
finally {
    Remove-Item -LiteralPath $metadata -Force -ErrorAction SilentlyContinue
}

exit 0
