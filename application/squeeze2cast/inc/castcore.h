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

#ifndef __CASTCORE_H
#define __CASTCORE_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "squeezedefs.h"
#include "squeeze2cast.h"
#include "util_common.h"
#include "castcore.h"

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

#define CAST_BEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define CAST_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define CAST_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define CAST_MEDIA "urn:x-cast:com.google.cast.media"

typedef int sockfd;

typedef struct {
	bool			running;
	enum { CAST_IDLE, CAST_CONNECTING, CAST_CONNECTED } Connect;
	void			*owner;
	SSL 			*ssl;
	bool			sslConnect;
	sockfd 			sock;
	int				reqId, waitId, waitMedia;
	pthread_t 		Thread, PingThread;
	pthread_mutex_t	Mutex, eventMutex;
	pthread_cond_t	eventCond;
	char 			*sessionId, *transportId;
	int				mediaSessionId;
	enum { CAST_WAIT, CAST_WAIT_MEDIA } State;
	struct in_addr	ip;
	u16_t			port;
	tQueue			eventQueue, reqQueue;
	u8_t 			MediaVolume;
	u32_t			lastPong;
	bool			group;
} tCastCtx;

typedef struct {
	char Type[32] ;
	union {
		json_t * msg;
	} data;
} tReqItem;

bool SendCastMessage(SSL *ssl, char *ns, char *dest, char *payload, ...);
bool ConnectReceiver(tCastCtx *Ctx);
void SetVolume(tCastCtx *Ctx, u8_t Volume);
void CastQueueFlush(tQueue *Queue);
void CastCoreInit(log_level level);

#endif
