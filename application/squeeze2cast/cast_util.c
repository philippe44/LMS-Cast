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


#include <stdlib.h>
#include <math.h>

#include "squeezedefs.h"
#include "util_common.h"
#include "util.h"
#include "castcore.h"
#include "cast_util.h"
#include "squeeze2cast.h"

static log_level loglevel = lWARN;

/*----------------------------------------------------------------------------*/
void CastInit(log_level level)
{
	loglevel = level;
}


/*----------------------------------------------------------------------------*/
void CastKeepAlive(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// SSL context might not be set yet
	if (!p) return;

	pthread_mutex_lock(&Ctx->Mutex);
	SendCastMessage(Ctx->ssl, CAST_BEAT, NULL, "{\"type\":\"PING\"}");
	if (Ctx->transportId) SendCastMessage(Ctx->ssl, CAST_BEAT, Ctx->transportId, "{\"type\":\"PING\"}");
	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
bool CastPeerDisc(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;
	bool status;

	pthread_mutex_lock(&Ctx->Mutex);
	status = Ctx->ssl && !Ctx->sslConnect;
	pthread_mutex_unlock(&Ctx->Mutex);
	return status;
}


/*----------------------------------------------------------------------------*/
void CastGetStatus(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// SSL context might not be set yet
	if (!p) return;

	pthread_mutex_lock(&Ctx->Mutex);
	SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL, "{\"type\":\"GET_STATUS\",\"requestId\":%d}", Ctx->reqId++);
	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
void CastGetMediaStatus(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// SSL context might not be set yet
	if (!p) return;

	pthread_mutex_lock(&Ctx->Mutex);

	if (Ctx->mediaSessionId) {
		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"GET_STATUS\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->reqId++, Ctx->mediaSessionId);
	}

	pthread_mutex_unlock(&Ctx->Mutex);
}



/*----------------------------------------------------------------------------*/
bool CastLoad(void *p, char *URI, char *ContentType, struct sq_metadata_s *MetaData)
{
	tCastCtx *Ctx = (tCastCtx*) p;
	json_t *msg;
	char* str;

	if (!ConnectReceiver(Ctx, 10000)) {
		LOG_ERROR("[%p]: Cannot connect Cast receiver", Ctx->owner);
		return false;
	}

	// if SSL connection is lost a signal will be sent to unlock
	pthread_mutex_lock(&Ctx->reqMutex);
	if (Ctx->waitId) pthread_cond_wait(&Ctx->reqCond, &Ctx->reqMutex);
	Ctx->waitId = Ctx->reqId++;
	pthread_mutex_unlock(&Ctx->reqMutex);

	pthread_mutex_lock(&Ctx->Mutex);

	msg = json_pack("{ss,ss,ss}", "contentId", URI, "streamType", "BUFFERED",
					"contentType", ContentType);

	if (MetaData) {
		json_t *metadata = json_pack("{si,ss,ss,ss,ss,si}",
							"metadataType", 3,
							"albumName", MetaData->album, "title", MetaData->title,
							"albumArtist", MetaData->artist, "artist", MetaData->artist,
							"trackNumber", MetaData->track, "images", "url", MetaData->artwork);

		/*
		if (MetaData->artwork) {
			json_t *artwork = json_pack("{s[{ss}]}", "images", "url", MetaData->artwork);
			json_object_update(metadata, artwork);
		}
		*/

		metadata = json_pack("{s,o}", "metadata", metadata);
		json_object_update(msg, metadata);
		json_decref(metadata);
	}

	msg = json_pack("{ss,si,ss,sf,sb,so}", "type", "LOAD",
					"requestId", Ctx->waitId, "sessionId", Ctx->sessionId,
					"currentTime", 0.0, "autoplay", 0,
					"media", msg);

	str = json_dumps(msg, JSON_ENCODE_ANY | JSON_INDENT(1));

	if (str) SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId, str);
	json_decref(msg);
	NFREE(str);

	pthread_mutex_unlock(&Ctx->Mutex);

	return true;
}


/*----------------------------------------------------------------------------*/
void CastPlay(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// lock on wait for a Cast response
	pthread_mutex_lock(&Ctx->reqMutex);
	if (Ctx->waitId) pthread_cond_wait(&Ctx->reqCond, &Ctx->reqMutex);

	// no media session, nothing to do
	pthread_mutex_lock(&Ctx->Mutex);
	if (Ctx->mediaSessionId) {
		Ctx->waitId = Ctx->reqId++;

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"PLAY\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->waitId++, Ctx->mediaSessionId);

	}
	pthread_mutex_unlock(&Ctx->Mutex);

	pthread_mutex_unlock(&Ctx->reqMutex);
}


/*----------------------------------------------------------------------------*/
void CastStop(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// lock on wait for a Cast response
	pthread_mutex_lock(&Ctx->reqMutex);
	if (Ctx->waitId) pthread_cond_reltimedwait(&Ctx->reqCond, &Ctx->reqMutex, 2000);

	// no media session, nothing to do
	pthread_mutex_lock(&Ctx->Mutex);
	if (Ctx->mediaSessionId) {
		Ctx->waitId = Ctx->reqId++;

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"STOP\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->waitId, Ctx->mediaSessionId);

		Ctx->mediaSessionId = 0;
	}
	pthread_mutex_unlock(&Ctx->Mutex);

	pthread_mutex_unlock(&Ctx->reqMutex);
}


/*----------------------------------------------------------------------------*/
void CastPause(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// lock on wait for a Cast response
	pthread_mutex_lock(&Ctx->reqMutex);
	if (Ctx->waitId) pthread_cond_wait(&Ctx->reqCond, &Ctx->reqMutex);

	// no media session, nothing to do
	pthread_mutex_lock(&Ctx->Mutex);
	if (Ctx->mediaSessionId) {
		Ctx->waitId = Ctx->reqId++;

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"PAUSE\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->waitId, Ctx->mediaSessionId);

	}
	pthread_mutex_unlock(&Ctx->Mutex);

	pthread_mutex_unlock(&Ctx->reqMutex);
}

/*----------------------------------------------------------------------------*/
void SetVolume(void *p, u8_t Volume)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// no media session, nothing to do
	pthread_mutex_lock(&Ctx->Mutex);
	if (Ctx->mediaSessionId) {

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"mediaSessionId\":%d,\"volume\":{\"level\":%lf}}",
						Ctx->reqId++, Ctx->mediaSessionId, (double) Volume / 100.0);

	}
	pthread_mutex_unlock(&Ctx->Mutex);
}

/*----------------------------------------------------------------------------*/
int CastSeek(char *ControlURL, unsigned Interval)
{
	int rc = 0;

	return rc;
}





