/*
 * $Id$
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

#include "heritage.h"
#include "mgt.h"

/*--------------------------------------------------------------------*/

void
TCP_name(struct sockaddr *addr, unsigned l, char *abuf, unsigned alen, char *pbuf, unsigned plen)
{
	int i;

	i = getnameinfo(addr, l, abuf, alen, pbuf, plen,
	   NI_NUMERICHOST | NI_NUMERICSERV);
	if (i) {
		printf("getnameinfo = %d %s\n", i, gai_strerror(i));
		strlcpy(abuf, "Conversion", alen);
		strlcpy(pbuf, "Failed", plen);
		return;
	}
}

/*--------------------------------------------------------------------*/

void
TCP_myname(int sock, char *abuf, unsigned alen, char *pbuf, unsigned plen)
{
	struct sockaddr addr[2];	/* XXX: IPv6 hack */
	socklen_t l;

	l = sizeof addr;
	AZ(getsockname(sock, addr, &l));
	TCP_name(addr, l, abuf, alen, pbuf, plen);
}

/*--------------------------------------------------------------------*/

#ifdef HAVE_ACCEPT_FILTERS
static void
accept_filter(int fd)
{
	struct accept_filter_arg afa;
	int i;

	bzero(&afa, sizeof(afa));
	strcpy(afa.af_name, "httpready");
	errno = 0;
	i = setsockopt(fd, SOL_SOCKET, SO_ACCEPTFILTER,
	    &afa, sizeof(afa));
	if (i)
		printf("Acceptfilter(%d, httpready): %d %s\n",
		    fd, i, strerror(errno));
}
#endif

static void
create_listen_socket(const char *addr, const char *port, int *sp, int nsp)
{
	struct addrinfo ai, *r0, *r1;
	int i, j, s;

	memset(&ai, 0, sizeof ai);
	ai.ai_family = PF_UNSPEC;
	ai.ai_socktype = SOCK_STREAM;
	ai.ai_flags = AI_PASSIVE;
	i = getaddrinfo(addr, port, &ai, &r0);

	if (i) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(i));
		return;
	}

	for (r1 = r0; r1 != NULL && nsp > 0; r1 = r1->ai_next) {
		s = socket(r1->ai_family, r1->ai_socktype, r1->ai_protocol);
		if (s < 0)
			continue;
		j = 1;
		i = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &j, sizeof j);
		assert(i == 0);

		i = bind(s, r1->ai_addr, r1->ai_addrlen);
		if (i != 0) {
			perror("bind");
			continue;
		}
		assert(i == 0);
		*sp = s;
		sp++;
		nsp--;
	}

	freeaddrinfo(r0);
}

int
open_tcp(const char *port)
{
	unsigned u;

	for (u = 0; u < HERITAGE_NSOCKS; u++) {
		heritage.sock_local[u] = -1;
		heritage.sock_remote[u] = -1;
	}

	create_listen_socket("localhost", port,
	    &heritage.sock_local[0], HERITAGE_NSOCKS);

	create_listen_socket(NULL, port,
	    &heritage.sock_remote[0], HERITAGE_NSOCKS);

	for (u = 0; u < HERITAGE_NSOCKS; u++) {
		if (heritage.sock_local[u] >= 0) {
			AZ(listen(heritage.sock_local[u], 16));
#ifdef HAVE_ACCEPT_FILTERS
			accept_filter(heritage.sock_local[u]);
#endif
		}
		if (heritage.sock_remote[u] >= 0) {
			AZ(listen(heritage.sock_remote[u], 16));
#ifdef HAVE_ACCEPT_FILTERS
			accept_filter(heritage.sock_remote[u]);
#endif
		}
	}
	return (0);
}
