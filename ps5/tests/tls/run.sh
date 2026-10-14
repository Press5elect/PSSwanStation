#!/usr/bin/env bash
# PSSwanStation: the PC's test of the title's own HTTPS client (src/tls_client.cpp).
#
#   ps5/tests/tls/run.sh
#
# Makes a certificate authority and a server certificate for localhost
# (openssl), starts tests/tls/server.py with TLS 1.3 and with TLS 1.2, and asks
# build-host's tls-test for answers of every kind: plain, chunked, redirected
# (absolute and relative), an error with a body, an answer ended by closing, a
# 3 MB download (both kinds, compared byte for byte), a list of DER
# certificates; and checks that a wrong authority, a wrong host name and a
# refused port fail.
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test=${TLS_TEST:-$here/../../../build-host/tls-test}
work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$work"' EXIT
cd "$work"
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj "/CN=Test CA" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr -subj "/CN=localhost" 2>/dev/null
printf "subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n" > ext.cnf
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out srv.pem -days 2 -extfile ext.cnf 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout other.key -out other.pem -days 2 -subj "/CN=Other CA" 2>/dev/null
openssl x509 -in other.pem -outform der -out other.der
openssl x509 -in ca.pem -outform der -out ca.der
cat other.der ca.der > list.der
python3 "$here/server.py" 18743 1.3 & python3 "$here/server.py" 18744 1.2 &
sleep 2
failures=0
expect() { # what, command...
    local what=$1; shift
    if "$@" > out.txt 2>&1; then echo "ok    $what"; else echo "FAIL  $what: $(tail -1 out.txt)"; failures=$((failures + 1)); fi
}
refuse() {
    local what=$1; shift
    if "$@" > out.txt 2>&1; then echo "FAIL  $what: it was accepted"; failures=$((failures + 1)); else echo "ok    $what ($(tail -1 out.txt))"; fi
}
for port in 18743 18744; do
    for path in ok chunked redirect rel close; do
        expect "$port /$path" bash -c "'$test' ca.pem https://localhost:$port/$path | grep -q 'status 200'"
    done
    expect "$port /missing gives 404 with its body" bash -c "'$test' ca.pem https://localhost:$port/missing | grep -q 'not here'"
    expect "$port 3 MB download" bash -c "'$test' ca.pem https://localhost:$port/big o.bin && cmp o.bin big$port.bin"
    expect "$port 3 MB chunked download" bash -c "'$test' ca.pem https://localhost:$port/bigchunked c.bin && cmp c.bin big$port.bin"
done
expect "DER certificate list" "$test" list.der https://localhost:18743/ok
refuse "another authority" "$test" other.pem https://localhost:18743/ok
refuse "another host name" "$test" ca.pem https://127.0.0.1:18743/ok
refuse "a refused port" "$test" ca.pem https://localhost:18799/ok
[[ $failures == 0 ]] && echo "tls: every case passed" || { echo "tls: $failures failed"; exit 1; }
