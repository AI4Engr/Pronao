#!/usr/bin/env bash
# The signing scripts (scripts/sign.ps1, scripts/check_signature.ps1) with a throwaway certificate that is made in memory and written to a file (no certificate store is
# touched). That tests the mechanics: an exe is signed, the signature says who signed it and is still good after the program starts, a changed file is reported as changed,
# a wrong password and a missing file are refused, and the password never needs to be on a command line.
# What it cannot test: that Windows TRUSTS a real authority's certificate (a self-signed one is reported as signed but not trusted, which is the right answer here), SmartScreen,
# and a real timestamp server (it needs the network and a certificate that chains to a root).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="${TMPDIR:-/tmp}/pronao-sign"
rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/run"
W() { cygpath -w "$1"; }
ps() { env -u PSModulePath powershell -NoProfile -ExecutionPolicy Bypass -File "$(W "$HERE/scripts/$1")" "${@:2}" 2>&1 | tr -d '\r'; }

# an exe to sign: the small program of the distribution test
ASSETS="$HERE/tests/site_placement" WORK="$S/work" RELEASE=1 STRIP=1 APP_VERSION=1.0.0 APP_NAME="Signing Test" bash "$HERE/scripts/build.sh" "$HERE/tests/runtime_window.clg" "$S/app.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; exit 1; }
cp "$S/app.exe" "$S/unsigned.exe"

# a throwaway code-signing certificate, in memory, to a .pfx
export SIGN_TEST_PASSWORD="correct horse battery"
env -u PSModulePath powershell -NoProfile -Command "
\$rsa = [System.Security.Cryptography.RSA]::Create(2048)
\$req = New-Object System.Security.Cryptography.X509Certificates.CertificateRequest('CN=pronao signing test', \$rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
\$oids = New-Object System.Security.Cryptography.OidCollection
[void]\$oids.Add((New-Object System.Security.Cryptography.Oid('1.3.6.1.5.5.7.3.3')))
\$req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension(\$oids, \$false)))
\$cert = \$req.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddDays(1))
[System.IO.File]::WriteAllBytes('$(W "$S/test.pfx")', \$cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, \$env:SIGN_TEST_PASSWORD))
" || { echo "could not make the test certificate"; exit 1; }
[ -s "$S/test.pfx" ] || { echo "no test certificate"; exit 1; }

# 1: unsigned
r="$(ps check_signature.ps1 -File "$(W "$S/unsigned.exe")")"; echo "unsigned: $r"
[ "${r%%|*}" = "NotSigned" ] || { echo "an unsigned exe is not reported as unsigned"; exit 1; }

# 2: signed with the password from the environment
out="$(ps sign.ps1 -File "$(W "$S/app.exe")" -Pfx "$(W "$S/test.pfx")" -PasswordEnv SIGN_TEST_PASSWORD)"; rc=$?
echo "$out"
r="$(ps check_signature.ps1 -File "$(W "$S/app.exe")")"; echo "signed: $r"
case "$r" in *"|CN=pronao signing test|"*) ;; *) echo "the signature does not name the certificate"; exit 1 ;; esac
case "${r%%|*}" in Valid|UnknownError|NotTrusted) ;; *) echo "unexpected status ${r%%|*}"; exit 1 ;; esac
echo "$out" | grep -q "^signed by: CN=pronao signing test" || { echo "sign.ps1 did not say who signed"; exit 1; }

# 3: a signed program still runs
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 -lgdi32 -lshell32 -lole32 -luuid -ladvapi32 > /dev/null 2>&1 || { echo "POKE BUILD FAILED"; exit 1; }
(cd "$S/run" && timeout 60 "$S/app.exe"; echo $? > "$S/app.rc") &
pid=$!
"$S/poke.exe" wait "runtime window" || { kill "$pid" 2> /dev/null; echo "the signed program did not open its window"; exit 1; }
"$S/poke.exe" close "runtime window"; wait "$pid"
[ "$(cat "$S/app.rc")" = 0 ] || { echo "the signed program ended with $(cat "$S/app.rc")"; exit 1; }
echo "the signed program runs"

# 4: a changed file is reported as changed
cp "$S/app.exe" "$S/tampered.exe"
python3 - "$S/tampered.exe" <<'PYEOF'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
b[0x600] ^= 0xFF                       # inside the first section: part of what the hash covers
open(p, 'wb').write(b)
PYEOF
r="$(ps check_signature.ps1 -File "$(W "$S/tampered.exe")")"; echo "changed after signing: $r"
[ "${r%%|*}" = "HashMismatch" ] || { echo "a changed file is not reported as changed"; exit 1; }

# 5: refusals
if out="$(SIGN_TEST_PASSWORD=wrong ps sign.ps1 -File "$(W "$S/unsigned.exe")" -Pfx "$(W "$S/test.pfx")" -PasswordEnv SIGN_TEST_PASSWORD)"; then echo "a wrong password was accepted"; exit 1; fi
r="$(ps check_signature.ps1 -File "$(W "$S/unsigned.exe")")"
[ "${r%%|*}" = "NotSigned" ] || { echo "a failed signing left a signature"; exit 1; }
if ps sign.ps1 -File "$(W "$S/nothing.exe")" -Pfx "$(W "$S/test.pfx")" -PasswordEnv SIGN_TEST_PASSWORD > /dev/null; then echo "a missing file was accepted"; exit 1; fi
if ps sign.ps1 -File "$(W "$S/unsigned.exe")" > /dev/null; then echo "no certificate was accepted"; exit 1; fi
if ps sign.ps1 -File "$(W "$S/unsigned.exe")" -Pfx "$(W "$S/test.pfx")" -PasswordEnv NO_SUCH_VARIABLE > /dev/null; then echo "an empty password variable was accepted"; exit 1; fi
echo "signing test: ok"
