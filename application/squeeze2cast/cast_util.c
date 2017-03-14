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
#include "log_util.h"
#include "util_common.h"
#include "util.h"
#include "castcore.h"
#include "cast_util.h"
#include "squeeze2cast.h"

extern log_level cast_loglevel;
static log_level *loglevel = &cast_loglevel;


/*----------------------------------------------------------------------------*/
bool CastIsConnected(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;
	bool status;

	pthread_mutex_lock(&Ctx->Mutex);
	status = (Ctx->ssl != NULL);
	pthread_mutex_unlock(&Ctx->Mutex);
	return status;
}


/*----------------------------------------------------------------------------*/
bool CastIsMediaSession(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;
	bool status;

	if (!Ctx) return false;

	pthread_mutex_lock(&Ctx->Mutex);
	status = Ctx->mediaSessionId != 0;
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
	if (!Ctx->waitId) {
		Ctx->waitId = Ctx->reqId++;

		SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL, "{\"type\":\"GET_STATUS\",\"requestId\":%d}", Ctx->waitId);
	}
	else {
		tReqItem *req = malloc(sizeof(tReqItem));
		strcpy(req->Type, "GET_STATUS");
		QueueInsert(&Ctx->reqQueue, req);
		LOG_INFO("[%p]: Queuing %s", Ctx->owner, req->Type);
	}

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
		if (!Ctx->waitId) {
			Ctx->waitId = Ctx->reqId++;

			SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
					"{\"type\":\"GET_STATUS\",\"requestId\":%d,\"mediaSessionId\":%d}",
					Ctx->waitId, Ctx->mediaSessionId);
		}
		else {
			tReqItem *req = malloc(sizeof(tReqItem));
			strcpy(req->Type, "GET_MEDIA_STATUS");
			QueueInsert(&Ctx->reqQueue, req);
			LOG_INFO("[%p]: Queuing %s", Ctx->owner, req->Type);
		}
	}

	pthread_mutex_unlock(&Ctx->Mutex);
}



/*----------------------------------------------------------------------------*/
bool CastLoad(void *p, char *URI, char *ContentType, struct sq_metadata_s *MetaData)
{
	tCastCtx *Ctx = (tCastCtx*) p;
	json_t *msg;
	char* str;

	if (!LaunchReceiver(Ctx)) {
		LOG_ERROR("[%p]: Cannot connect Cast receiver", Ctx->owner);
		return false;
	}

	msg = json_pack("{ss,ss,ss}", "contentId", URI, "streamType", "BUFFERED",
					"contentType", ContentType);

	if (MetaData) {

		json_t *metadata = json_pack("{si,ss,ss,ss,ss,si}",
							"metadataType", 3,
							"albumName", MetaData->album, "title", MetaData->title,
							"albumArtist", MetaData->artist, "artist", MetaData->artist,
							"trackNumber", MetaData->track);

		if (MetaData->artwork) {
			json_t *artwork = json_pack("{s[{ss}]}", "images", "url", MetaData->artwork);
			json_object_update(metadata, artwork);
			json_decref(artwork);
		}

		metadata = json_pack("{s,o}", "metadata", metadata);
		json_object_update(msg, metadata);
		json_decref(metadata);
	}

	pthread_mutex_lock(&Ctx->Mutex);

	// if receiver launched and no STOP pending (precaution) just send message
	if (Ctx->Status == CAST_LAUNCHED && !Ctx->waitId) {
		Ctx->waitId = Ctx->reqId++;
		Ctx->waitMedia = Ctx->waitId;
		Ctx->mediaSessionId = 0;

		msg = json_pack("{ss,si,ss,sf,sb,so}", "type", "LOAD",
						"requestId", Ctx->waitId, "sessionId", Ctx->sessionId,
						"currentTime", 0.0, "autoplay", 0,
						"media", msg);

		str = json_dumps(msg, JSON_ENCODE_ANY | JSON_INDENT(1));
		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId, str);
		json_decref(msg);
		NFREE(str);
	}
	// otherwise queue it for later
	else {
		tReqItem *req = malloc(sizeof(tReqItem));
		strcpy(req->Type, "LOAD");
		req->data.msg = msg;
		QueueInsert(&Ctx->reqQueue, req);
		LOG_INFO("[%p]: Queuing %s", Ctx->owner, req->Type);
	}

	pthread_mutex_unlock(&Ctx->Mutex);

	return true;
}


/*----------------------------------------------------------------------------*/
void CastSimple(void *p, char *Type)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// lock on wait for a Cast response
	pthread_mutex_lock(&Ctx->Mutex);
	if (Ctx->Status == CAST_LAUNCHED && !Ctx->waitId) {
		// no media session, nothing to do
		if (Ctx->mediaSessionId) {
			Ctx->waitId = Ctx->reqId++;

			SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
							"{\"type\":\"%s\",\"requestId\":%d,\"mediaSessionId\":%d}",
							Type, Ctx->waitId, Ctx->mediaSessionId);

		}
		else {
			LOG_WARN("[%p]: %s req w/o a session", Type, Ctx->owner);
	   }
	}
	else {
		tReqItem *req = malloc(sizeof(tReqItem));
		strcpy(req->Type, Type);
		QueueInsert(&Ctx->reqQueue, req);
		LOG_INFO("[%p]: Queuing %s", Ctx->owner, req->Type);
	}

	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
void CastStop(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// lock on wait for a Cast response
	pthread_mutex_lock(&Ctx->Mutex);
	CastQueueFlush(&Ctx->reqQueue);
	if (Ctx->Status == CAST_LAUNCHED && Ctx->mediaSessionId) {
		Ctx->waitId = Ctx->reqId++;

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"STOP\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->waitId, Ctx->mediaSessionId);

		Ctx->mediaSessionId = 0;

	}
	else {
		if (Ctx->Status == CAST_LAUNCHING) {
			Ctx->Status = CAST_CONNECTED;
			LOG_WARN("[%p]: Stop while still launching receiver", Ctx->owner);
		} else {
			LOG_WARN("[%p]: Stop w/o session or connect", Ctx->owner);
		}
	}

	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
void CastPowerOff(void *p)
{
	CastDisconnect(p);
}


/*----------------------------------------------------------------------------*/
void CastPowerOn(void *p)
{
	CastConnect(p);
}


/*----------------------------------------------------------------------------*/
void CastSetDeviceVolume(void *p, u8_t Volume)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	if (Ctx->group) Volume = ((u32_t) Volume * Ctx->MediaVolume) / 100;

	if (Volume > 100) Volume = 100;

	pthread_mutex_lock(&Ctx->Mutex);

	if (Volume) {
		SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL,
						"{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"volume\":{\"level\":%f}}",
						Ctx->reqId++, (float) Volume / 100.0);

		SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL,
						"{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"volume\":{\"muted\":false}}",
						Ctx->reqId++);
	}
	else {
		SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL,
						"{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"volume\":{\"muted\":true}}",
						Ctx->reqId++);
	}

	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
int CastSeek(char *ControlURL, unsigned Interval)
{
	int rc = 0;

	return rc;
}





