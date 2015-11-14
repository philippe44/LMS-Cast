/*
 *  Squeeze2cast - LMS to Cast gateway
 *
 *  Squeezelite : (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *  Additions & gateway : (c) Philippe 2014, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdarg.h>

#include "squeezedefs.h"
#include "squeeze2cast.h"
#include "util_common.h"
#include "util.h"
#include "castcore.h"
#include "castitf.h"

#if LINUX || OSX || FREEBSD
#define last_error() errno
#elif WIN
#define last_error() WSAGetLastError()
#endif


/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static SSL_CTX *glSSLctx;
static void *CastSocketThread(void *args);
static log_level loglevel = lINFO;

#define DEFAULT_RECEIVER	"CC1AD845"


/*----------------------------------------------------------------------------*/
static void set_nonblock(sockfd s) {
#if WIN
	u_long iMode = 1;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}


/*----------------------------------------------------------------------------*/
static void set_block(sockfd s) {
#if WIN
	u_long iMode = 0;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags & (~O_NONBLOCK));
#endif
}


/*----------------------------------------------------------------------------*/
static int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout) {
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


/*----------------------------------------------------------------------------*/
void InitSSL(void)
{
	const SSL_METHOD *method;

	// initialize SSL stuff
	SSL_load_error_strings();
	SSL_library_init();

	// build the SSL objects...
	method = SSLv23_client_method();

	glSSLctx = SSL_CTX_new(method);
	SSL_CTX_set_options(glSSLctx, SSL_OP_NO_SSLv2);

}


/*----------------------------------------------------------------------------*/
void EndSSL(void)
{
	SSL_CTX_free(glSSLctx);
}


/*----------------------------------------------------------------------------*/
void swap32(u32_t *n)
{
 u32_t buf = *n;
 *n = 	(((u8_t) (buf >> 24))) +
		(((u8_t) (buf >> 16)) << 8) +
		(((u8_t) (buf >> 8)) << 16) +
		(((u8_t) (buf)) << 24);
}



/*----------------------------------------------------------------------------*/
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


/*----------------------------------------------------------------------------*/
bool SendCastMessage(SSL *ssl, char *ns, char *dest, char *payload, ...)
{
	CastMessage message = CastMessage_init_default;
	pb_ostream_t stream;
	u8_t *buffer;
	u16_t buffer_len = 2048;
	bool status;
	u32_t len;
	va_list args;

	if (!ssl) return false;

	va_start(args, payload);

	if (dest) strcpy(message.destination_id, dest);
	strcpy(message.namespace, ns);
	vsprintf(message.payload_utf8, payload, args);
	LOG_INFO("Cast sending: %s", message.payload_utf8);
	message.has_payload_utf8 = true;
	if ((buffer = malloc(buffer_len)) == NULL) return false;
	stream = pb_ostream_from_buffer(buffer, buffer_len);
	status = pb_encode(&stream, CastMessage_fields, &message);
	len = stream.bytes_written;
	swap32(&len);
	status &= (SSL_write(ssl, &len, 4) != 1);
	status &= SSL_write(ssl, buffer, stream.bytes_written);
	free(buffer);

	return status;
}

/*----------------------------------------------------------------------------*/
bool DecodeCastMessage(u8_t *buffer, u16_t len, CastMessage *msg)
{
	bool status;
	CastMessage message = CastMessage_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(buffer, len);

	status = pb_decode(&stream, CastMessage_fields, &message);
	memcpy(msg, &message, sizeof(CastMessage));
	return status;
}


/*----------------------------------------------------------------------------*/
bool GetNextMessage(SSL *ssl, CastMessage *message)
{
	bool status;
	u32_t len;
	u8_t *buf;

	if (!read_bytes(ssl, &len, 4)) return false;

	swap32(&len);
	if ((buf = malloc(len))== NULL) return false;
	status = read_bytes(ssl, buf, len);
	status &= DecodeCastMessage(buf, len, message);
	free(buf);
	return status;
}

/*----------------------------------------------------------------------------*/
bool ConnectReceiver(tCastCtx *Ctx, u32_t msWait)
{
	u32_t now = gettime_ms();

	pthread_mutex_lock(&Ctx->Mutex);

	if (Ctx->Connect == CAST_CONNECTED) {
		pthread_mutex_unlock(&Ctx->Mutex);
		return true;
	}

	if (Ctx->Connect == CAST_IDLE) {
		SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL, "{\"type\":\"LAUNCH\",\"requestId\":%d,\"appId\":\"%s\"}", Ctx->reqId++, DEFAULT_RECEIVER);
		SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL, "{\"type\":\"GET_STATUS\",\"requestId\":%d}", Ctx->reqId++);
		Ctx->Connect = CAST_CONNECTING;
	}

	while (Ctx->Connect != CAST_CONNECTED && gettime_ms() - now < msWait) {
		pthread_mutex_unlock(&Ctx->Mutex);
		usleep(50000);
		pthread_mutex_lock(&Ctx->Mutex);
	}

	pthread_mutex_unlock(&Ctx->Mutex);
	return (Ctx->Connect == CAST_CONNECTED);
}


/*----------------------------------------------------------------------------*/
bool ConnectCastDevice(void *p, in_addr_t ip)
{
	int err;
	struct sockaddr_in addr;
	tCastCtx *Ctx = p;
	SSL 			*ssl;

	// do nothing if we are already connected
	if (Ctx->ssl) return true;

	ssl  = SSL_new(glSSLctx);
	Ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
	set_nonblock(Ctx->sock);
//	set_nosigpipe(sock);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port = htons(8009);

	connect_timeout(Ctx->sock, (struct sockaddr *) &addr, sizeof(addr), 1);
	set_block(Ctx->sock);
	SSL_set_fd(ssl, Ctx->sock);
	err = SSL_connect(ssl);
	if (err != 1) {
		err = SSL_get_error(ssl,err);
		return false;
	}

	pthread_mutex_lock(&Ctx->Mutex);
	Ctx->ssl = ssl;
	Ctx->reqId = Ctx->waitId = Ctx->mediaSessionId = 0;
	Ctx->Connect = CAST_IDLE;
	NFREE(Ctx->sessionId);
	NFREE(Ctx->transportId);
	pthread_mutex_unlock(&Ctx->Mutex);

	pthread_create(&Ctx->Thread, NULL, &CastSocketThread, Ctx);

	SendCastMessage(Ctx->ssl, CAST_CONNECTION, NULL, "{\"type\":\"CONNECT\"}");

	return true;
}


/*----------------------------------------------------------------------------*/
void DisconnectCastDevice(void *p)
{
	tCastCtx *Ctx = p;

	// do nothing if we are not connected
	if (!Ctx->ssl) return;

	pthread_mutex_lock(&Ctx->Mutex);
	SSL_shutdown(Ctx->ssl);
#if 0
	// FIXME: causes a segfault !
	SSL_free(Ctx->ssl);
#endif
	Ctx->ssl = NULL;
	closesocket(Ctx->sock);
	Ctx->running = false;
	pthread_join(Ctx->Thread, NULL);
	NFREE(Ctx->sessionId);
	NFREE(Ctx->transportId);
	Ctx->reqId = Ctx->waitId = Ctx->mediaSessionId = 0;
	pthread_cond_signal(&Ctx->eventCond);
	pthread_cond_signal(&Ctx->reqCond);
	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
void *InitCastCtx(void *owner)
{
	tCastCtx *Ctx = malloc(sizeof(tCastCtx));

	Ctx->reqId = Ctx->waitId = Ctx->mediaSessionId = 0;
	Ctx->sessionId = Ctx->transportId = NULL;
	Ctx->owner = owner;
	Ctx->ssl = NULL;

	QueueInit(&Ctx->eventQueue);
	pthread_mutex_init(&Ctx->Mutex, 0);
	pthread_mutex_init(&Ctx->reqMutex, 0);
	pthread_mutex_init(&Ctx->eventMutex, 0);
	pthread_cond_init(&Ctx->reqCond, 0);
	pthread_cond_init(&Ctx->eventCond, 0);

	return Ctx;
}


/*----------------------------------------------------------------------------*/
void CloseCastCtx(void *p)
{
	tCastCtx *Ctx = p;

	pthread_cond_destroy(&Ctx->reqCond);
	pthread_cond_destroy(&Ctx->eventCond);
	pthread_mutex_destroy(&Ctx->reqMutex);
	pthread_mutex_destroy(&Ctx->eventMutex);
	free(p);
}


/*----------------------------------------------------------------------------*/
json_t *GetTimedEvent(void *p, u32_t msWait)
{
	json_t *data;
	tCastCtx *Ctx = (tCastCtx*) p;

	pthread_mutex_lock(&Ctx->eventMutex);
	pthread_cond_reltimedwait(&Ctx->eventCond, &Ctx->eventMutex, msWait);
	data = QueueExtract(&Ctx->eventQueue);
	pthread_mutex_unlock(&Ctx->eventMutex);

	return data;
}


/*----------------------------------------------------------------------------*/
static void *CastSocketThread(void *args)
{
	tCastCtx *Ctx = (tCastCtx*) args;
	CastMessage Message;
	json_t *root, *val;
	json_error_t  error;
	Ctx->running = true;

	while (Ctx->running) {
		int requestId = 0;
		bool done = false;
		const char *str = NULL;

		if (!GetNextMessage(Ctx->ssl, &Message)) {
			LOG_WARN("[%p]: SSL connection lost", Ctx);
			return NULL;
		}

		pthread_mutex_lock(&Ctx->Mutex);

		root = json_loads(Message.payload_utf8, 0, &error);
		val = json_object_get(root, "requestId");

		if (json_is_integer(val)) requestId = json_integer_value(val);
		val = json_object_get(root, "type");

		if (json_is_string(val)) {
			str = json_string_value(val);

			LOG_DEBUG("type:%s (id%d) (s:%s) (d:%s)\n%s", str, requestId,
					 Message.source_id,Message.destination_id, Message.payload_utf8);

			// respond to device ping
			if (!strcasecmp(str,"PING")) {
				SendCastMessage(Ctx->ssl, CAST_BEAT, Message.source_id, "{\"type\":\"PONG\"}");
				json_decref(root);
				done = true;
			}

			// connection closing
			if (!strcasecmp(str,"CLOSE")) {
				NFREE(Ctx->sessionId);
				NFREE(Ctx->transportId);
				Ctx->Connect = CAST_IDLE;
			}

			// receiver status before connection is fully established
			if (!strcasecmp(str,"RECEIVER_STATUS") && Ctx->Connect == CAST_CONNECTING) {
				const char *str;

				NFREE(Ctx->sessionId);
				str = GetAppIdItem(root, DEFAULT_RECEIVER, "sessionId");
				if (str) Ctx->sessionId = strdup(str);
				NFREE(Ctx->transportId);
				str = GetAppIdItem(root, DEFAULT_RECEIVER, "transportId");
				if (str) Ctx->transportId = strdup(str);

				if (Ctx->sessionId && Ctx->transportId) {
					Ctx->Connect = CAST_CONNECTED;
					SendCastMessage(Ctx->ssl, CAST_CONNECTION, Ctx->transportId,
									"{\"type\":\"CONNECT\",\"origin\":{}}");
                }
				else {
					usleep(200000);
					Ctx->waitId = Ctx->reqId;
					SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL,
									"{\"type\":\"GET_STATUS\",\"requestId\":%d}",
									Ctx->reqId++);
				}

				json_decref(root);
				done = true;
			}

			// manage queue of requests
			pthread_mutex_lock(&Ctx->reqMutex);
			if (Ctx->waitId && Ctx->waitId <= requestId) {
				pthread_cond_signal(&Ctx->reqCond);
				Ctx->waitId = 0;

				// media status only acquired for expected id
				if (!strcasecmp(str,"MEDIA_STATUS")) {
					int id = GetMediaItem_I(root, 0, "mediaSessionId");
					if (id) {
						Ctx->mediaSessionId = id;
						LOG_INFO("[%p]: Media session id %d", Ctx->owner, Ctx->mediaSessionId);
					}
				}
			}
			pthread_mutex_unlock(&Ctx->reqMutex);

		}

		pthread_mutex_unlock(&Ctx->Mutex);

		// queue event and signal handler
		if (!done) {
			pthread_mutex_lock(&Ctx->eventMutex);
			QueueInsert(&Ctx->eventQueue, root);
			pthread_cond_signal(&Ctx->eventCond);
			pthread_mutex_unlock(&Ctx->eventMutex);
		}
	}

	return NULL;
}



