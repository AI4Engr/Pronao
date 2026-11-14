#!/usr/bin/env bash
# The signing hook of scripts/build.sh (SIGN_COMMAND, SIGN_VERIFY_COMMAND), end to end, Windows.
#   tests/run_sign_hook.sh
# Part 1, with a stand-in for the signing tool (it only appends a marker), tests the MECHANICS: the hook runs last and the exe is the final one; the path reaches the tool as ONE argument
# whatever characters it has; a failing tool, a tool that changes nothing, and a failing verification each FAIL the build and delete the exe; a mistake in the configuration is refused
# before anything is built; the command (which may hold a key) is not printed.
# Part 2 signs for real with scripts/sign.ps1 and a throwaway self-signed certificate and checks the result with scripts/check_signature.ps1: the build ends with a signed exe.
# What it cannot test: a certificate that Windows trusts, a real timestamp server, SmartScreen (see docs/manual/10-distribution.md).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="$(cygpath -m "${TMPDIR:-/tmp}")/pronao-sign-hook"          # a C:/... path: the native tools of the build take it as it is, whatever characters follow
rm -rf "$S" 2>/dev/null || true; mkdir -p "$S"
export OBJ_CACHE="$S/cache" OPT=-O0
W() { cygpath -w "$1"; }
fail() { echo "FAIL: $1"; exit 1; }
ok() { echo "PASS $1"; }

build() { # out.exe [extra env assignments are taken from the caller] -> build.sh on the small program of the distribution test, RELEASE=1
    ASSETS="$HERE/tests/site_placement" WORK="$S/work" RELEASE=1 STRIP=1 APP_VERSION=1.0.0 APP_NAME="Sign Hook Test" bash "$HERE/scripts/build.sh" "$HERE/tests/runtime_window.clg" "$1" > "$S/build.log" 2>&1
}

# a stand-in for a signing tool: records what it was given and appends a marker to the file
cat > "$S/fake_sign.sh" <<'EOF'
#!/usr/bin/env bash
echo "$#" > "$RECORD.argc"
printf '%s' "$1" > "$RECORD.arg1"
[ -n "${FAKE_FAIL:-}" ] && exit 3
[ -n "${FAKE_NOOP:-}" ] && exit 0
printf 'SIGNED-BY-FAKE' >> "$1"
EOF
cat > "$S/fake_verify.sh" <<'EOF'
#!/usr/bin/env bash
[ -n "${VERIFY_FAIL:-}" ] && exit 4
tail -c 14 "$1" | grep -q 'SIGNED-BY-FAKE'
EOF

# ---- 1: the hook runs last, on the final exe, with the path as one argument ----------------------------------------------------------------------------------------------------------
mkdir -p "$S/odd dir"
ODD="$S/odd dir/a b&c'd\$HOME(x).exe"          # (no ";": the MSYS tools of a Windows build read it as a list separator in a path before the hook is involved)
export RECORD="$S/rec1"
if SIGN_COMMAND="bash $S/fake_sign.sh %1" SIGN_VERIFY_COMMAND="bash $S/fake_verify.sh %1" build "$ODD"; then ok "a build with the hook succeeds"; else cat "$S/build.log" | tail -15; fail "the build with a working hook failed"; fi
[ -f "$ODD" ] || fail "no exe"
[ "$(tail -c 14 "$ODD")" = "SIGNED-BY-FAKE" ] && ok "the signing tool ran last: its marker is the last thing in the exe" || fail "the marker is not at the end of the exe"
[ "$(cat "$RECORD.argc")" = 1 ] && ok "the tool got exactly one argument" || fail "the tool got $(cat "$RECORD.argc") arguments"
[ "$(cat "$RECORD.arg1")" = "$ODD" ] && ok "and it is the path of the exe, with every odd character intact (space & ' $ parentheses)" || fail "the argument was '$(cat "$RECORD.arg1")'"
grep -q '^signed: ' "$S/build.log" && ok "the build says it signed" || fail "no 'signed:' line"

# the same with a %1 that the user put in quotes: still one argument
export RECORD="$S/rec1q"
if SIGN_COMMAND="bash $S/fake_sign.sh \"%1\"" build "$ODD"; then [ "$(cat "$RECORD.argc")" = 1 ] && [ "$(cat "$RECORD.arg1")" = "$ODD" ] && ok "a quoted \"%1\" is taken as the same thing" || fail "a quoted %1 split the path"; else tail -8 "$S/build.log"; fail "build with a quoted %1 failed"; fi

# ---- 2: what must FAIL the build, and leave no exe -----------------------------------------------------------------------------------------------------------------------------------
OUT="$S/out.exe"
rm -f "$OUT"; export RECORD="$S/rec2"
if SIGN_COMMAND="bash $S/fake_sign.sh %1" FAKE_FAIL=1 build "$OUT"; then fail "a failing signing tool did not fail the build"; fi
[ ! -e "$OUT" ] && ok "a failing signing tool fails the build and the unsigned exe is deleted" || fail "an unsigned exe was left behind"
grep -q 'signing command failed' "$S/build.log" && ok "and the message says why" || fail "no message about the failed command"

rm -f "$OUT"
if SIGN_COMMAND="bash $S/fake_sign.sh %1" FAKE_NOOP=1 build "$OUT"; then fail "a signing tool that changes nothing was accepted"; fi
[ ! -e "$OUT" ] && grep -q 'did not change the exe' "$S/build.log" && ok "a tool that succeeds but changes nothing fails the build (nothing was signed)" || fail "a no-op signer was accepted or the exe was left"

rm -f "$OUT"
if SIGN_COMMAND="bash $S/fake_sign.sh %1" SIGN_VERIFY_COMMAND="bash $S/fake_verify.sh %1" VERIFY_FAIL=1 build "$OUT"; then fail "a failing verification did not fail the build"; fi
[ ! -e "$OUT" ] && grep -q 'did not verify' "$S/build.log" && ok "a verification that fails fails the build and deletes the exe" || fail "a failed verification left an exe or gave no message"

# ---- 3: configuration mistakes are refused BEFORE anything is built (the main file does not exist: if the build had started it would say so) ---------------------------------------------
refuse() { # description expected-text env...
    local what="$1" text="$2"; shift 2
    local t0=$SECONDS out
    out="$(env "$@" bash "$HERE/scripts/build.sh" "$S/does-not-exist.clg" "$S/refused.exe" 2>&1)"; local rc=$?
    [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "$text" && [ ! -e "$S/refused.exe" ] && [ $((SECONDS - t0)) -lt 15 ] && ok "$what" || { echo "$out" | tail -4; fail "$what (rc $rc)"; }
}
refuse "a command without %1 is refused at once" "must contain %1" RELEASE=1 SIGN_COMMAND="bash fake.sh"
refuse "without RELEASE=1 the hook is refused" "needs RELEASE=1" SIGN_COMMAND="bash fake.sh %1"
refuse "a verification command with no signing command is refused" "nothing to verify" RELEASE=1 SIGN_VERIFY_COMMAND="bash fake.sh %1"
refuse "a verification command without %1 is refused" "SIGN_VERIFY_COMMAND must contain %1" RELEASE=1 SIGN_COMMAND="bash fake.sh %1" SIGN_VERIFY_COMMAND="bash verify.sh"
out="$(RELEASE=1 SIGN_COMMAND="bash fake.sh %1" bash "$HERE/scripts/build.sh" "$S/does-not-exist.clg" "$S/refused.exe" --headless 2>&1)"; rc=$?
[ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "not signed" && ok "a --headless (test) build is not signed" || fail "a headless build with SIGN_COMMAND was not refused"

# ---- 4: the command is not printed ------------------------------------------------------------------------------------------------------------------------------------------------------
export RECORD="$S/rec4"
if SIGN_COMMAND="bash $S/fake_sign.sh %1 --token=TOP-SECRET-12345" build "$S/secret.exe"; then
    if grep -q 'TOP-SECRET-12345' "$S/build.log"; then fail "the secret in the signing command was printed"; else ok "the signing command, which may hold a key, is not printed"; fi
else tail -5 "$S/build.log"; fail "the build with a token argument failed"; fi

# ---- 5: for real: scripts/sign.ps1 with a throwaway certificate, checked with scripts/check_signature.ps1 -------------------------------------------------------------------------------
export SIGN_TEST_PASSWORD="correct horse battery"
env -u PSModulePath powershell -NoProfile -Command "
\$rsa = [System.Security.Cryptography.RSA]::Create(2048)
\$req = New-Object System.Security.Cryptography.X509Certificates.CertificateRequest('CN=pronao sign hook test', \$rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
\$oids = New-Object System.Security.Cryptography.OidCollection
[void]\$oids.Add((New-Object System.Security.Cryptography.Oid('1.3.6.1.5.5.7.3.3')))
\$req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension(\$oids, \$false)))
\$cert = \$req.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddDays(1))
[System.IO.File]::WriteAllBytes('$(W "$S/test.pfx")', \$cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, \$env:SIGN_TEST_PASSWORD))
" || fail "could not make the test certificate"
REAL="$S/real.exe"
SIGN='env -u PSModulePath powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w '"$HERE"'/scripts/sign.ps1)" -File "$(cygpath -w %1)" -Pfx "$(cygpath -w '"$S"'/test.pfx)" -PasswordEnv SIGN_TEST_PASSWORD'
VERIFY='r="$(env -u PSModulePath powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w '"$HERE"'/scripts/check_signature.ps1)" -File "$(cygpath -w %1)" | tr -d "\r")"; case "${r%%|*}" in Valid|UnknownError|NotTrusted) exit 0 ;; *) echo "status: $r"; exit 1 ;; esac'
if SIGN_COMMAND="$SIGN" SIGN_VERIFY_COMMAND="$VERIFY" build "$REAL"; then ok "a build signs the exe with sign.ps1 and verifies it"; else tail -15 "$S/build.log"; fail "the build with the real signing script failed"; fi
r="$(env -u PSModulePath powershell -NoProfile -ExecutionPolicy Bypass -File "$(W "$HERE/scripts/check_signature.ps1")" -File "$(W "$REAL")" | tr -d '\r')"
case "$r" in *"|CN=pronao sign hook test|"*) ok "the finished exe carries the signature of that certificate ($r)" ;; *) fail "the exe is not signed by the test certificate: $r" ;; esac
# the same build WITHOUT the hook is not signed: the signature is the hook's doing
env -u SIGN_COMMAND -u SIGN_VERIFY_COMMAND bash -c 'true'
UNSIGNED="$S/unsigned.exe"
build "$UNSIGNED" || fail "the build without the hook failed"
r="$(env -u PSModulePath powershell -NoProfile -ExecutionPolicy Bypass -File "$(W "$HERE/scripts/check_signature.ps1")" -File "$(W "$UNSIGNED")" | tr -d '\r')"
[ "${r%%|*}" = "NotSigned" ] && ok "and the same build without the hook is not signed" || fail "an exe built without the hook is reported as $r"

# ---- 6: the CI sketch of the manual, its shell run here as it is written (only the GitHub runner is missing): the certificate as base64 in a variable, decoded to a file for the build ---------------
CI="$S/ci"; mkdir -p "$CI"
export RUNNER_TEMP="$CI" GITHUB_REF_NAME="v1.4.0"
export WINDOWS_CERTIFICATE="$(base64 -w0 "$S/test.pfx")" SIGN_PASSWORD="$SIGN_TEST_PASSWORD"
STEP="$(sed -n '/^- name: Build and sign/,/^```/p' "$HERE/docs/manual/10-distribution.md")"
[ -n "$STEP" ] || fail "the CI step is not in the manual"
# the shell of the step is the text under "run: |", with the indentation taken off
echo "$STEP" | sed -n '/run: |/,$p' | sed '1d;$d' | sed 's/^    //' | sed 's|scripts/sign.ps1|'"$(cygpath -m "$HERE")"'/scripts/sign.ps1|; s|bash scripts/build.sh main.clg out/my-app.exe|bash '"$(cygpath -m "$HERE")"'/scripts/build.sh '"$(cygpath -m "$HERE")"'/tests/runtime_window.clg '"$CI"'/ci.exe|; s|ICON=app.ico ||' > "$CI/step.sh"
cp -r "$HERE/tests/site_placement" "$CI/site"
if (cd "$CI" && env ASSETS="$CI/site" WORK="$CI/work" bash "$CI/step.sh" > "$CI/step.log" 2>&1); then ok "the CI step, as written in the manual, builds a signed exe"; else tail -15 "$CI/step.log"; fail "the CI step of the manual failed when run here"; fi
r="$(env -u PSModulePath powershell -NoProfile -ExecutionPolicy Bypass -File "$(W "$HERE/scripts/check_signature.ps1")" -File "$(W "$CI/ci.exe")" | tr -d '\r')"
case "$r" in *"|CN=pronao sign hook test|"*) ok "and that exe carries the certificate's signature" ;; *) fail "the CI step's exe is not signed: $r" ;; esac
[ ! -e "$CI/cert.pfx" ] && ok "and the certificate file was deleted when the step ended" || fail "the certificate file was left on the runner"
grep -q "$SIGN_TEST_PASSWORD" "$CI/step.log" && fail "the certificate password is in the log" || ok "the password is not in the log"

echo "sign hook test: ok"
