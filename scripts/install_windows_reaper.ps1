param(
    [string]$Version = '7.78',
    [string]$InstallerUrl = 'https://www.reaper.fm/files/7.x/reaper778_x64-install.exe',
    [string]$PortableRoot = 'C:\profligacy-tools\reaper-7.78-portable',
    [string]$Receipt = 'C:\profligacy-tools\reaper-7.78-portable-receipt.json'
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $PortableRoot) { throw "Refusing existing path: $PortableRoot" }
$installer = Join-Path $env:TEMP "reaper-$Version-x64-install.exe"
Invoke-WebRequest -Uri $InstallerUrl -OutFile $installer
$signature = Get-AuthenticodeSignature -LiteralPath $installer
if ($signature.Status -ne 'Valid') { throw "Invalid installer signature: $($signature.Status)" }
New-Item -ItemType Directory -Path (Split-Path -Parent $PortableRoot) -Force | Out-Null
$process = Start-Process -FilePath $installer -ArgumentList @(
    '/S', '/PORTABLE', "/D=$PortableRoot"
) -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "REAPER installer exit $($process.ExitCode)" }
$exe = Join-Path $PortableRoot 'reaper.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing $exe" }
$exeSignature = Get-AuthenticodeSignature -LiteralPath $exe
$body = [ordered]@{
    schema = 'profligacy-windows-reaper-tool-v1'
    created_utc = [DateTime]::UtcNow.ToString('o')
    version = $Version
    source_url = $InstallerUrl
    installer_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash
    installer_signature = $signature.Status.ToString()
    installer_signer = $signature.SignerCertificate.Subject
    portable_root = $PortableRoot
    executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
    executable_signature = $exeSignature.Status.ToString()
    executable_signer = $exeSignature.SignerCertificate.Subject
}
$body | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -LiteralPath $Receipt
$body | ConvertTo-Json -Depth 6
