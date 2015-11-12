#pragma hdrstop
#pragma argsused

#include <stdio.h>

#ifdef _WIN32
#include <tchar.h>
#else
  typedef char _TCHAR;
  #define _tmain main
#endif

#define WIN 1

#if WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <iphlpapi.h>
#endif

#include "openssl/crypto.h"
#include "openssl/x509.h"
#include "openssl/pem.h"
#include "openssl/ssl.h"
#include "openssl/err.h"

#include <fcntl.h>

#include <pb_encode.h>
#include <pb_decode.h>
#include "jansson.h"
#include "castmessage.pb.h"
#include "pthread.h"

typedef unsigned __int8  u8_t;
typedef unsigned __int16 u16_t;
typedef unsigned __int32 u32_t;
typedef unsigned __int64 u64_t;
typedef __int16 s16_t;
typedef __int32 s32_t;
typedef __int64 s64_t;

#define in_addr_t u32_t
#define socklen_t int
#define ssize_t int
#define usleep(x) Sleep(x/1000)
#define sleep(x) Sleep(x*1000)

typedef int sockfd;

#if OSX
void set_nosigpipe(sockfd s);
#else
#define set_nosigpipe(s)
#endif

#if LINUX || OSX || FREEBSD
#define last_error() errno
#elif WIN
#define last_error() WSAGetLastError()
#endif

void set_nonblock(sockfd s) {
#if WIN
	u_long iMode = 1;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

void set_block(sockfd s) {
#if WIN
	u_long iMode = 0;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags & (~O_NONBLOCK));
#endif
}

// connect for socket already set to non blocking with timeout in seconds
int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout) {
	fd_set w, e;
	struct timeval tval;

	if (connect(sock, addr, addrlen) < 0) {
#if !WIN
		if (last_error() != EINPROGRESS) {
#else
		if (last_error() != WSAEWOULDBLOCK) {
#endif
			return -1;
		}
	}

	FD_ZERO(&w);
	FD_SET(sock, &w);
	e = w;
	tval.tv_sec = timeout;
	tval.tv_usec = 0;

	// only return 0 if w set and sock error is zero, otherwise return error code
	if (select(sock + 1, NULL, &w, &e, timeout ? &tval : NULL) == 1 && FD_ISSET(sock, &w)) {
		int	error = 0;
		socklen_t len = sizeof(error);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&error, &len);
		return error;
	}

	return -1;
}

#if WIN
void winsock_init(void) {
    WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 2);
    int WSerr = WSAStartup(wVersionRequested, &wsaData);
    if (WSerr != 0) {
        exit(1);
    }
}

#define NFREE(p) if (p) { free(p); p = NULL; }

void winsock_close(void) {
	WSACleanup();
}
#endif

u32_t gettime_ms(void) {
#if WIN
	return GetTickCount();
#else
#if LINUX || FREEBSD
	struct timespec ts;
	if (!clock_gettime(CLOCK_MONOTONIC, &ts)) {
		return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}
#endif
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

void swap32(u32_t *n)
{
 u32_t buf = *n;
 *n = 	(((u8_t) (buf >> 24))) +
		(((u8_t) (buf >> 16)) << 8) +
		(((u8_t) (buf >> 8)) << 16) +
		(((u8_t) (buf)) << 24);
}


bool read_bytes(SSL *ssl, void *buffer, u16_t bytes)
{
	u16_t read = 0;

	while (bytes - read) {
		int nb = SSL_read(ssl, (u8_t*) buffer + read, bytes - read);
		if (nb < 0) return false;
		read += nb;
	}

	return true;
}


bool SendCastMessage(SSL *ssl, char *ns, char *dest, char *payload, ...)
{
	CastMessage message = CastMessage_init_default;
	pb_ostream_t stream;
	u8_t buffer[1024];
	bool status;
	u32_t len;
	va_list args;

	va_start(args, payload);

	if (dest) strcpy(message.destination_id, dest);
	strcpy(message.namespace, ns);
	vsprintf(message.payload_utf8, payload, args);
	// strcpy(message.payload_utf8, payload);
	message.has_payload_utf8 = true;
	stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
	status = pb_encode(&stream, CastMessage_fields, &message);
	len = stream.bytes_written;
	swap32(&len);
	status &= (SSL_write(ssl, &len, 4) != 1);
	status &= SSL_write(ssl, buffer, stream.bytes_written);

	return status;
}

bool DecodeCastMessage(u8_t *buffer, u16_t len, CastMessage *msg)
{
	bool status;
	CastMessage message = CastMessage_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(buffer, len);

	status = pb_decode(&stream, CastMessage_fields, &message);
	memcpy(msg, &message, sizeof(CastMessage));
	return status;
}


bool GetNextMessage(SSL *ssl, CastMessage *message)
{
	bool status;
	u32_t len;
	u8_t *buf;

	status = read_bytes(ssl, &len, 4);
	swap32(&len);
	if ((buf = malloc(len))== NULL) return false;
	status &= read_bytes(ssl, buf, len);
	status &= DecodeCastMessage(buf, len, message);
	free(buf);
	return status;
}

static pthread_t 	glSocketThread;
static pthread_t 	glTimerThread;
pthread_mutex_t		glSSLMutex;

char *GetItemId(json_t *root, char* appId, char *item)
{
	json_t *elm;
	int i;

	if ((elm = json_object_get(root, "status")) == NULL) return NULL;
	if ((elm = json_object_get(elm, "applications")) == NULL) return NULL;
	for (i = 0; i < json_array_size(elm); i++) {
		json_t *id, *data = json_array_get(elm, i);
		id = json_object_get(data, "appId");
		if (stricmp(json_string_value(id), appId)) continue;
		id = json_object_get(data, item);
		return json_string_value(id);
	}

	return NULL;
}

char *transportId = NULL;
char *sessionId = NULL;
int reqId = 10000;

static void *SocketThread(void *args)
{
	SSL *ssl = (SSL*) args;
	CastMessage Message;
	json_t *root, *val;
	json_error_t	error;
	char *str;
	bool playing = false;

	while (1) {
		GetNextMessage(ssl, &Message);
		printf("(s:%s) (r:%s) %s\n", Message.destination_id, Message.source_id, Message.payload_utf8);

		root = json_loads(Message.payload_utf8, 0, &error);
		val = json_object_get(root, "type");

		if (json_is_string(val)) {
			str = json_string_value(val);
			printf("%s\n",str);

			// respond to device ping
			if (!stricmp(str,"PING")) {
				pthread_mutex_lock(&glSSLMutex);
				SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.heartbeat", Message.source_id, "{\"type\":\"PONG\"}");
				pthread_mutex_unlock(&glSSLMutex);
			}

			// connection closed, re-open it
			if (!stricmp(str,"CLOSE")) {
				pthread_mutex_lock(&glSSLMutex);
				SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.connection", NULL, "{\"type\":\"CONNECT\"}");
				pthread_mutex_unlock(&glSSLMutex);
			}

			if (!stricmp(str,"RECEIVER_STATUS")) {

				// acquire the APP session Id and transport Id and then connect
				if (!sessionId || !transportId) {
					char *str;

					NFREE(sessionId);
					str = GetItemId(root, "CC1AD845", "sessionId");
					if (str) sessionId = strdup(str);
					NFREE(transportId);
					str = GetItemId(root, "CC1AD845", "transportId");
					if (str) transportId = strdup(str);

					pthread_mutex_lock(&glSSLMutex);
					SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.connection", transportId,
										 "{\"type\":\"CONNECT\",\"origin\":{}}");
					pthread_mutex_unlock(&glSSLMutex);
					json_decref(root);
					continue;
				}

				if (sessionId && transportId && !playing) {
					pthread_mutex_lock(&glSSLMutex);
					SendCastMessage(ssl, "urn:x-cast:com.google.cast.media", transportId,
									"{\"type\":\"LOAD\",\"requestId\":4,\"sessionId\":\"%s\",\"media\":{\"contentId\":\"http://95.81.155.24/8470/nrj_165631.mp3\",\"streamType\":\"LIVE\",\"contentType\":\"audio/mpeg\"}}",
									sessionId);
					pthread_mutex_unlock(&glSSLMutex);
					playing = true;
				}
			}
		}

		json_decref(root);
	}
}


static void *TimerThread(void *args)
{
	SSL *ssl = (SSL*) args;
	u32_t elapsed, last = gettime_ms();
	u32_t PingPoll = 0, StatusPoll = 0;

	while (1) {
		elapsed = gettime_ms() - last;
		PingPoll += elapsed;
		if (PingPoll > 1000) {
			printf("Ping\n");
			pthread_mutex_lock(&glSSLMutex);
			SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.heartbeat", NULL, "{\"type\":\"PING\"}");
			// if (transportId) SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.heartbeat", transportId, "{\"type\":\"PING\"}");
			pthread_mutex_unlock(&glSSLMutex);
			PingPoll = 0;
		}

		StatusPoll += elapsed;
		 if (StatusPoll > 5000) {
			printf("Poll status\n");
			pthread_mutex_lock(&glSSLMutex);
			SendCastMessage(ssl, "urn:x-cast:com.google.cast.receiver", NULL, "{\"type\":\"GET_STATUS\",\"requestId\":%d}", reqId++);
			pthread_mutex_unlock(&glSSLMutex);
			StatusPoll = 0;
		}

		last = gettime_ms();
		sleep(1);
    }
}


int main(int argc, _TCHAR* argv[])
{
	sockfd sock;
	bool status;
	json_t			*root, *val;
	json_error_t	error;
	SSL_CTX *ctx;
	SSL *ssl;
	const SSL_METHOD *method;
	int err;
	struct sockaddr_in addr;
	CastMessage Message;
	char *str;

	#if WIN
	winsock_init();
	#endif

	// initialize SSL stuff
	SSL_load_error_strings();
	SSL_library_init();

	// build the SSL objects...
	method = SSLv23_client_method();

	ctx = SSL_CTX_new(method);
	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2);

	ssl  = SSL_new(ctx);
	sock = socket(AF_INET, SOCK_STREAM, 0);
	set_nonblock(sock);
//	set_nosigpipe(sock);

	addr.sin_family = AF_INET;
	err = inet_pton(AF_INET, "192.168.2.16", &addr.sin_addr.s_addr);
	addr.sin_port = htons(8009);

	connect_timeout(sock, (struct sockaddr *) &addr, sizeof(addr), 1);
	set_block(sock);
	SSL_set_fd(ssl, sock);
	err = SSL_connect(ssl);
	if (err != 1) err = SSL_get_error(ssl,err);

	pthread_mutex_init(&glSSLMutex, 0);
	pthread_create(&glSocketThread, NULL, &SocketThread, ssl);

	//SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.connection", NULL, "{\"type\":\"CONNECT\",\"origin\":{}}");
	SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.connection", NULL, "{\"type\":\"CONNECT\"}");

	pthread_create(&glTimerThread, NULL, &TimerThread, ssl);

	SendCastMessage(ssl, "urn:x-cast:com.google.cast.receiver", NULL, "{\"type\":\"LAUNCH\",\"requestId\":%d,\"appId\":\"CC1AD845\"}", reqId++);
	// SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.connection", NULL, "{\"type\":\"CONNECT\"}");
	// SendCastMessage(ssl, "urn:x-cast:com.google.cast.tp.heartbeat", NULL, "{\"type\":\"PING\"}");
	// SendCastMessage(ssl, "urn:x-cast:com.google.cast.receiver", NULL, "{\"type\":\"GET_STATUS\",\"requestId\":2}");
	//SendCastMessage(ssl, "urn:x-cast:com.google.cast.receiver", "{\"type\":\"SET_VOLUME\",\"volume\":{\"level\":1},\"requestId\":3}");


	/*
	root = json_pack("{sisi}", "foo", 42, "bar", 7);
	root = json_pack("{so}", "sub", root);
	root = json_pack("{sosb}", "highfoo", root, "last", 1);
	res = json_dumps(root, JSON_ENCODE_ANY | JSON_INDENT(3));
	printf("%s", res);

	root = json_object();
	val = json_pack("{sb}", "sub", 1);
	json_object_set(root, "essai", val);
	res = json_dumps(root, JSON_ENCODE_ANY | JSON_INDENT(3));
	printf("%s", res);

	//root = json_loads(res, 0, &error);
	*/

	while (1) {
	}

	// free the SSL stuff....
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);

	return 0;
}
