/*
	PSSwanStation - HTTP(S) GET with a TLS of the title's own (mbedTLS).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	tls_client.h says why. The pieces, each from what was measured on the
	console by the title or by BlackBearReloaded's ps5-native-app-boilerplate
	(GPL-3.0-or-later, examples/update-check/console_curl.c and docs/CURL.md):
	  - names are looked up with sceNetResolver (IPv4, 5 s a try, 2 retries):
	    a title's getaddrinfo resolves to a module it does not load;
	  - the certificate list is /system/common/cert/CA_LIST.cer once the title
	    has the console's root (else under the sandbox's random word);
	  - mbedTLS's randomness is the console's arc4random;
	  - one connection a request, "Connection: close", no compression.
*/
#include "tls_client.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(SWANSTATION_PS5)
#include <ps5platform/libc.h>
extern "C"
{
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address, int timeout, int retries,
		int flags);
int sceNetResolverDestroy(int resolver);
const char *sceKernelGetFsSandboxRandomWord(void);
}
#else
#include <sys/random.h>
#endif

// mbedTLS's randomness (MBEDTLS_ENTROPY_HARDWARE_ALT, mbedtls_config_fe.h).
extern "C" int mbedtls_hardware_poll(void *, unsigned char *output, size_t length, size_t *written)
{
#if defined(SWANSTATION_PS5)
	ps5_arc4random_buf(output, length);
#else
	size_t done = 0;
	while (done < length)
	{
		const ssize_t n = getrandom(output + done, length - done, 0);
		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)n;
	}
#endif
	*written = length;
	return 0;
}

namespace fe::tls
{
namespace
{
using Clock = std::chrono::steady_clock;

std::mutex mutex;
void (*logger)(const char *) = nullptr;
std::vector<std::string> certificateFiles;
// Lists read as well as the first of those: the title's own copy of
// Mozilla's (assets/cacert.pem), for servers whose root the console's list,
// fixed with its firmware, does not have yet.
std::vector<std::string> extraFiles;
std::string agent = "PSSwanStation";

void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void say(const char *format, ...)
{
	if (logger == nullptr)
		return;
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	logger(line);
}

std::string mbedError(int code)
{
	char text[160];
	mbedtls_strerror(code, text, sizeof(text));
	char line[200];
	snprintf(line, sizeof(line), "%s (-%#x)", text, (unsigned)-code);
	return line;
}

// What every connection shares, made once.
struct Shared
{
	bool tried = false, ready = false;
	std::string failure;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt ca;
	mbedtls_ssl_config config;
	int certificates = 0;
} shared;

int drbgEntropy(void *, unsigned char *output, size_t length)
{
	size_t written = 0;
	return mbedtls_hardware_poll(nullptr, output, length, &written);
}

bool readWhole(const std::string& path, std::vector<unsigned char>& out)
{
	FILE *file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	out.clear();
	unsigned char block[65536];
	size_t n;
	while ((n = fread(block, 1, sizeof(block), file)) > 0)
		out.insert(out.end(), block, block + n);
	fclose(file);
	return !out.empty();
}

// The certificates of a file: PEM (any number), or DER certificates one after
// another (each an ASN.1 SEQUENCE with its length).
int parseCertificates(mbedtls_x509_crt& chain, std::vector<unsigned char> data)
{
	int count = 0;
	static const char pemMark[] = "-----BEGIN CERTIFICATE-----";
	if (std::search(data.begin(), data.end(), pemMark, pemMark + sizeof(pemMark) - 1) != data.end())
	{
		data.push_back(0);	// mbedTLS reads PEM as a C string
		const int failed = mbedtls_x509_crt_parse(&chain, data.data(), data.size());
		for (mbedtls_x509_crt *c = &chain; c != nullptr && c->raw.len != 0; c = c->next)
			count++;
		if (failed > 0)
			say("tls: %d certificate(s) of the list could not be read", failed);
		return count;
	}
	size_t at = 0;
	while (at + 4 <= data.size())
	{
		if (data[at] != 0x30)
			break;
		size_t length = 0, header = 2;
		const unsigned char first = data[at + 1];
		if (first < 0x80)
			length = first;
		else
		{
			const unsigned bytes = first & 0x7f;
			if (bytes == 0 || bytes > 4 || at + 2 + bytes > data.size())
				break;
			for (unsigned i = 0; i < bytes; i++)
				length = (length << 8) | data[at + 2 + i];
			header = 2 + bytes;
		}
		const size_t total = header + length;
		if (at + total > data.size())
			break;
		if (mbedtls_x509_crt_parse_der(&chain, data.data() + at, total) == 0)
			count++;
		at += total;
	}
	return count;
}

std::vector<std::string> defaultCertificateFiles()
{
	std::vector<std::string> files;
	if (const char *own = getenv("SWANSTATION_CA_FILE"); own != nullptr && own[0] != 0)
		files.push_back(own);
#if defined(SWANSTATION_PS5)
	files.push_back("/system/common/cert/CA_LIST.cer");
	if (const char *word = sceKernelGetFsSandboxRandomWord(); word != nullptr && word[0] != 0)
		files.push_back(std::string("/") + word + "/common/cert/CA_LIST.cer");
#else
	files.push_back("/etc/ssl/certs/ca-certificates.crt");
	files.push_back("/etc/pki/tls/certs/ca-bundle.crt");
	files.push_back("/etc/ssl/cert.pem");
#endif
	return files;
}

bool setUp()
{
	Shared& s = shared;
	if (s.tried)
		return s.ready;
	s.tried = true;
	const psa_status_t psa = psa_crypto_init();
	if (psa != PSA_SUCCESS)
	{
		s.failure = "PSA crypto could not start (" + std::to_string((int)psa) + ")";
		say("tls: %s", s.failure.c_str());
		return false;
	}
	mbedtls_ctr_drbg_init(&s.drbg);
	mbedtls_x509_crt_init(&s.ca);
	mbedtls_ssl_config_init(&s.config);
	static const char personal[] = "PSSwanStation TLS";
	int r = mbedtls_ctr_drbg_seed(&s.drbg, drbgEntropy, nullptr, (const unsigned char *)personal, sizeof(personal) - 1);
	if (r != 0)
	{
		s.failure = "no randomness: " + mbedError(r);
		say("tls: %s", s.failure.c_str());
		return false;
	}
	const std::vector<std::string> files = certificateFiles.empty() ? defaultCertificateFiles() : certificateFiles;
	std::string used;
	for (const std::string& file : files)
	{
		std::vector<unsigned char> data;
		if (!readWhole(file, data))
			continue;
		s.certificates = parseCertificates(s.ca, data);
		if (s.certificates > 0)
		{
			used = file;
			break;
		}
	}
	for (const std::string& file : extraFiles)
	{
		std::vector<unsigned char> data;
		if (!readWhole(file, data))
			continue;
		const int before = s.certificates;
		s.certificates = std::max(parseCertificates(s.ca, data), s.certificates);
		used += (used.empty() ? "" : " and ") + file + " (" + std::to_string(s.certificates - before) + " more)";
	}
	if (s.certificates == 0)
	{
		s.failure = "no certificate list could be read";
		std::string tried;
		for (const std::string& file : files)
			tried += (tried.empty() ? "" : ", ") + file;
		say("tls: %s (tried %s)", s.failure.c_str(), tried.c_str());
		return false;
	}
	r = mbedtls_ssl_config_defaults(&s.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
			MBEDTLS_SSL_PRESET_DEFAULT);
	if (r != 0)
	{
		s.failure = "no TLS configuration: " + mbedError(r);
		say("tls: %s", s.failure.c_str());
		return false;
	}
	mbedtls_ssl_conf_authmode(&s.config, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&s.config, &s.ca, nullptr);
	mbedtls_ssl_conf_rng(&s.config, mbedtls_ctr_drbg_random, &s.drbg);
	say("tls: mbedTLS %s ready, %d certificates from %s", MBEDTLS_VERSION_STRING, s.certificates, used.c_str());
	s.ready = true;
	return true;
}

struct Url
{
	bool secure = true;
	std::string host;
	int port = 443;
	std::string path = "/";
};

bool parseUrl(const std::string& text, Url& url)
{
	size_t rest;
	if (text.compare(0, 8, "https://") == 0)
	{
		url.secure = true;
		url.port = 443;
		rest = 8;
	}
	else if (text.compare(0, 7, "http://") == 0)
	{
		url.secure = false;
		url.port = 80;
		rest = 7;
	}
	else
		return false;
	const size_t slash = text.find_first_of("/?#", rest);
	std::string authority = text.substr(rest, slash == std::string::npos ? std::string::npos : slash - rest);
	url.path = slash == std::string::npos ? "/" : text.substr(slash);
	if (url.path[0] != '/')
		url.path = "/" + url.path;
	if (const size_t hash = url.path.find('#'); hash != std::string::npos)
		url.path.resize(hash);
	if (const size_t at = authority.rfind('@'); at != std::string::npos)
		authority = authority.substr(at + 1);
	if (const size_t colon = authority.rfind(':'); colon != std::string::npos && authority.find(']') == std::string::npos)
	{
		url.port = atoi(authority.c_str() + colon + 1);
		authority.resize(colon);
	}
	url.host = authority;
	return !url.host.empty() && url.port > 0 && url.port < 65536;
}

// The address of `host`, IPv4.
bool lookUp(const std::string& host, in_addr& address, std::string& why)
{
	if (inet_pton(AF_INET, host.c_str(), &address) == 1)
		return true;
#if defined(SWANSTATION_PS5)
	const int pool = sceNetPoolCreate("psswanstation-dns", 16 * 1024, 0);
	if (pool < 0)
	{
		why = "no memory for the name lookup";
		return false;
	}
	bool ok = false;
	const int resolver = sceNetResolverCreate("psswanstation-dns", pool, 0);
	if (resolver >= 0)
	{
		const int r = sceNetResolverStartNtoa(resolver, host.c_str(), &address, 5 * 1000 * 1000, 2, 0);
		ok = r >= 0;
		if (!ok)
		{
			char text[96];
			snprintf(text, sizeof(text), "%s could not be looked up (%#x)", host.c_str(), (unsigned)r);
			why = text;
		}
		sceNetResolverDestroy(resolver);
	}
	else
		why = "no name resolver";
	sceNetPoolDestroy(pool);
	return ok;
#else
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *found = nullptr;
	const int r = getaddrinfo(host.c_str(), nullptr, &hints, &found);
	if (r != 0 || found == nullptr)
	{
		why = host + " could not be looked up";
		return false;
	}
	address = ((sockaddr_in *)found->ai_addr)->sin_addr;
	freeaddrinfo(found);
	return true;
#endif
}

void setWaits(int fd, unsigned seconds)
{
	timeval limit{};
	limit.tv_sec = seconds == 0 ? 1 : seconds;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

// A connected socket, or -1.
int connectTo(const in_addr& address, int port, unsigned seconds, std::string& why)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		why = "no socket (" + std::string(strerror(errno)) + ")";
		return -1;
	}
	setWaits(fd, seconds);
	const int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	sockaddr_in target{};
	target.sin_family = AF_INET;
	target.sin_port = htons((uint16_t)port);
	target.sin_addr = address;
#if defined(__FreeBSD__) || defined(SWANSTATION_PS5)
	target.sin_len = sizeof(target);
#endif
	// Without waiting longer than asked where the socket can be made
	// non-blocking (a title in its sandbox may not); else a plain connect.
	const int flags = fcntl(fd, F_GETFL, 0);
	const bool waitable = flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
	int r = connect(fd, (const sockaddr *)&target, sizeof(target));
	if (r != 0 && waitable && (errno == EINPROGRESS || errno == EWOULDBLOCK))
	{
		pollfd ready{ fd, POLLOUT, 0 };
		const int state = poll(&ready, 1, (int)(seconds * 1000));
		if (state == 1)
		{
			int error = 0;
			socklen_t length = sizeof(error);
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
			r = error == 0 ? 0 : -1;
			errno = error;
		}
		else
		{
			if (state == 0)
				errno = ETIMEDOUT;
			r = -1;
		}
	}
	if (waitable)
		fcntl(fd, F_SETFL, flags);
	if (r != 0)
	{
		char text[128];
		snprintf(text, sizeof(text), "no connection to port %d (%s)", port, strerror(errno));
		why = text;
		close(fd);
		return -1;
	}
	return fd;
}

int sendAll(void *context, const unsigned char *data, size_t length)
{
	const int fd = *(int *)context;
#if defined(MSG_NOSIGNAL)
	const ssize_t n = ::send(fd, data, length, MSG_NOSIGNAL);
#else
	const ssize_t n = ::send(fd, data, length, 0);
#endif
	if (n >= 0)
		return (int)n;
	if (errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int receive(void *context, unsigned char *data, size_t length)
{
	const int fd = *(int *)context;
	const ssize_t n = recv(fd, data, length, 0);
	if (n >= 0)
		return (int)n;
	if (errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_READ;
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	if (errno == ECONNRESET)
		return MBEDTLS_ERR_SSL_CONN_EOF;
	return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

// One connection, plain or TLS, read through a small buffer.
class Connection
{
public:
	~Connection() { close(); }

	bool open(const Url& url, unsigned seconds, Clock::time_point deadline, std::string& why)
	{
		m_deadline = deadline;
		in_addr address{};
		if (!lookUp(url.host, address, why))
			return false;
		m_fd = connectTo(address, url.port, seconds, why);
		if (m_fd < 0)
			return false;
		m_secure = url.secure;
		if (!m_secure)
			return true;
		if (!setUp())
		{
			why = "TLS is not available: " + shared.failure;
			return false;
		}
		mbedtls_ssl_init(&m_ssl);
		m_sslMade = true;
		int r = mbedtls_ssl_setup(&m_ssl, &shared.config);
		if (r == 0)
			r = mbedtls_ssl_set_hostname(&m_ssl, url.host.c_str());
		if (r != 0)
		{
			why = "TLS could not be set up: " + mbedError(r);
			return false;
		}
		mbedtls_ssl_set_bio(&m_ssl, &m_fd, sendAll, receive, nullptr);
		while ((r = mbedtls_ssl_handshake(&m_ssl)) != 0)
		{
			if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			{
				if (Clock::now() > m_deadline)
				{
					why = "the TLS handshake took too long";
					return false;
				}
				continue;
			}
			if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
			{
				char reasons[256];
				mbedtls_x509_crt_verify_info(reasons, sizeof(reasons), "", mbedtls_ssl_get_verify_result(&m_ssl));
				for (char *c = reasons; *c; c++)
					if (*c == '\n')
						*c = ';';
				why = std::string("the server's certificate was refused: ") + reasons;
			}
			else
				why = "the TLS handshake failed: " + mbedError(r);
			return false;
		}
		return true;
	}

	bool write(const std::string& text, std::string& why)
	{
		size_t done = 0;
		while (done < text.size())
		{
			int n;
			if (m_secure)
				n = mbedtls_ssl_write(&m_ssl, (const unsigned char *)text.data() + done, text.size() - done);
			else
				n = sendAll(&m_fd, (const unsigned char *)text.data() + done, text.size() - done);
			if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			if (n <= 0)
			{
				why = "the request could not be sent";
				return false;
			}
			done += (size_t)n;
		}
		return true;
	}

	// Up to `length` bytes: their count, 0 at the end of the answer, -1 on an error.
	int read(unsigned char *data, size_t length, std::string& why)
	{
		if (m_held < m_have)
		{
			const size_t n = std::min(length, m_have - m_held);
			memcpy(data, m_buffer + m_held, n);
			m_held += n;
			return (int)n;
		}
		return rawRead(data, length, why);
	}

	// A line ending in CRLF (without it), for headers and chunk sizes.
	bool readLine(std::string& line, std::string& why)
	{
		line.clear();
		for (;;)
		{
			if (m_held == m_have)
			{
				m_held = m_have = 0;
				const int n = rawRead(m_buffer, sizeof(m_buffer), why);
				if (n <= 0)
				{
					if (n == 0)
						why = "the answer ended early";
					return false;
				}
				m_have = (size_t)n;
			}
			const char c = (char)m_buffer[m_held++];
			if (c == '\n')
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				return true;
			}
			line += c;
			if (line.size() > 16384)
			{
				why = "a line of the answer is too long";
				return false;
			}
		}
	}

private:
	int rawRead(unsigned char *data, size_t length, std::string& why)
	{
		for (;;)
		{
			if (Clock::now() > m_deadline)
			{
				why = "the server took too long";
				return -1;
			}
			int n;
			if (m_secure)
				n = mbedtls_ssl_read(&m_ssl, data, length);
			else
				n = receive(&m_fd, data, length);
			if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			// TLS 1.3: the server's tickets come as data is waited for.
			if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
				continue;
			// The end, also from a server that closes without TLS's goodbye, as browsers take it.
			if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF || n == 0)
				return 0;
			if (n == MBEDTLS_ERR_SSL_TIMEOUT)
			{
				why = "the server took too long";
				return -1;
			}
			if (n < 0)
			{
				why = m_secure ? "reading the answer failed: " + mbedError(n) : "reading the answer failed";
				return -1;
			}
			return n;
		}
	}

	void close()
	{
		if (m_sslMade)
		{
			if (m_fd >= 0)
				mbedtls_ssl_close_notify(&m_ssl);
			mbedtls_ssl_free(&m_ssl);
			m_sslMade = false;
		}
		if (m_fd >= 0)
			::close(m_fd);
		m_fd = -1;
	}

	int m_fd = -1;
	bool m_secure = false, m_sslMade = false;
	mbedtls_ssl_context m_ssl;
	Clock::time_point m_deadline;
	unsigned char m_buffer[16384];
	size_t m_held = 0, m_have = 0;
};

std::string lower(std::string text)
{
	for (char& c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

std::string trim(const std::string& text)
{
	size_t a = 0, b = text.size();
	while (a < b && (text[a] == ' ' || text[a] == '\t'))
		a++;
	while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t'))
		b--;
	return text.substr(a, b - a);
}

std::string resolveLocation(const Url& from, const std::string& location)
{
	if (location.compare(0, 7, "http://") == 0 || location.compare(0, 8, "https://") == 0)
		return location;
	const std::string origin = std::string(from.secure ? "https://" : "http://") + from.host +
			((from.secure && from.port == 443) || (!from.secure && from.port == 80) ? "" : ":" + std::to_string(from.port));
	if (location.compare(0, 2, "//") == 0)
		return std::string(from.secure ? "https:" : "http:") + location;
	if (!location.empty() && location[0] == '/')
		return origin + location;
	std::string base = from.path.substr(0, from.path.find('?'));
	base.resize(base.rfind('/') + 1);
	return origin + base + location;
}

// Makes the request, following redirects; `body` is given the answer's body
// in pieces and stops it by answering false. The final status, or -1.
// A method other than GET, with headers and a body of the caller's: send().
struct Outgoing
{
	std::string method = "GET";
	std::vector<std::pair<std::string, std::string>> headers;
	const std::string *body = nullptr;
};

int request(const std::string& start, unsigned seconds, bool wholeTime, uint64_t *announced,
		const std::function<bool(int status, const unsigned char *data, size_t length)>& body, std::string& why,
		const Outgoing& outgoing = Outgoing())
{
	std::string current = start;
	const auto began = Clock::now();
	for (int hop = 0; hop < 8; hop++)
	{
		Url url;
		if (!parseUrl(current, url))
		{
			why = "not an http(s) address";
			return -1;
		}
		const auto deadline = wholeTime ? began + std::chrono::seconds(seconds)
										: Clock::now() + std::chrono::hours(24);
		Connection connection;
		if (!connection.open(url, seconds, deadline, why))
			return -1;
		const bool defaultPort = (url.secure && url.port == 443) || (!url.secure && url.port == 80);
		// A redirect of another method is followed as a GET only when it says so (303).
		const bool plainGet = hop > 0 || outgoing.method == "GET";
		std::string head = (plainGet ? std::string("GET") : outgoing.method) + " " + url.path + " HTTP/1.1\r\nHost: "
				+ url.host + (defaultPort ? "" : ":" + std::to_string(url.port)) + "\r\nUser-Agent: " + agent
				+ "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
		if (!plainGet)
		{
			for (const auto& [name, value] : outgoing.headers)
				head += name + ": " + value + "\r\n";
			if (outgoing.body != nullptr || outgoing.method == "POST" || outgoing.method == "PUT"
					|| outgoing.method == "PATCH")
				head += "Content-Length: " + std::to_string(outgoing.body != nullptr ? outgoing.body->size() : 0) + "\r\n";
		}
		else if (hop == 0)
			for (const auto& [name, value] : outgoing.headers)
				head += name + ": " + value + "\r\n";
		head += "\r\n";
		if (!connection.write(head, why))
			return -1;
		if (!plainGet && outgoing.body != nullptr && !outgoing.body->empty() && !connection.write(*outgoing.body, why))
			return -1;
		std::string line;
		if (!connection.readLine(line, why))
			return -1;
		// "HTTP/1.1 200 OK"
		int status = -1;
		if (line.compare(0, 5, "HTTP/") == 0)
		{
			const size_t space = line.find(' ');
			if (space != std::string::npos)
				status = atoi(line.c_str() + space + 1);
		}
		if (status < 100)
		{
			why = "not an HTTP answer";
			return -1;
		}
		bool chunked = false;
		int64_t length = -1;
		std::string location;
		for (;;)
		{
			if (!connection.readLine(line, why))
				return -1;
			if (line.empty())
				break;
			const size_t colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			const std::string name = lower(trim(line.substr(0, colon)));
			const std::string value = trim(line.substr(colon + 1));
			if (name == "transfer-encoding")
				chunked = lower(value).find("chunked") != std::string::npos;
			else if (name == "content-length")
				length = strtoll(value.c_str(), nullptr, 10);
			else if (name == "location")
				location = value;
		}
		// 1xx before the real answer (100 Continue): read on.
		if (status >= 100 && status < 200)
		{
			hop--;
			continue;
		}
		if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !location.empty()
				&& (outgoing.method == "GET" || status == 303))
		{
			current = resolveLocation(url, location);
			continue;
		}
		if (announced != nullptr)
			*announced = !chunked && length > 0 ? (uint64_t)length : 0;
		unsigned char block[16384];
		if (chunked)
		{
			for (;;)
			{
				if (!connection.readLine(line, why))
					return -1;
				const uint64_t size = strtoull(line.c_str(), nullptr, 16);
				if (size == 0)
				{
					// Trailers, then the empty line.
					while (connection.readLine(line, why) && !line.empty())
						;
					break;
				}
				uint64_t left = size;
				while (left > 0)
				{
					const int n = connection.read(block, (size_t)std::min<uint64_t>(left, sizeof(block)), why);
					if (n <= 0)
					{
						if (n == 0)
							why = "the answer ended early";
						return -1;
					}
					left -= (uint64_t)n;
					if (!body(status, block, (size_t)n))
						return status;
				}
				if (!connection.readLine(line, why))
					return -1;
			}
			return status;
		}
		uint64_t left = length >= 0 ? (uint64_t)length : UINT64_MAX;
		while (left > 0)
		{
			const int n = connection.read(block, (size_t)std::min<uint64_t>(left, sizeof(block)), why);
			if (n < 0)
				return -1;
			if (n == 0)
			{
				if (length >= 0)
				{
					why = "the answer ended early";
					return -1;
				}
				break;
			}
			left -= (uint64_t)n;
			if (!body(status, block, (size_t)n))
				return status;
		}
		return status;
	}
	why = "too many redirects";
	return -1;
}
}

void setCertificateFiles(const std::vector<std::string>& files)
{
	std::lock_guard<std::mutex> lock(mutex);
	certificateFiles = files;
}

void addCertificateFile(const std::string& file)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (std::find(extraFiles.begin(), extraFiles.end(), file) == extraFiles.end())
		extraFiles.push_back(file);
}

void setUserAgent(const std::string& value)
{
	std::lock_guard<std::mutex> lock(mutex);
	agent = value;
}

void setLog(void (*log)(const char *message))
{
	std::lock_guard<std::mutex> lock(mutex);
	logger = log;
}

int get(const std::string& url, std::vector<uint8_t>& out, unsigned seconds, size_t limit, std::string *why)
{
	std::lock_guard<std::mutex> lock(mutex);
	out.clear();
	std::string reason;
	bool tooLarge = false;
	const int status = request(url, seconds, true, nullptr,
			[&](int code, const unsigned char *data, size_t length) {
				const size_t most = code >= 200 && code < 300 ? limit : (size_t)(64u << 10);
				if (out.size() + length > most)
				{
					tooLarge = code >= 200 && code < 300;
					out.insert(out.end(), data, data + (most - out.size()));
					return false;
				}
				out.insert(out.end(), data, data + length);
				return true;
			},
			reason);
	if (tooLarge)
	{
		reason = "the answer is larger than " + std::to_string(limit) + " bytes";
		out.clear();
		if (why != nullptr)
			*why = reason;
		return -1;
	}
	if (status < 0)
	{
		out.clear();
		if (why != nullptr)
			*why = reason;
	}
	return status;
}

int send(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
		const std::string& body, std::vector<uint8_t>& out, unsigned seconds, size_t limit, std::string *why)
{
	std::lock_guard<std::mutex> lock(mutex);
	out.clear();
	Outgoing outgoing;
	outgoing.method = method;
	outgoing.headers = headers;
	outgoing.body = &body;
	std::string reason;
	bool tooLarge = false;
	const int status = request(url, seconds, true, nullptr,
			[&](int code, const unsigned char *data, size_t length) {
				const size_t most = code >= 200 && code < 300 ? limit : (size_t)(64u << 10);
				if (out.size() + length > most)
				{
					tooLarge = code >= 200 && code < 300;
					return false;
				}
				out.insert(out.end(), data, data + length);
				return true;
			},
			reason, outgoing);
	if (tooLarge || status < 0)
	{
		out.clear();
		if (why != nullptr)
			*why = tooLarge ? "the answer is larger than " + std::to_string(limit) + " bytes" : reason;
		return -1;
	}
	return status;
}

int download(const std::string& url, const std::string& path, uint64_t limit,
		const std::function<bool(uint64_t done, uint64_t total)>& progress, unsigned seconds, std::string *why)
{
	std::lock_guard<std::mutex> lock(mutex);
	std::string reason;
	FILE *file = nullptr;
	uint64_t done = 0, total = 0;
	bool ok = true;
	std::vector<unsigned char> held;
	held.reserve(1 << 20);
	const int status = request(url, seconds, false, &total,
			[&](int code, const unsigned char *data, size_t length) {
				if (code != 200)
					return false;
				if (file == nullptr)
				{
					file = fopen(path.c_str(), "wb");
					if (file == nullptr)
					{
						reason = "the file could not be made";
						ok = false;
						return false;
					}
				}
				held.insert(held.end(), data, data + length);
				done += length;
				// Written in pieces of a megabyte: a small write costs the
				// console's storage as much as a large one.
				if (held.size() >= (1u << 20))
				{
					if (fwrite(held.data(), 1, held.size(), file) != held.size())
					{
						reason = "the file could not be written";
						ok = false;
						return false;
					}
					held.clear();
				}
				if (done > limit || (progress && !progress(done, total)))
				{
					reason = done > limit ? "larger than allowed" : "stopped";
					ok = false;
					return false;
				}
				return true;
			},
			reason);
	if (file == nullptr && status == 200 && ok)
		file = fopen(path.c_str(), "wb");	// an empty answer
	if (file != nullptr)
	{
		if (ok && !held.empty() && fwrite(held.data(), 1, held.size(), file) != held.size())
			ok = false;
		ok = fclose(file) == 0 && ok;
	}
	ok = ok && status == 200 && (total == 0 || done == total);
	if (!ok)
	{
		if (file != nullptr)
			unlink(path.c_str());
		if (why != nullptr)
			*why = reason;
		return status == 200 ? -1 : status;
	}
	chmod(path.c_str(), 0666);
	return 200;
}
}
