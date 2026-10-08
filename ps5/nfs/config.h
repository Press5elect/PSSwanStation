/* PSSwanStation - libnfs's configuration for the PS5 payload SDK (FreeBSD
 * headers; AUTH_SYS only: no Kerberos, no TLS, no threads of its own). */
#define HAVE_ARPA_INET_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_SOCKADDR_LEN 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UNISTD_H 1
/* The SDK's FreeBSD headers name the coarse clock otherwise, and leave
 * IFNAMSIZ to <net/if.h>, which libnfs does not include here. */
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC_FAST
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif
#define HAVE_NET_IF_H 1
#define HAVE_SYS_STATVFS_H 1
/* No looking for servers on the network (it lists the interfaces with ioctl,
 * which the console's sockets are not asked for here). */
#define NO_SRV_AUTOSCAN 1
#define HAVE_UTIME_H 1
