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

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "squeezedefs.h"
#if WIN
#include <process.h>
#endif
#include "squeeze2cast.h"
#include "upnpdebug.h"
#include "upnptools.h"
#include "webserver.h"
#include "util_common.h"
#include "util.h"
#include "cast_util.h"
#include "castitf.h"

/*
TODO :
- for no pause, the solution will be to send the elapsed time to LMS through CLI so that it does take care of the seek
- samplerate management will have to be reviewed when decode will be used
*/

/*----------------------------------------------------------------------------*/
/* globals initialized */
/*----------------------------------------------------------------------------*/
char				glBaseVDIR[] = "LMS2CAST";
char				glSQServer[SQ_STR_LENGTH] = "?";
u8_t				glMac[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
sq_log_level_t		glLog = { lINFO, lINFO, lINFO, lINFO, lINFO, lINFO, lINFO, lINFO, lINFO};
#if LINUX || FREEBSD
bool				glDaemonize = false;
#endif
bool				glInteractive = true;
char				*glLogFile;
s32_t				glLogLimit = -1;
static char			*glPidFile = NULL;
static char			*glSaveConfigFile = NULL;
bool				glAutoSaveConfigFile = false;
bool				glGracefullShutdown = true;

tMRConfig			glMRConfig = {
							-2L,
							SQ_STREAM,
							true,
							"",
							-1,
							false,
							true,
							true,
							true,
							3,
							false,
					};

static u8_t LMSVolumeMap[101] = {
				0, 1, 1, 1, 2, 2, 2, 3,  3,  4,
				5, 5, 6, 6, 7, 8, 9, 9, 10, 11,
				12, 13, 14, 15, 16, 16, 17, 18, 19, 20,
				22, 23, 24, 25, 26, 27, 28, 29, 30, 32,
				33, 34, 35, 37, 38, 39, 40, 42, 43, 44,
				46, 47, 48, 50, 51, 53, 54, 56, 57, 59,
				60, 61, 63, 65, 66, 68, 69, 71, 72, 74,
				75, 77, 79, 80, 82, 84, 85, 87, 89, 90,
				92, 94, 96, 97, 99, 101, 103, 104, 106, 108, 110,
				112, 113, 115, 117, 119, 121, 123, 125, 127, 128
			};

sq_dev_param_t glDeviceParam = {
					 // both are multiple of 3*4(2) for buffer alignement on sample
					(200 * 1024 * (4*3)),
					(200 * 1024 * (4*3)),
					SQ_STREAM,
					{ 	SQ_RATE_384000, SQ_RATE_352000, SQ_RATE_192000, SQ_RATE_176400,
						SQ_RATE_96000, SQ_RATE_48000, SQ_RATE_44100,
						SQ_RATE_32000, SQ_RATE_24000, SQ_RATE_22500, SQ_RATE_16000,
						SQ_RATE_12000, SQ_RATE_11025, SQ_RATE_8000, 0 },
					-1,
					100,
					"flc,pcm,mp3",
					SQ_RATE_96000,
					L24_PACKED_LPCM,
					FLAC_NORMAL_HEADER,
					"?",
					-1L,
					0,
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
					false,
				} ;

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
static ithread_t 	glMainThread;
char				gluPNPSocket[128] = "?";
unsigned int 		glPort;
char 				glIPaddress[128] = "";
UpnpClient_Handle 	glControlPointHandle;
void				*glConfigID = NULL;
char				glConfigName[SQ_STR_LENGTH] = "./config.xml";
static bool			glDiscovery = false;
u32_t				gluPNPScanInterval = SCAN_INTERVAL;
u32_t				gluPNPScanTimeout = SCAN_TIMEOUT;
struct sMR			glMRDevices[MAX_RENDERERS];
ithread_mutex_t		glMRFoundMutex;

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/
static const char 	MEDIA_RENDERER[] 	= "urn:dial-multiscreen-org:device:dial:1";

static const char 	cLogitech[] 		= "Logitech";

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 	loglevel = lWARN;
ithread_t			glUpdateMRThread;
static bool			glMainRunning = true;
static struct sLocList {
	char 			*Location;
	struct sLocList *Next;
} *glMRFoundList = NULL;

static char usage[] =
			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server>[:<port>]\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
  		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output|web|upnp|main|sq2mr, level: info|debug|sdebug\n"
#if LINUX || FREEBSD
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -Z \t\t\tNOT interactive\n"
		   "  -k \t\t\tImmediate exit on SIGQUIT and SIGTERM\n"
		   "  -t \t\t\tLicense terms\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
		   "\n\n";

static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
#if DSD
		   "Contains dsd2pcm library Copyright 2009, 2011 Sebastian Gesemann which\n"
		   "is subject to its own license.\n\n"
#endif
	;

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void *MRThread(void *args);
static void *UpdateMRThread(void *args);
static bool AddCastDevice(struct sMR *Device, char * UDN, IXML_Document *DescDoc,	const char *location);
void 		DelCastDevice(struct sMR *Device);
static int	uPNPTerminate(void);

/*----------------------------------------------------------------------------*/
bool sq_callback(sq_dev_handle_t handle, void *caller, sq_action_t action, u8_t *cookie, void *param)
{
	struct sMR *device = caller;
	char *p = (char*) param;
	bool rc = true;

	if (!device)	{
		LOG_ERROR("No caller ID in callback", NULL);
		return false;
	}

	if (action == SQ_ONOFF) {
		device->on = *((bool*) param);

		if (device->on)
			ConnectCastDevice(device->CastCtx, device->ip);

		if (!device->on)
			DisconnectCastDevice(device->CastCtx);

		if (device->on && device->Config.AutoPlay)
			sq_notify(device->SqueezeHandle, device, SQ_PLAY, NULL, &device->on);

		LOG_DEBUG("[%p]: device set on/off %d", caller, device->on);
	}

	if (!device->on) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		return false;
	}

	LOG_SDEBUG("callback for %s", device->FriendlyName);
	ithread_mutex_lock(&device->Mutex);

	switch (action) {

		case SQ_SETURI:
		case SQ_SETNEXTURI: {
			sq_seturi_t *p = (sq_seturi_t*) param;
			char uri[SQ_STR_LENGTH];

			LOG_INFO("[%p]: codec:%c, ch:%d, s:%d, r:%d", device, p->codec,
										p->channels, p->sample_size, p->sample_rate);

			if (!SetContentType(device, p)) {
				LOG_ERROR("[%p]: no matching codec in player", caller);
				rc = false;
				break;
			}

			sprintf(uri, "http://%s:%d/%s/%s.%s", glIPaddress, glPort, glBaseVDIR, p->name, p->ext);

			if (device->Config.SendMetaData) {
				sq_get_metadata(device->SqueezeHandle, &device->MetaData, true);
				p->file_size = device->MetaData.file_size ?
							   device->MetaData.file_size : device->Config.StreamLength;
			}
			else {
				sq_default_metadata(&device->MetaData, true);
				p->file_size = device->Config.StreamLength;
			}

			p->duration 	= device->MetaData.duration;
			p->src_format 	= ext2format(device->MetaData.path);
			p->remote 		= device->MetaData.remote;
			p->track_hash	= device->MetaData.track_hash;
			if (!device->Config.SendCoverArt) NFREE(device->MetaData.artwork);

			if (action == SQ_SETNEXTURI) {
				NFREE(device->NextURI);
				strcpy(device->ContentType, p->content_type);

				if (device->Config.AcceptNextURI){
				/*
					AVTSetNextURI(device->Service[AVT_SRV_IDX].ControlURL, uri, p->proto_info,
								  &device->MetaData, &device->Config, device->seqN++);
				*/
					sq_free_metadata(&device->MetaData);
				}

				// to know what is expected next
				device->NextURI = (char*) malloc(strlen(uri) + 1);
				strcpy(device->NextURI, uri);
				LOG_INFO("[%p]: next URI set %s", device, device->NextURI);
			}
			else {
				// to detect properly transition
				NFREE(device->CurrentURI);
				NFREE(device->NextURI);

				CastLoad(device->CastCtx, uri, p->content_type, &device->MetaData, &device->Config);
				sq_free_metadata(&device->MetaData);

				device->CurrentURI = (char*) malloc(strlen(uri) + 1);
				strcpy(device->CurrentURI, uri);
				LOG_INFO("[%p]: current URI set %s", device, device->CurrentURI);
			}

			break;
      	}
		case SQ_UNPAUSE:
			if (device->CurrentURI) {
				CastPlay(device->CastCtx);
				device->sqState = SQ_PLAY;
			}
			break;
		case SQ_PLAY:
			if (device->CurrentURI) {
				device->StartTime = sq_get_time(device->SqueezeHandle);
				device->LocalStartTime = gettime_ms();
				CastPlay(device->CastCtx);
				if (device->Config.VolumeOnPlay != -1) SetVolume(device->CastCtx, device->Volume);
				device->sqState = SQ_PLAY;
			}
			else rc = false;
			break;
		case SQ_STOP:
			CastStop(device->CastCtx);
			NFREE(device->CurrentURI);
			NFREE(device->NextURI);
			device->sqState = action;
			break;
		case SQ_PAUSE:
			CastPause(device->CastCtx);
			device->sqState = action;
			break;
		case SQ_NEXT:
			break;
		case SQ_SEEK:
			break;
		case SQ_VOLUME: {
			u32_t Volume = *(u16_t*)p;
			int i;

			for (i = 100; Volume < LMSVolumeMap[i] && i; i--);

			device->Volume = i;
			device->PreviousVolume = device->Volume;

            // calculate but do not transmit so that we can compare
			if (device->Config.VolumeOnPlay == -1) break;

			SetVolume(device->CastCtx, device->Volume);

			break;
		}
		default:
			break;
	}

	ithread_mutex_unlock(&device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
void SyncNotifState(char *State, struct sMR* Device)
{
	struct sAction *Action = NULL;
	sq_event_t Event = SQ_NONE;
	bool Param = false;

	// an update can have happended that has destroyed the device
	if (!Device->InUse) return;

	if (!strcasecmp(State, "STOPPED")) {
		if (Device->State != STOPPED) {
			LOG_INFO("[%p]: Cast stop", Device);
			if (Device->NextURI && !Device->Config.AcceptNextURI) {

					// fake a "SETURI" and a "PLAY" request
					NFREE(Device->CurrentURI);
					Device->CurrentURI = malloc(strlen(Device->NextURI) + 1);
					strcpy(Device->CurrentURI, Device->NextURI);
					NFREE(Device->NextURI);

					CastLoad(Device->CastCtx, Device->CurrentURI, Device->ContentType, &Device->MetaData, &Device->Config);
					sq_free_metadata(&Device->MetaData);
					CastPlay(Device->CastCtx);

					Event = SQ_TRACK_CHANGE;
					LOG_INFO("[%p]: no gapless %s", Device, Device->CurrentURI);
			}
			else {
				// Can be a user stop, an error or a normal stop
				Event = SQ_STOP;
			}
		}

		Device->State = STOPPED;
	}

	if (!strcasecmp(State, "PLAYING")) {
		if (Device->State != PLAYING) {

			LOG_INFO("[%p]: Cast playing", Device);
			switch (Device->sqState) {
			case SQ_PAUSE:
				if (!Action || (Action->Action != SQ_PAUSE)) {
					Param = true;
				}
			case SQ_PLAY:
				Event = SQ_PLAY;
				break;
			default:
				/*
				can be a local playing after stop or a N-1 playing after a quick
				sequence of "next" when a N stop has been sent ==> ignore it
				*/
				LOG_ERROR("[%p]: unhandled playing", Device);
				break;
			}

			Device->State = PLAYING;
		}
	}

	if (!strcasecmp(State, "PAUSED")) {
		/*
		STOPPED ==> PAUSED is not a valid transition but Cast device start
		playing in PAUSE state
		*/
		if (Device->State != PAUSED && Device->State != STOPPED) {

			// detect unsollicited pause, but do not confuse it with a fast pause/play
			if (Device->sqState != SQ_PAUSE && (!Action || (Action->Action != SQ_PLAY && Action->Action != SQ_UNPAUSE))) {
				Event = SQ_PAUSE;
				Param = true;
			}
			LOG_INFO("%s: Cast pause", Device->FriendlyName);

			if ((Device->Config.VolumeOnPlay != -1) && (!Device->Config.PauseVolume))
				SetVolume(Device->CastCtx, Device->PreviousVolume);

			Device->State = PAUSED;
		}
	}

	/*
	Squeeze "domain" execution has the right to consume own mutexes AND callback
	upnp "domain" function that will consume upnp "domain" mutex, but the reverse
	cannot be true otherwise deadlocks will occur
	*/
	if (Event != SQ_NONE)
		sq_notify(Device->SqueezeHandle, Device, Event, NULL, &Param);
}


/*----------------------------------------------------------------------------*/
void ProcessVolume(char *Volume, struct sMR* Device)
{
	u16_t UPnPVolume = atoi(Volume);

	LOG_SDEBUG("[%p]: Volume %s", Device, Volume);

	// do not report Volume set to 0 on pause
	if (UPnPVolume != Device->Volume &&	(UPnPVolume != 0 || (Device->sqState != SQ_PAUSE && Device->State != PAUSED))) {
		LOG_INFO("[%p]: UPnP Volume local change %d", Device, UPnPVolume);
		UPnPVolume =  UPnPVolume;
		sq_notify(Device->SqueezeHandle, Device, SQ_VOLUME, NULL, &UPnPVolume);
	}
}


/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define ALIVE_POLL  (3000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed;
	unsigned last;
	struct sMR *p = (struct sMR*) args;
	json_t *data;

	last = gettime_ms();

	while (p->Running) {
		data = GetTimedEvent(p->CastCtx, 500);
		elapsed = gettime_ms() - last;
		ithread_mutex_lock(&p->Mutex);

		LOG_SDEBUG("Cast thread timer %d", elapsed);

		// a message has been received
		if (data) {
			json_t *val = json_object_get(data, "type");
			char *type = json_string_value(val);

			if (type && !strcasecmp(type, "MEDIA_STATUS")) {
				char *state = GetMediaItem_S(data, 0, "playerState");

				if (state && (!strcasecmp(state, "PLAYING") || !strcasecmp(state, "PAUSED"))) {
					SyncNotifState(state, p);
				}

				if (state && !strcasecmp(state, "IDLE")) {
					char *cause = GetMediaItem_S(data, 0, "idleReason");
					if (cause) SyncNotifState("STOPPED", p);
					NFREE(cause);
				}

				NFREE(state);

				/*
				Discard any time info unless we are confirmed playing. Cast
				devices seems to report time according to seekpoint, so in case
				difference is too large, it means that we have a LMS repositioning
				*/
				if (p->State == PLAYING) {
					u32_t elapsed = 1000L * GetMediaItem_F(data, 0, "currentTime");
					if (elapsed > gettime_ms() - p->LocalStartTime + 5000) elapsed -= p->StartTime;
					sq_notify(p->SqueezeHandle, p, SQ_TIME, NULL, &elapsed);
				}

			}
			json_decref(data);
		}

		// Cast devices need a keep-alive every < 5s
		p->KeepAlive += elapsed;
		if (p->on && p->KeepAlive > ALIVE_POLL) {
			p->KeepAlive = 0;
			CastKeepAlive(p->CastCtx);
		}

		// make sure that both domains are in sync that nothing shall be done
		if (!p->on || (p->sqState == SQ_STOP && p->State == STOPPED) ||
			 p->ErrorCount > MAX_ACTION_ERRORS) {
			ithread_mutex_unlock(&p->Mutex);
			last = gettime_ms();
			continue;
		}

		// get track position & CurrentURI
		p->TrackPoll += elapsed;
		if (p->TrackPoll > TRACK_POLL) {
			p->TrackPoll = 0;
			// if (p->State != STOPPED && p->State != PAUSED) {
			CastGetMediaStatus(p->CastCtx);
			//	}
		}

		ithread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
int CallbackEventHandler(Upnp_EventType EventType, void *Event, void *Cookie)
{
	LOG_SDEBUG("event: %i [%s] [%p]", EventType, uPNPEvent2String(EventType), Cookie);

	switch ( EventType ) {
		case UPNP_DISCOVERY_SEARCH_RESULT: {
			struct Upnp_Discovery *d_event = (struct Upnp_Discovery *) Event;
			struct sLocList **p, *prev = NULL;

			LOG_DEBUG("Answer to uPNP search %d", d_event->Location);
			if (d_event->ErrCode != UPNP_E_SUCCESS) {
				LOG_SDEBUG("Error in Discovery Callback -- %d", d_event->ErrCode);
				break;
			}

			ithread_mutex_lock(&glMRFoundMutex);
			p = &glMRFoundList;
			while (*p) {
				prev = *p;
				p = &((*p)->Next);
			}
			(*p) = (struct sLocList*) malloc(sizeof (struct sLocList));
			(*p)->Location = strdup(d_event->Location);
			(*p)->Next = NULL;
			if (prev) prev->Next = *p;
			ithread_mutex_unlock(&glMRFoundMutex);
			break;
		}
		case UPNP_DISCOVERY_SEARCH_TIMEOUT:	{
			pthread_attr_t attr;

			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 32*1024);
			pthread_create(&glUpdateMRThread, &attr, &UpdateMRThread, NULL);
			pthread_detach(glUpdateMRThread);
			pthread_attr_destroy(&attr);
			break;
		}
		case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		case UPNP_EVENT_RECEIVED:
		case UPNP_CONTROL_GET_VAR_COMPLETE:
		case UPNP_CONTROL_ACTION_COMPLETE:
		case UPNP_EVENT_AUTORENEWAL_FAILED:
		case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
		case UPNP_EVENT_RENEWAL_COMPLETE:
		case UPNP_EVENT_SUBSCRIBE_COMPLETE:
		case UPNP_EVENT_SUBSCRIPTION_REQUEST:
		case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
		case UPNP_CONTROL_ACTION_REQUEST:
		case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		case UPNP_CONTROL_GET_VAR_REQUEST:
		break;
	}

	Cookie = Cookie;
	return 0;
}


/*----------------------------------------------------------------------------*/
static bool RefreshTO(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].InUse && !strcmp(glMRDevices[i].UDN, UDN)) {
			glMRDevices[i].UPnPTimeOut = false;
			glMRDevices[i].UPnPMissingCount = glMRDevices[i].Config.UPnPRemoveCount;
			glMRDevices[i].ErrorCount = 0;
			return true;
		}
	}
	return false;
}


/*----------------------------------------------------------------------------*/
static void *UpdateMRThread(void *args)
{
	struct sLocList *p, *m;
	struct sMR *Device = NULL;
	int i, TimeStamp;

	LOG_DEBUG("Begin Cast devices update", NULL);
	TimeStamp = gettime_ms();

	// first add any newly found uPNP renderer
	ithread_mutex_lock(&glMRFoundMutex);
	m = p = glMRFoundList;
	glMRFoundList = NULL;
	ithread_mutex_unlock(&glMRFoundMutex);

	if (!glMainRunning) {
		LOG_DEBUG("Aborting ...", NULL);
		while (p) {
			m = p->Next;
			free(p->Location); free(p);
			p = m;
		}
		return NULL;
	}

	while (p) {
		IXML_Document *DescDoc = NULL;
		char *UDN = NULL, *Manufacturer = NULL;
		int rc;
		void *n = p->Next;

		rc = UpnpDownloadXmlDoc(p->Location, &DescDoc);
		if (rc != UPNP_E_SUCCESS) {
			LOG_DEBUG("Error obtaining description %s -- error = %d\n", p->Location, rc);
			if (DescDoc) ixmlDocument_free(DescDoc);
			p = n;
			continue;
		}

		Manufacturer = XMLGetFirstDocumentItem(DescDoc, "manufacturer");
		UDN = XMLGetFirstDocumentItem(DescDoc, "UDN");
		if (!strstr(Manufacturer, cLogitech) && !RefreshTO(UDN)) {
			// new device so search a free spot.
			for (i = 0; i < MAX_RENDERERS && glMRDevices[i].InUse; i++)

			// no more room !
			if (i == MAX_RENDERERS) {
				LOG_ERROR("Too many Cast devices", NULL);
				NFREE(UDN); NFREE(Manufacturer);
				break;
			}

			Device = &glMRDevices[i];
			if (AddCastDevice(Device, UDN, DescDoc, p->Location) && !glSaveConfigFile) {
				// create a new slimdevice
				Device->SqueezeHandle = sq_reserve_device(Device, &sq_callback);
				if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle,
					*(Device->Config.Name) ? Device->Config.Name : Device->FriendlyName,
					&Device->sq_config)) {
					sq_release_device(Device->SqueezeHandle);
					Device->SqueezeHandle = 0;
					LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
					DelCastDevice(Device);
				}
			}
		}

		if (DescDoc) ixmlDocument_free(DescDoc);
		NFREE(UDN);	NFREE(Manufacturer);
		p = n;
	}

	// free the list of discovered location URL's
	p = m;
	while (p) {
		m = p->Next;
		free(p->Location); free(p);
		p = m;
	}

	// then walk through the list of devices to remove missing ones
	for (i = 0; i < MAX_RENDERERS; i++) {
		Device = &glMRDevices[i];
		if (!Device->InUse || !Device->UPnPTimeOut ||
			!Device->UPnPMissingCount || --Device->UPnPMissingCount) continue;

		LOG_INFO("[%p]: removing renderer (%s)", Device, Device->FriendlyName);
		if (Device->SqueezeHandle) sq_delete_device(Device->SqueezeHandle);
		DelCastDevice(Device);
	}

	glDiscovery = true;
	if (glAutoSaveConfigFile && !glSaveConfigFile) {
		LOG_DEBUG("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	LOG_DEBUG("End Cast devices update %d", gettime_ms() - TimeStamp);
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	unsigned last = gettime_ms();
	int ScanPoll = 0;

	while (glMainRunning) {
		int i, rc;
		int elapsed = gettime_ms() - last;

		// reset timeout and re-scan devices
		ScanPoll += elapsed;
		if (gluPNPScanInterval && ScanPoll > gluPNPScanInterval*1000) {
			ScanPoll = 0;

			for (i = 0; i < MAX_RENDERERS; i++) {
				glMRDevices[i].UPnPTimeOut = true;
				glDiscovery = false;
			}

			// launch a new search for Media Renderer
			rc = UpnpSearchAsync(glControlPointHandle, gluPNPScanTimeout, MEDIA_RENDERER, NULL);
			if (UPNP_E_SUCCESS != rc) LOG_ERROR("Error sending search update%d", rc);
		}

		if (glLogFile && glLogLimit != - 1) {
			u32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				u32_t Sum, BufSize = 16384;
				u8_t *buf = malloc(BufSize);

				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
            }
		}

		last = gettime_ms();
		sleep(1);
	}
	return NULL;
}


/*----------------------------------------------------------------------------*/
int uPNPInitialize(char *IPaddress, unsigned int *Port)
{
	int rc;
	struct UpnpVirtualDirCallbacks VirtualDirCallbacks;

	if (gluPNPScanInterval) {
		if (gluPNPScanInterval < SCAN_INTERVAL) gluPNPScanInterval = SCAN_INTERVAL;
		if (gluPNPScanTimeout < SCAN_TIMEOUT) gluPNPScanTimeout = SCAN_TIMEOUT;
		if (gluPNPScanTimeout > gluPNPScanInterval - SCAN_TIMEOUT) gluPNPScanTimeout = gluPNPScanInterval - SCAN_TIMEOUT;
	}

	ithread_mutex_init(&glMRFoundMutex, 0);
	memset(&glMRDevices, 0, sizeof(glMRDevices));

	UpnpSetLogLevel(UPNP_ALL);
	if (*IPaddress) rc = UpnpInit(IPaddress, *Port);
	else rc = UpnpInit(NULL, *Port);
	UpnpSetMaxContentLength(60000);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("uPNP init failed: %d\n", rc);
		UpnpFinish();
		return false;
	}

	if (!*IPaddress) {
		strcpy(IPaddress, UpnpGetServerIpAddress());
	}
	if (!*Port) {
		*Port = UpnpGetServerPort();
	}

	LOG_INFO("uPNP init success - %s:%u", IPaddress, *Port);

	rc = UpnpRegisterClient(CallbackEventHandler,
				&glControlPointHandle, &glControlPointHandle);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering ControlPoint: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("ControlPoint registered", NULL);
	}

	rc = UpnpEnableWebserver(true);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error initalizing WebServer: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("WebServer enabled", NULL);
	}

	rc = UpnpAddVirtualDir(glBaseVDIR);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error setting VirtualDir: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("VirtualDir set for Squeezelite", NULL);
	}

	VirtualDirCallbacks.get_info = WebGetInfo;
	VirtualDirCallbacks.open = WebOpen;
	VirtualDirCallbacks.read  = WebRead;
	VirtualDirCallbacks.seek = WebSeek;
	VirtualDirCallbacks.close = WebClose;
	VirtualDirCallbacks.write = WebWrite;
	rc = UpnpSetVirtualDirCallbacks(&VirtualDirCallbacks);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering VirtualDir callbacks: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("Callbacks registered for VirtualDir", NULL);
	}

	/* start the main thread */
	ithread_create(&glMainThread, NULL, &MainThread, NULL);
	return true;
}

/*----------------------------------------------------------------------------*/
int uPNPTerminate(void)
{
	LOG_DEBUG("terminate main thread ...", NULL);
	ithread_join(glMainThread, NULL);
	LOG_DEBUG("un-register libupnp callbacks ...", NULL);
	UpnpUnRegisterClient(glControlPointHandle);
	LOG_DEBUG("disable webserver ...", NULL);
	UpnpRemoveVirtualDir(glBaseVDIR);
	UpnpEnableWebserver(false);
	LOG_DEBUG("end libupnp ...", NULL);
	UpnpFinish();

	return true;
}

/*----------------------------------------------------------------------------*/
int uPNPSearchMediaRenderer(void)
{
	int rc;

	/* search for (Media Render and wait 15s */
	glDiscovery = false;
	rc = UpnpSearchAsync(glControlPointHandle, SCAN_TIMEOUT, MEDIA_RENDERER, NULL);

	if (UPNP_E_SUCCESS != rc) {
		LOG_ERROR("Error sending uPNP search request%d", rc);
		return false;
	}
	return true;
}


/*----------------------------------------------------------------------------*/
static bool AddCastDevice(struct sMR *Device, char *UDN, IXML_Document *DescDoc, const char *location)
{
	char *deviceType = NULL;
	char *friendlyName = NULL;
	char *URLBase = NULL;
	char *presURL = NULL;
	char *manufacturer = NULL;
	u32_t mac_size = 6;
	pthread_attr_t attr;

	// read parameters from default then config file
	memset(Device, 0, sizeof(struct sMR));
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, UDN, &Device->Config, &Device->sq_config);
	if (!Device->Config.Enabled) return false;

	// Read key elements from description document
	deviceType = XMLGetFirstDocumentItem(DescDoc, "deviceType");
	friendlyName = XMLGetFirstDocumentItem(DescDoc, "friendlyName");
	URLBase = XMLGetFirstDocumentItem(DescDoc, "URLBase");
	presURL = XMLGetFirstDocumentItem(DescDoc, "presentationURL");
	manufacturer = XMLGetFirstDocumentItem(DescDoc, "manufacturer");

	LOG_SDEBUG("UDN:\t%s\nDeviceType:\t%s\nFriendlyName:\t%s", UDN, deviceType, friendlyName);

	if (presURL) {
		char UsedPresURL[200] = "";
		UpnpResolveURL((URLBase ? URLBase : location), presURL, UsedPresURL);
		strcpy(Device->PresURL, UsedPresURL);
	}
	else strcpy(Device->PresURL, "");

	LOG_INFO("[%p]: adding renderer (%s)", Device, friendlyName);

	ithread_mutex_init(&Device->Mutex, 0);
	Device->Magic = MAGIC;
	Device->UPnPTimeOut = false;
	Device->UPnPMissingCount = Device->Config.UPnPRemoveCount;
	Device->on = false;
	Device->SqueezeHandle = 0;
	Device->ErrorCount = 0;
	Device->Running = true;
	Device->InUse = true;
	Device->sqState = SQ_STOP;
	Device->State = STOPPED;
	strcpy(Device->UDN, UDN);
	strcpy(Device->DescDocURL, location);
	strcpy(Device->FriendlyName, friendlyName);
	strcpy(Device->Manufacturer, manufacturer);
	Device->CastCtx = InitCastCtx(Device);

	ExtractIP(location, &Device->ip);
	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", mac_size) &&
		SendARP(*((in_addr_t*) &Device->ip), INADDR_ANY, Device->sq_config.mac, &mac_size)) {
		LOG_ERROR("[%p]: cannot get mac %s", Device, Device->FriendlyName);
		// not sure what SendARP does with the MAC if it does not find one
		memset(Device->sq_config.mac, 0, sizeof(Device->sq_config.mac));
	}



	NFREE(deviceType);
	NFREE(friendlyName);
	NFREE(URLBase);
	NFREE(presURL);
	NFREE(manufacturer);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 32*1024);
	pthread_create(&Device->Thread, &attr, &MRThread, Device);
	pthread_attr_destroy(&attr);

	return true;
}


/*----------------------------------------------------------------------------*/
void FlushCastDevices(void)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->InUse) {
			if (p->sqState == SQ_PLAY || p->sqState == SQ_PAUSE) CastStop(p->CastCtx);
			DelCastDevice(p);
		}
	}
}


/*----------------------------------------------------------------------------*/
void DelCastDevice(struct sMR *Device)
{
	pthread_mutex_lock(&Device->Mutex);
	DisconnectCastDevice(Device->CastCtx);
	Device->Running = false;
	Device->InUse = false;
	pthread_mutex_unlock(&Device->Mutex);
	pthread_join(Device->Thread, NULL);

	CloseCastCtx(Device->CastCtx);
	NFREE(Device->CurrentURI);
	NFREE(Device->NextURI);

	pthread_mutex_destroy(&Device->Mutex);
	memset(Device, 0, sizeof(struct sMR));
}

/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	InitSSL();
	if (!uPNPInitialize(glIPaddress, &glPort)) return false;
	uPNPSearchMediaRenderer();
	return true;
}

static bool Stop(void)
{
	struct sLocList *p, *m;

	LOG_DEBUG("flush renderers ...", NULL);
	FlushCastDevices();
	LOG_DEBUG("terminate libupnp ...", NULL);
	uPNPTerminate();
	EndSSL();

	ithread_mutex_lock(&glMRFoundMutex);
	m = p = glMRFoundList;
	glMRFoundList = NULL;
	ithread_mutex_unlock(&glMRFoundMutex);
	while (p) {
		m = p->Next;
		free(p->Location); free(p);
		p = m;
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	int i;

	glMainRunning = false;

	if (!glGracefullShutdown) {
		for (i = 0; i < MAX_RENDERERS; i++) {
			struct sMR *p = &glMRDevices[i];
			if (p->InUse && p->sqState == SQ_PLAY) CastStop(p);
		}
		LOG_INFO("forced exit", NULL);
		exit(EXIT_SUCCESS);
	}

	sq_stop();
	Stop();
	exit(EXIT_SUCCESS);
}


/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	int i;

#define MAXCMDLINE 256
	char cmdline[MAXCMDLINE] = "";

	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < MAXCMDLINE); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("stxdfpi", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIk"
#if RESAMPLE
						  "uR"
#endif
#if DSD
						  "D"
#endif
		  , opt)) {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case 's':
			strcpy(glSQServer, optarg);
			break;
#if RESAMPLE
		case 'u':
		case 'R':
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				gl_resample = argv[optind++];
			} else {
				gl_resample = "";
			}
			break;
#endif
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			glSaveConfigFile = optarg;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;

#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
#if DSD
		case 'D':
			gl_dop = true;
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				gl_dop_delay = atoi(argv[optind++]);
			}
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto")) glLog.slimproto = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    glLog.stream = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    glLog.decode = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    glLog.output = new;
					if (!strcmp(l, "all") || !strcmp(l, "web")) glLog.web = new;
					if (!strcmp(l, "all") || !strcmp(l, "upnp"))    glLog.upnp = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))    glLog.main = new;
					if (!strcmp(l, "all") || !strcmp(l, "sq2mr"))    glLog.sq2mr = new;

				}
				else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		default:
			break;
		}
	}

	return true;
}


/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int i;
	char resp[20] = "";
	char *tmpdir;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting squeeze2cast version: %s\n", VERSION);

	if (!glConfigID) {
		LOG_ERROR("\n\n!!!!!!!!!!!!!!!!!! ERROR LOADING CONFIG FILE !!!!!!!!!!!!!!!!!!!!!\n", NULL);
	}

#if LINUX || FREEBSD
	if (glDaemonize && !glSaveConfigFile) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", getpid());
			fclose(pid_file);
		}
		else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	if (strstr(glSQServer, "?")) sq_init(NULL, glMac, &glLog);
	else sq_init(glSQServer, glMac, &glLog);

	loglevel = glLog.sq2mr;
	WebServerLogLevel(glLog.web);
	CastInit(glLog.sq2mr);
	UtilInit(glLog.sq2mr);

	tmpdir = malloc(SQ_STR_LENGTH);
	GetTempPath(SQ_STR_LENGTH, tmpdir);
	LOG_INFO("Buffer path %s", tmpdir);
	free(tmpdir);

	if (!strstr(gluPNPSocket, "?")) {
		sscanf(gluPNPSocket, "%[^:]:%u", glIPaddress, &glPort);
	}

	if (!Start()) {
		LOG_ERROR("Cannot start uPnP", NULL);
		strcpy(resp, "exit");
	}

	if (glSaveConfigFile) {
		while (!glDiscovery) sleep(1);
		SaveConfig(glSaveConfigFile, glConfigID, true);
	}

	while (strcmp(resp, "exit") && !glSaveConfigFile) {

#if LINUX || FREEBSD
		if (!glDaemonize && glInteractive)
			i = scanf("%s", resp);
		else
			pause();
#else
		if (glInteractive)
			i = scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif

		if (!strcmp(resp, "sdbg"))	{
			char level[20];
			i = scanf("%s", level);
			stream_loglevel(debug2level(level));
		}

		if (!strcmp(resp, "odbg"))	{
			char level[20];
			i = scanf("%s", level);
			output_mr_loglevel(debug2level(level));
		}

		if (!strcmp(resp, "pdbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimproto_loglevel(debug2level(level));
		}

		if (!strcmp(resp, "wdbg"))	{
			char level[20];
			i = scanf("%s", level);
			WebServerLogLevel(debug2level(level));
		}

		if (!strcmp(resp, "mdbg"))	{
			char level[20];
			i = scanf("%s", level);
			main_loglevel(debug2level(level));
		}

		if (!strcmp(resp, "qdbg"))	{
			char level[20];
			i = scanf("%s", level);
			LOG_ERROR("Squeeze change log", NULL);
			loglevel = debug2level(level);
		}

		 if (!strcmp(resp, "save"))	{
			char name[128];
			i = scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}
	}

	if (glConfigID) ixmlDocument_free(glConfigID);
	glMainRunning = false;
	LOG_INFO("stopping squeelite devices ...", NULL);
	sq_stop();
	LOG_INFO("stopping Cast devices ...", NULL);
	Stop();
	LOG_INFO("all done", NULL);

	return true;
}




