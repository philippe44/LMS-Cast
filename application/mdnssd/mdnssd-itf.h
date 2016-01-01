#ifndef __MDNSSD_ITF_H
#define __MDNSSD_ITF_H

#if defined(linux)
#define LINUX     1
#define OSX       0
#define WIN       0
#define FREEBSD   0
#elif defined (__APPLE__)
#define LINUX     0
#define OSX       1
#define WIN       0
#define FREEBSD   0
#elif defined (_MSC_VER) || defined(__BORLANDC__)
#define LINUX     0
#define OSX       0
#define WIN       1
#define FREEBSD   0
#elif defined(__FreeBSD__)
#define LINUX     0
#define OSX       0
#define WIN       0
#define FREEBSD   1
#else
#error unknown target
#endif

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#if WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#define MAX_ANSWERS (32)

typedef struct {
	char *name;
	char *value;
} txt_attr;

typedef struct {
  struct mDNSItem_s {
	char* name; // name from PTR
	char* hostname; // from SRV
	struct in_addr addr; // from A
	unsigned short port; // from SRVFound;
	txt_attr *attr;
	int	attr_count;
  } items[MAX_ANSWERS];
  int count;
} DiscoveredList;

bool 	query_mDNS(int sock, char* query_arg, DiscoveredList* dlist, int runtime);
int 	init_mDNS(int dbg, char* ip);
void 	close_mDNS(int sock);
void 	free_discovered_list(DiscoveredList* dlist);
#endif
