/*
	PSSwanStation - what this title changes in mbedTLS's default configuration
	(MBEDTLS_USER_CONFIG_FILE, read after include/mbedtls/mbedtls_config.h).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	mbedTLS (Apache-2.0) is the TLS of tls_client.cpp: the console's own HTTPS
	client refuses every public certificate once the sandbox elevator has run.
	Here mbedTLS only checks certificates and encrypts: the sockets, the name
	lookup, the files and the randomness are the title's.
*/
#pragma once

/* Randomness: mbedtls_hardware_poll() in tls_client.cpp (the console's
   arc4random), never /dev/urandom or getrandom, which a title may not have. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* The title makes its own sockets and reads the certificate list itself. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* Not used by a client that only fetches. */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_ECJPAKE_C
#undef MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED
#undef MBEDTLS_LMS_C
