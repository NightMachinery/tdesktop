#!/usr/bin/env bash
# Creates the self-signed code signing certificate install.sh signs with, so
# that TCC permissions - Full Disk Access in particular - survive a rebuild.
#
# An ad-hoc signature has no certificate, so the app's designated requirement
# is the code hash of one exact binary and every build produces a new one. TCC
# keys its grants to that requirement, which is why the Full Disk Access the
# focus detector needs was lost on every install. Signed with a certificate,
# the requirement names the bundle identifier and the certificate instead, and
# neither changes when the code does.
#
# Idempotent: does nothing if the identity is already in the keychain. See
# docs/mac/build.md.
set -e

CertName="${CertName:-Purple Telegram Local Signing}"
Keychain="${Keychain:-$HOME/Library/Keychains/login.keychain-db}"
Days="${Days:-3650}"

if security find-certificate -c "$CertName" >/dev/null 2>&1; then
    echo "=== '$CertName' is already in the keychain ==="
    security find-identity -v -p codesigning | grep "$CertName" || true
    exit 0
fi

Work="$(mktemp -d "${TMPDIR:-/tmp}/purple-signing.XXXXXX")"
trap 'rm -rf "$Work"' EXIT

#: The system LibreSSL, deliberately, not whatever `openssl' resolves to. A
#: Homebrew or anaconda OpenSSL 3 writes PKCS#12 with a SHA-256 MAC that
#: macOS's Security framework refuses, and the import fails claiming the
#: password is wrong. LibreSSL's defaults are what `security import' expects.
OpenSSL=/usr/bin/openssl

echo "=== generating a code signing certificate ==="

#: extendedKeyUsage=codeSigning is the part that makes it a *code signing*
#: identity rather than a generic one; without it codesign will not use the
#: key and `find-identity -p codesigning' will not list it.
cat > "$Work/openssl.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no

[dn]
CN = $CertName

[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF

"$OpenSSL" req -x509 -newkey rsa:2048 -nodes \
    -keyout "$Work/key.pem" -out "$Work/cert.pem" -days "$Days" \
    -config "$Work/openssl.cnf"

#: A throwaway password rather than an empty one: an empty PKCS#12 password is
#: another thing `security import' has been known to trip over.
Password="$("$OpenSSL" rand -hex 16)"
"$OpenSSL" pkcs12 -export -out "$Work/identity.p12" \
    -inkey "$Work/key.pem" -in "$Work/cert.pem" -passout "pass:$Password"

echo "=== importing it into $(basename "$Keychain") ==="
#: -A lets codesign use the key without a prompt per invocation. macOS may
#: still ask once for the keychain password; answer it and choose Always Allow.
security import "$Work/identity.p12" -k "$Keychain" -P "$Password" -A \
    -T /usr/bin/codesign

echo "=== trusting it for code signing ==="
#: User trust settings, not the admin store, so this needs no root. macOS shows
#: an authentication prompt for it.
security add-trusted-cert -r trustRoot -p codeSign -k "$Keychain" \
    "$Work/cert.pem" || {
    echo "Could not set trust. codesign may still work; check with:" >&2
    echo "  security find-identity -v -p codesigning" >&2
}

echo "=== done ==="
security find-identity -v -p codesigning | grep "$CertName" \
    || echo "Not listed as a valid identity yet - see docs/mac/build.md." >&2
