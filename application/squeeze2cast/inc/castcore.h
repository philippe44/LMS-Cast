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
	void			*owner;
	SSL 			*ssl;
	sockfd 			sock;
	int				reqId, waitId;
	pthread_t 		Thread, TimerThread;
	pthread_mutex_t	Mutex, reqMutex, eventMutex;
	pthread_cond_t	reqCond, eventCond;
	char 			*sessionId, *transportId;
	int				mediaSessionId;
	tQueue			eventQueue;
} tCastCtx;

bool SendCastMessage(SSL *ssl, char *ns, char *dest, char *payload, ...);

#endif
