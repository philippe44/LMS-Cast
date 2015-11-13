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

	SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
					"{\"type\":\"GET_STATUS\",\"requestId\":%d,\"mediaSessionId\":%d}",
					Ctx->reqId++, Ctx->mediaSessionId);

	pthread_mutex_unlock(&Ctx->Mutex);
}



/*----------------------------------------------------------------------------*/
void CastLoad(void *p, char *URI, char *ContentType, struct sq_metadata_s *MetaData, struct sMRConfig *Config)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	pthread_mutex_lock(&Ctx->reqMutex);

	if (Ctx->waitId) pthread_cond_wait(&Ctx->reqCond, &Ctx->reqMutex);

	Ctx->waitId = Ctx->reqId;
	pthread_mutex_unlock(&Ctx->reqMutex);
	pthread_mutex_lock(&Ctx->Mutex);

	SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"LOAD\",\"requestId\":%d,\"sessionId\":\"%s\",\"media\":{\"contentId\":\"%s\",\"streamType\":\"BUFFERED\",\"contentType\":\"%s\"},\"currentTime\":0.0,\"autoplay\":false}",
//						"{\"type\":\"LOAD\",\"requestId\":%d,\"sessionId\":\"%s\",\"media\":{\"contentId\":\"%s\",\"streamType\":\"LIVE\",\"contentType\":\"%s\"},\"autoplay\":false}",
					Ctx->reqId++, Ctx->sessionId, URI, ContentType);

	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
void CastPlay(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	pthread_mutex_lock(&Ctx->reqMutex);

	// lock on wait for a Cast response
	if (Ctx->waitId) pthread_cond_wait(&Ctx->reqCond, &Ctx->reqMutex);

	// no media session, nothing to do
	if (Ctx->mediaSessionId) {
		Ctx->waitId = Ctx->reqId;
		pthread_mutex_lock(&Ctx->Mutex);

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"PLAY\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->reqId++, Ctx->mediaSessionId);

		pthread_mutex_unlock(&Ctx->Mutex);
	}

	pthread_mutex_unlock(&Ctx->reqMutex);
}


/*----------------------------------------------------------------------------*/
void CastStop(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	pthread_mutex_lock(&Ctx->reqMutex);

	// lock on wait for a Cast response
	if (Ctx->waitId) pthread_cond_wait(&Ctx->reqCond, &Ctx->reqMutex);

	// no media session, nothing to do
	if (Ctx->mediaSessionId) {
		Ctx->waitId = Ctx->reqId;
		pthread_mutex_lock(&Ctx->Mutex);

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"STOP\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->reqId++, Ctx->mediaSessionId);

		pthread_mutex_unlock(&Ctx->Mutex);
		Ctx->mediaSessionId = 0;
	}

	pthread_mutex_unlock(&Ctx->reqMutex);
}


/*----------------------------------------------------------------------------*/
void CastPause(void *p)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	pthread_mutex_lock(&Ctx->reqMutex);

	// lock on wait for a Cast response
	if (Ctx->waitId) pthread_cond_wait(&Ctx->reqCond, &Ctx->reqMutex);

	// no media session, nothing to do
	if (Ctx->mediaSessionId) {
		Ctx->waitId = Ctx->reqId;
		pthread_mutex_lock(&Ctx->Mutex);

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"PAUSE\",\"requestId\":%d,\"mediaSessionId\":%d}",
						Ctx->reqId++, Ctx->mediaSessionId);

		pthread_mutex_unlock(&Ctx->Mutex);
	}

	pthread_mutex_unlock(&Ctx->reqMutex);
}

/*----------------------------------------------------------------------------*/
void SetVolume(void *p, u8_t Volume)
{
	tCastCtx *Ctx = (tCastCtx*) p;

	// no media session, nothing to do
	if (Ctx->mediaSessionId) {
		pthread_mutex_lock(&Ctx->Mutex);

		SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"mediaSessionId\":%d,\"volume\":{\"level\":%lf}}",
						Ctx->reqId++, Ctx->mediaSessionId, (double) Volume / 100.0);

		pthread_mutex_unlock(&Ctx->Mutex);
	}
}

/*----------------------------------------------------------------------------*/
int CastSeek(char *ControlURL, unsigned Interval)
{
	int rc = 0;

	return rc;
}


/*----------------------------------------------------------------------------*/
#if 0
char *CreateDIDL(char *URI, char *ProtInfo, struct sq_metadata_s *MetaData, struct sMRConfig *Config)
{
	char *s;
	u32_t Sinc = 0;
	char DLNAOpt[128];

	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Node	 *node, *root;

	root = XMLAddNode(doc, NULL, "DIDL-Lite", NULL);
	XMLAddAttribute(doc, root, "xmlns:dc", "http://purl.org/dc/elements/1.1/");
	XMLAddAttribute(doc, root, "xmlns:upnp", "urn:schemas-upnp-org:metadata-1-0/upnp/");
	XMLAddAttribute(doc, root, "xmlns", "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/");
	XMLAddAttribute(doc, root, "xmlns:dlna", "urn:schemas-dlna-org:metadata-1-0/");

	node = XMLAddNode(doc, root, "item", NULL);
	XMLAddAttribute(doc, node, "id", "1");
	XMLAddAttribute(doc, node, "parentID", "0");
	XMLAddAttribute(doc, node, "restricted", "1");
	XMLAddNode(doc, node, "dc:title", MetaData->title);
	XMLAddNode(doc, node, "dc:creator", MetaData->artist);
	XMLAddNode(doc, node, "upnp:genre", MetaData->genre);

	if (MetaData->artwork)
		XMLAddNode(doc, node, "upnp:albumArtURI", "%s", MetaData->artwork);

	if (MetaData->duration) {
		div_t duration 	= div(MetaData->duration, 1000);

		XMLAddNode(doc, node, "upnp:artist", MetaData->artist);
		XMLAddNode(doc, node, "upnp:album", MetaData->album);
		XMLAddNode(doc, node, "upnp:originalTrackNumber", "%d", MetaData->track);
		XMLAddNode(doc, node, "upnp:class", "object.item.audioItem.musicTrack");
		node = XMLAddNode(doc, node, "res", URI);
		XMLAddAttribute(doc, node, "duration", "%1d:%02d:%02d.%03d",
						duration.quot/3600, (duration.quot % 3600) / 60,
						duration.quot % 60, duration.rem);
	}
	else {
		Sinc = DLNA_ORG_FLAG_SN_INCREASE;
		XMLAddNode(doc, node, "upnp:channelName", MetaData->artist);
		XMLAddNode(doc, node, "upnp:channelNr", "%d", MetaData->track);
		XMLAddNode(doc, node, "upnp:class", "object.item.audioItem.audioBroadcast");
		node = XMLAddNode(doc, node, "res", URI);
	}

	if (Config->ByteSeek)
		sprintf(DLNAOpt, ";DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
						  DLNA_ORG_FLAG | Sinc);
	else
		sprintf(DLNAOpt, ";DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
						  DLNA_ORG_FLAG | Sinc);

	if (ProtInfo[strlen(ProtInfo) - 1] == ':')
		XMLAddAttribute(doc, node, "protocolInfo", "%s%s", ProtInfo, DLNAOpt + 1);
	else
		XMLAddAttribute(doc, node, "protocolInfo", "%s%s", ProtInfo, DLNAOpt);

	s = ixmlNodetoString((IXML_Node*) doc);

	ixmlDocument_free(doc);

	return s;
}
#endif



