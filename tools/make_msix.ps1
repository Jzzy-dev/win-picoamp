# make_msix.ps1 - PicoAmp.exe into an MSIX package for the Microsoft Store.
#
#   pwsh tools/make_msix.ps1 -Version 0.2.0.0 `
#        -Name "12345Jzzy.PicoAmp" -Publisher "CN=1A2B3C4D-..." -PublisherName "JZZY"
#
# Name, Publisher and PublisherName are the values Partner Center shows for
# the app (Product management > Product identity): Package/Identity/Name,
# Package/Identity/Publisher and Package/Properties/PublisherDisplayName.
# Without them the package gets test values: fine for trying it, not for the
# Store. The Store signs the package itself: upload it unsigned.
#
# -TestSign: also sign it with a self-made certificate and install it here
# (needs an administrator: the certificate goes into Trusted People), to try
# the packaged app on this PC.
param(
    [string]$Exe = "build\PicoAmp.exe",
    [string]$Version = "0.2.0.0",
    [string]$Name = "Jzzy.PicoAmp.Test",
    [string]$Publisher = "CN=PicoAmp Test",
    [string]$PublisherName = "JZZY & Claude",
    [string]$Out = "build\PicoAmp.msix",
    [switch]$TestSign
)
$ErrorActionPreference = "Stop"

# the SDK's tools: the newest makeappx, makepri and signtool
function Find-Tool($n) {
    $t = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\$n" -ErrorAction SilentlyContinue |
         Sort-Object FullName | Select-Object -Last 1
    if (-not $t) { throw "$n not found: install the Windows 10/11 SDK" }
    return $t.FullName
}
$makeappx = Find-Tool "makeappx.exe"
$makepri = Find-Tool "makepri.exe"

# a version for the Store: four numbers, the last 0
if ($Version -notmatch '^\d+\.\d+\.\d+\.0$') { throw "Version must look like 1.2.3.0 (the Store wants the last number 0)" }

$stage = "build\msix"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $Exe "$stage\PicoAmp.exe"
Copy-Item -Recurse "msix\Assets" "$stage\Assets"
Copy-Item README.md, LICENSE.txt $stage
Copy-Item third_party\cbios\LICENSE "$stage\LICENSE-C-BIOS-font.txt"
Copy-Item third_party\miniaudio\LICENSE "$stage\LICENSE-miniaudio.txt"

$esc = { param($s) [System.Security.SecurityElement]::Escape($s) }
(Get-Content -Raw "msix\AppxManifest.xml.in") `
    -replace '@NAME@', (& $esc $Name) `
    -replace '@PUBLISHER@', (& $esc $Publisher) `
    -replace '@PUBLISHER_NAME@', (& $esc $PublisherName) `
    -replace '@VERSION@', $Version |
    Set-Content -Encoding utf8 "$stage\AppxManifest.xml"

# the icons' sizes (targetsize-16 ...) indexed in resources.pri
& $makepri createconfig /cf "build\priconfig.xml" /dq en-US /o | Out-Null
& $makepri new /pr $stage /cf "build\priconfig.xml" /mn "$stage\AppxManifest.xml" /of "$stage\resources.pri" /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri failed" }

Remove-Item $Out -ErrorAction SilentlyContinue
& $makeappx pack /d $stage /p $Out /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }
Write-Host "Package: $Out ($Name $Version, publisher $Publisher)"

if ($TestSign) {
    $signtool = Find-Tool "signtool.exe"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
        -FriendlyName "PicoAmp test signing" -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
    $pw = ConvertTo-SecureString -String "picoamp" -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath "build\test-sign.pfx" -Password $pw | Out-Null
    Export-Certificate -Cert $cert -FilePath "build\test-sign.cer" | Out-Null
    Import-Certificate -FilePath "build\test-sign.cer" -CertStoreLocation "Cert:\LocalMachine\TrustedPeople" | Out-Null
    & $signtool sign /fd SHA256 /f "build\test-sign.pfx" /p picoamp $Out | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
    Add-AppxPackage -Path $Out
    Write-Host "Installed for testing: $((Get-AppxPackage -Name $Name).PackageFullName)"
}
