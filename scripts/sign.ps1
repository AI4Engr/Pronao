# Signs an exe with a code-signing certificate (Windows PowerShell; needs no SDK).
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sign.ps1 -File out\my-app.exe -Pfx cert.pfx -PasswordEnv SIGN_PASSWORD [-Timestamp http://timestamp.digicert.com]
#   powershell ... -File scripts\sign.ps1 -File out\my-app.exe -Thumbprint 0123ABCD...       (a certificate that is already in the user's or the machine's store)
# The password is read from an environment variable, never from the command line (a command line is visible to other programs and ends up in logs).
# Sign LAST: any change to the exe afterwards (strip, a packer, editing resources) breaks the signature. Use -Timestamp so it stays valid after the certificate expires.
# Exit code 0 when the file now carries a signature that hashes right (whether Windows TRUSTS the certificate is another question: see docs/distribution.md).
param(
    [Parameter(Mandatory = $true)][string]$File,
    [string]$Pfx = '',
    [string]$PasswordEnv = '',
    [string]$Thumbprint = '',
    [string]$Timestamp = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $File -PathType Leaf)) { Write-Error "no such file: $File"; exit 2 }
$cert = $null
if ($Pfx -ne '') {
    if (-not (Test-Path -LiteralPath $Pfx -PathType Leaf)) { Write-Error "no such certificate file: $Pfx"; exit 2 }
    $password = $null
    if ($PasswordEnv -ne '') {
        $value = [Environment]::GetEnvironmentVariable($PasswordEnv)
        if ([string]::IsNullOrEmpty($value)) { Write-Error "the environment variable $PasswordEnv is empty or not set"; exit 2 }
        $password = ConvertTo-SecureString -String $value -AsPlainText -Force
    }
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList (Resolve-Path -LiteralPath $Pfx).Path, $password
} elseif ($Thumbprint -ne '') {
    $cert = Get-ChildItem -Path Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $Thumbprint.ToUpper() } | Select-Object -First 1
    if ($null -eq $cert) { Write-Error "no certificate with that thumbprint in the user's or the machine's store"; exit 2 }
} else {
    Write-Error 'give -Pfx (with -PasswordEnv) or -Thumbprint'; exit 2
}
if (-not $cert.HasPrivateKey) { Write-Error 'the certificate has no private key'; exit 2 }
$arguments = @{ FilePath = $File; Certificate = $cert; HashAlgorithm = 'SHA256' }
if ($Timestamp -ne '') { $arguments['TimestampServer'] = $Timestamp }
$result = Set-AuthenticodeSignature @arguments
Write-Output ("signed by: " + $cert.Subject)
Write-Output ("status: " + $result.Status)
# a certificate Windows does not trust yet (a self-signed one, or a new authority) reports UnknownError / NotTrusted but the file IS signed; a failure to sign or a bad hash is not that
if ($result.Status -eq 'HashMismatch' -or $null -eq $result.SignerCertificate) { exit 1 }
exit 0
