# Says what the signature of an exe is: one line "status|subject|thumbprint". Status is what Windows says: Valid (signed, and the certificate chains to a root it trusts),
# NotSigned, HashMismatch (the file was changed after it was signed), UnknownError / NotTrusted (signed, but by a certificate Windows does not trust: a self-signed one).
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check_signature.ps1 -File out\my-app.exe
param([Parameter(Mandatory = $true)][string]$File)
$s = Get-AuthenticodeSignature -LiteralPath $File
$subject = ''
$thumb = ''
if ($null -ne $s.SignerCertificate) { $subject = $s.SignerCertificate.Subject; $thumb = $s.SignerCertificate.Thumbprint }
Write-Output ("{0}|{1}|{2}" -f $s.Status, $subject, $thumb)
