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
#include "log_util.h"
#include "util.h"
#include "cast_util.h"
#include "castitf.h"
#include "mdnssd-itf.h"


/*
TODO :
- for no pause, the solution will be to send the elapsed time to LMS through CLI so that it does take care of the seek
- samplerate management will have to be reviewed when decode will be used
*/

/*----------------------------------------------------------------------------*/
/* globals initialized */
/*----------------------------------------------------------------------------*/
char				glBaseVDIR[] = "LMS2CAST";

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
int					gl_mDNSId;

log_level	slimproto_loglevel = lINFO;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lWARN;
log_level	web_loglevel = lWARN;
log_level	main_loglevel = lINFO;
log_level	slimmain_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	cast_loglevel = lINFO;

tMRConfig			glMRConfig = {
							-2L,
							SQ_STREAM,
							true,
							false,
							"",
							1,
							false,
							true,
							true,
							3,
							false,
							50,
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
					"pcm,aif,flc,mp3",
					"?",
					SQ_RATE_96000,
					L24_PACKED_LPCM,
					FLAC_NORMAL_HEADER,
					"?",
					"",
					-1L,
					1024*1024L,
					0,
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
					false,
					true,
				} ;

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
static ithread_t 	glMainThread;
char				glUPnPSocket[128] = "?";
unsigned int 		glPort = 0;
char 				glIPaddress[128] = "";
void				*glConfigID = NULL;
char				glConfigName[SQ_STR_LENGTH] = "./config.xml";
static bool			glDiscovery = false;
u32_t				glScanInterval = SCAN_INTERVAL;
u32_t				glScanTimeout = SCAN_TIMEOUT;
struct sMR			glMRDevices[MAX_RENDERERS];

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/

static log_level 	*loglevel = &main_loglevel;
ithread_t			glUpdateMRThread;
static bool			glMainRunning = true;

static char usage[] =
			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server[:port]>\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -b <address[:port]>]\tNetwork address and port to bind to\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
  		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output|web|main|util|cast, level: error|warn|info|debug|sdebug\n"
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
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool Group, struct in_addr ip, u16_t port);
void 		DelCastDevice(struct sMR *Device);
static int	Terminate(void);
static int  Initialize(void);

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

		LOG_INFO("[%p]: device set %s", caller, device->on ? "ON" : "OFF");

		if (device->on) {
			CastPowerOn(device->CastCtx);
			if (device->Config.AutoPlay) sq_notify(device->SqueezeHandle, device, SQ_PLAY, NULL, &device->on);
		} else {
			// cannot disconnect when LMS is configured for pause when OFF
			if (device->sqState == SQ_STOP) CastPowerOff(device->CastCtx);
		}
	}

	if (!device->on && action != SQ_SETNAME && action != SQ_SETSERVER) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		return false;
	}

	LOG_SDEBUG("callback for %s (%d)", device->FriendlyName, action);
	ithread_mutex_lock(&device->Mutex);

	switch (action) {

		case SQ_SETURI:
		case SQ_SETNEXTURI: {
			sq_seturi_t *p = (sq_seturi_t*) param;
			char uri[SQ_STR_LENGTH];

			LOG_INFO("[%p]: codec:%c, ch:%d, s:%d, r:%d", device, p->codec,
										p->channels, p->sample_size, p->sample_rate);

			sq_get_metadata(device->SqueezeHandle, &device->MetaData, (action == SQ_SETURI) ? false : true);
			p->file_size = device->MetaData.file_size ?
						   device->MetaData.file_size : device->Config.StreamLength;

			p->duration 	= device->MetaData.duration;
			p->src_format 	= ext2format(device->MetaData.path);
			p->remote 		= device->MetaData.remote;
			p->track_hash	= device->MetaData.track_hash;
			if (!device->Config.SendCoverArt) NFREE(device->MetaData.artwork);

			// must be done after the above
			if (!SetContentType(device, p)) {
				LOG_ERROR("[%p]: no matching codec in player", caller);
				sq_free_metadata(&device->MetaData);
				rc = false;
				break;
			}

			sprintf(uri, "http://%s:%d/%s/%s.%s", glIPaddress, glPort, glBaseVDIR, p->name, p->ext);

			if (action == SQ_SETNEXTURI) {
				NFREE(device->NextURI);
				strcpy(device->ContentType, p->content_type);

				// to know what is expected next
				device->NextURI = (char*) malloc(strlen(uri) + 1);
				strcpy(device->NextURI, uri);
				LOG_INFO("[%p]: next URI set %s", device, device->NextURI);
			}
			else {
				// to detect properly transition
				NFREE(device->CurrentURI);
				NFREE(device->NextURI);

				rc = CastLoad(device->CastCtx, uri, p->content_type,
							  (device->Config.SendMetaData) ? &device->MetaData : NULL);

				sq_free_metadata(&device->MetaData);

				device->CurrentURI = (char*) malloc(strlen(uri) + 1);
				strcpy(device->CurrentURI, uri);
				LOG_INFO("[%p]: current URI set %s", device, device->CurrentURI);
			}

			break;
		}
		case SQ_UNPAUSE:
			if (device->CurrentURI) {
				if (device->Config.VolumeOnPlay == 1)
					CastSetDeviceVolume(device->CastCtx, device->Volume);

				CastPlay(device->CastCtx);
				device->sqState = SQ_PLAY;
			}
			break;
		case SQ_PLAY:
			if (device->CurrentURI) {
#if !defined(REPOS_TIME)
				device->StartTime = sq_get_time(device->SqueezeHandle);
				device->LocalStartTime = gettime_ms();
#endif
				if (device->Config.VolumeOnPlay == 1)
					CastSetDeviceVolume(device->CastCtx, device->Volume);

				CastPlay(device->CastCtx);
				device->sqState = SQ_PLAY;
				device->sqStamp = gettime_ms();
			}
			else rc = false;
			break;
		case SQ_STOP:
			CastStop(device->CastCtx);
			NFREE(device->CurrentURI);
			NFREE(device->NextURI);
			device->sqState = action;
			device->sqStamp = gettime_ms();
			break;
		case SQ_PAUSE:
			CastPause(device->CastCtx);
			device->sqState = action;
			device->sqStamp = gettime_ms();
			break;
		case SQ_NEXT:
			break;
		case SQ_SEEK:
			break;
		case SQ_VOLUME: {
			u32_t Volume = *(u16_t*)p;
			u32_t now = gettime_ms();
			int i;

			for (i = 100; Volume < LMSVolumeMap[i] && i; i--);

			device->Volume = i;
			LOG_INFO("Volume %d", i);

			if ((now > device->VolumeStamp + 1000 || now < device->VolumeStamp) &&
				(!device->Config.VolumeOnPlay || (device->Config.VolumeOnPlay == 1 && device->sqState == SQ_PLAY)))
				CastSetDeviceVolume(device->CastCtx, device->Volume);

			break;
		}
		case SQ_SETNAME:
			strcpy(device->sq_config.name, param);
			break;
		case SQ_SETSERVER:
			strcpy(device->sq_config.server, inet_ntoa(*(struct in_addr*) param));
			break;
		default:
			break;
	}

	ithread_mutex_unlock(&device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
void SyncNotifState(const char *State, struct sMR* Device)
{
	sq_event_t Event = SQ_NONE;
	bool Param = false;
	u32_t now = gettime_ms();

	// an update can have happended that has destroyed the device
	if (!Device->InUse) return;

	if (!strcasecmp(State, "CLOSED")) {
		Device->State = STOPPED;
		Param = true;
		Event = SQ_STOP;
	}

	if (!strcasecmp(State, "STOPPED")) {
		if (Device->State != STOPPED) {
			LOG_INFO("[%p]: Cast stop", Device);
			if (Device->NextURI && !Device->Config.AcceptNextURI) {

				// fake a "SETURI" and a "PLAY" request
				NFREE(Device->CurrentURI);
				Device->CurrentURI = malloc(strlen(Device->NextURI) + 1);
				strcpy(Device->CurrentURI, Device->NextURI);
				NFREE(Device->NextURI);

				CastLoad(Device->CastCtx, Device->CurrentURI, Device->ContentType,
						  (Device->Config.SendMetaData) ? &Device->MetaData : NULL);
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
				Param = true;
			case SQ_PLAY:
				if (now > Device->sqStamp + 2000) Event = SQ_PLAY;
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
		Cast devices start "paused" so there is a first status received with
		that state, even if the play request has been sent. Make sure that
		this is filtered out and that "pause" state are only taken into account
		when already playing
		*/
		if (Device->State == PLAYING) {
			// detect unsollicited pause, but do not confuse it with a fast pause/play
			if (Device->sqState != SQ_PAUSE && now > Device->sqStamp + 2000) {
				Event = SQ_PAUSE;
				Param = true;
			}
			LOG_INFO("%s: Cast pause", Device->FriendlyName);

			Device->State = PAUSED;
		}
	}

	/*
	Squeeze "domain" execution has the right to consume own mutexes AND callback
	cast "domain" function that will consume upnp "domain" mutex, but the reverse
	cannot be true otherwise deadlocks will occur
	*/
	if (Event != SQ_NONE)
		sq_notify(Device->SqueezeHandle, Device, Event, NULL, &Param);
}



/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed;
	unsigned last;
	struct sMR *p = (struct sMR*) args;
	json_t *data;
	u16_t Volume = 0xff;

	last = gettime_ms();

	while (p->Running) {
		data = GetTimedEvent(p->CastCtx, 500);
		elapsed = gettime_ms() - last;
		ithread_mutex_lock(&p->Mutex);

		LOG_SDEBUG("Cast thread timer %d", elapsed);

		// make sure that both domains are in sync that nothing shall be done
		if (!p->on) {
			ithread_mutex_unlock(&p->Mutex);
			last = gettime_ms();
			continue;
		}

		// a message has been received
		if (data) {
			json_t *val = json_object_get(data, "type");
			const char *type = json_string_value(val);

			// a mediaSessionId has been acquired
			if (type && !strcasecmp(type, "MEDIA_STATUS")) {
				const char *state = GetMediaItem_S(data, 0, "playerState");

				if (state && (!strcasecmp(state, "PLAYING") || !strcasecmp(state, "BUFFERING"))) {
					SyncNotifState("PLAYING", p);
				}

				if (state && !strcasecmp(state, "PAUSED")) {
					SyncNotifState("PAUSED", p);
				}

				if (state && !strcasecmp(state, "IDLE")) {
					const char *cause = GetMediaItem_S(data, 0, "idleReason");
					if (cause) SyncNotifState("STOPPED", p);
				}

				/*
				Discard any time info unless we are confirmed playing. Cast
				devices seems to report time according to seekpoint, so in case
				difference is too large, it means that we have a LMS repositioning
				*/
				if (p->State == PLAYING && p->sqState == SQ_PLAY && CastIsMediaSession(p->CastCtx)) {
					u32_t elapsed = 1000L * GetMediaItem_F(data, 0, "currentTime");
#if !defined(REPOS_TIME)
					// LMS reposition time can be a bit BEFORE seek time ...
					if (elapsed > gettime_ms() - p->LocalStartTime + 5000) {
						if (elapsed > p->StartTime)	elapsed -= p->StartTime;
						else elapsed = 0;
					}
#endif
					sq_notify(p->SqueezeHandle, p, SQ_TIME, NULL, &elapsed);
				}

			}

			// check for volume at the receiver level, but only record the change
			if (type && !strcasecmp(type, "RECEIVER_STATUS")) {
				double volume;
				bool muted;

				if (!p->Group && GetMediaVolume(data, 0, &volume, &muted)) {
					u16_t vol = volume * 100 + 0.5;
					if (volume != -1 && !muted && vol != p->Volume) Volume = vol;
				}
			}

			// now apply the volume change if any and if "filtering" has been made
			if (Volume != 0xff && Volume != p->Volume)
			{
				p->VolumeStamp = gettime_ms();
				LOG_INFO("[%p]: Volume local change %d", p, Volume);
				sq_notify(p->SqueezeHandle, p, SQ_VOLUME, NULL, &Volume);
				Volume = 0xff;
			}

			// Cast devices has closed the connection
			if (type && !strcasecmp(type, "CLOSE")) SyncNotifState("CLOSED", p);

			json_decref(data);
		}


		// get track position & CurrentURI
		p->TrackPoll += elapsed;
		if (p->TrackPoll > TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->State != STOPPED) CastGetMediaStatus(p->CastCtx);
		}

		ithread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool RefreshTO(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].InUse && !strcmp(glMRDevices[i].UDN, UDN)) {
			glMRDevices[i].TimeOut = false;
			glMRDevices[i].MissingCount = glMRDevices[i].Config.RemoveCount;
			return true;
		}
	}
	return false;
}


/*----------------------------------------------------------------------------*/
char *GetmDNSAttribute(struct mDNSItem_s *p, char *name)
{
	int j;

	for (j = 0; j < p->attr_count; j++)
		if (!strcasecmp(p->attr[j].name, name))
			return strdup(p->attr[j].value);

	return NULL;
}


/*----------------------------------------------------------------------------*/
static void *UpdateMRThread(void *args)
{
	struct sMR *Device = NULL;
	int i, TimeStamp;
	DiscoveredList DiscDevices;

	LOG_DEBUG("Begin Cast devices update", NULL);
	TimeStamp = gettime_ms();

	if (!glMainRunning) {
		LOG_DEBUG("Aborting ...", NULL);
		return NULL;
	}

	query_mDNS(gl_mDNSId, "_googlecast._tcp.local", &DiscDevices, glScanTimeout);

	for (i = 0; i < DiscDevices.count; i++) {
		char *UDN = NULL, *Name = NULL;
		int j;
		struct mDNSItem_s *p = &DiscDevices.items[i];

		// is the mDNS record usable
		if ((UDN = GetmDNSAttribute(p, "id")) == NULL) continue;

		if (!RefreshTO(UDN)) {
			char *Model;
			bool Group;

			// new device so search a free spot.
			for (j = 0; j < MAX_RENDERERS && glMRDevices[j].InUse; j++);

			// no more room !
			if (j == MAX_RENDERERS) {
				LOG_ERROR("Too many Cast devices", NULL);
				NFREE(UDN);
				break;
			}
			else Device = &glMRDevices[j];

			Name = GetmDNSAttribute(p, "fn");
			if (!Name) Name = strdup(p->hostname);

			// if model is a group, must ignore a few things
			Model = GetmDNSAttribute(p, "md");
			if (Model && !stristr(Model, "Group")) Group = false;
			else Group = true;
			NFREE(Model);

			if (AddCastDevice(Device, Name, UDN, Group, p->addr, p->port) && !glSaveConfigFile) {
				// create a new slimdevice
				Device->SqueezeHandle = sq_reserve_device(Device, Device->on, &sq_callback);
   				if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->FriendlyName);
				if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle, &Device->sq_config)) {
					sq_release_device(Device->SqueezeHandle);
					Device->SqueezeHandle = 0;
					LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
					DelCastDevice(Device);
				}
			}
		}
		else for (j = 0; j < MAX_RENDERERS; j++) {
			if (glMRDevices[j].InUse && !strcmp(glMRDevices[j].UDN, UDN)) {
				UpdateCastDevice(glMRDevices[j].CastCtx, p->addr, p->port);
				break;
			}
		}

		NFREE(UDN);
		NFREE(Name);
	}

	free_discovered_list(&DiscDevices);

	// then walk through the list of devices to remove missing ones
	for (i = 0; i < MAX_RENDERERS; i++) {
		Device = &glMRDevices[i];
		if (!Device->InUse) continue;
		if (Device->TimeOut && Device->MissingCount) Device->MissingCount--;
		if (CastIsConnected(Device->CastCtx) || Device->MissingCount) continue;

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
	int ScanPoll = glScanInterval*1000 + 1;

	while (glMainRunning) {
		int i;
		int elapsed = gettime_ms() - last;

		// reset timeout and re-scan devices
		ScanPoll += elapsed;
		if (glScanInterval && ScanPoll > glScanInterval*1000) {
			pthread_attr_t attr;
			ScanPoll = 0;

			for (i = 0; i < MAX_RENDERERS; i++) {
				glMRDevices[i].TimeOut = true;
				glDiscovery = false;
			}

			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 32*1024);
			pthread_create(&glUpdateMRThread, &attr, &UpdateMRThread, NULL);
			pthread_detach(glUpdateMRThread);
			pthread_attr_destroy(&attr);
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
int Initialize(void)
{
	int rc;
	struct UpnpVirtualDirCallbacks VirtualDirCallbacks;

	if (glScanInterval) {
		if (glScanInterval < SCAN_INTERVAL) glScanInterval = SCAN_INTERVAL;
		if (glScanTimeout < SCAN_TIMEOUT) glScanTimeout = SCAN_TIMEOUT;
		if (glScanTimeout > glScanInterval - SCAN_TIMEOUT) glScanTimeout = glScanInterval - SCAN_TIMEOUT;
	}

	memset(&glMRDevices, 0, sizeof(glMRDevices));

	UpnpSetLogLevel(UPNP_ALL);
	UpnpSetMaxContentLength(60000);

	if (!strstr(glUPnPSocket, "?")) sscanf(glUPnPSocket, "%[^:]:%u", glIPaddress, &glPort);

	if (*glIPaddress) rc = UpnpInit(glIPaddress, glPort);
	else rc = UpnpInit(NULL, glPort);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("UPnP init failed: %d\n", rc);
		UpnpFinish();
		return false;
	}

	if (!*glIPaddress) strcpy(glIPaddress, UpnpGetServerIpAddress());
	if (!glPort) glPort = UpnpGetServerPort();

	LOG_INFO("UPnP init success - %s:%u", glIPaddress, glPort);

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

	return true;
}

/*----------------------------------------------------------------------------*/
int Terminate(void)
{
	LOG_DEBUG("un-register libupnp callbacks ...", NULL);
	LOG_DEBUG("disable webserver ...", NULL);
	UpnpRemoveVirtualDir(glBaseVDIR);
	UpnpEnableWebserver(false);
	LOG_DEBUG("end libupnp ...", NULL);
	UpnpFinish();

	return true;
}


/*----------------------------------------------------------------------------*/
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool group, struct in_addr ip, u16_t port)
{
	u32_t mac_size = 6;
	pthread_attr_t attr;

	// read parameters from default then config file
	memset(Device, 0, sizeof(struct sMR));
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, UDN, &Device->Config, &Device->sq_config);
	if (!Device->Config.Enabled) return false;

	ithread_mutex_init(&Device->Mutex, 0);
	strcpy(Device->UDN, UDN);
	Device->Magic = MAGIC;
	Device->TimeOut = false;
	Device->MissingCount = Device->Config.RemoveCount;
	Device->SqueezeHandle = 0;
	Device->Running = true;
	Device->InUse = true;
	Device->sqState = SQ_STOP;
	Device->State = STOPPED;
	Device->Group = group;
	Device->VolumeStamp = 0;
	if (Device->Config.RoonMode) {
		Device->on = true;
		Device->sq_config.use_cli = false;
	}
	strcpy(Device->FriendlyName, Name);

	LOG_INFO("[%p]: adding renderer (%s)", Device, Name);

	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", mac_size)) {
		if (group || SendARP(*((in_addr_t*) &ip), INADDR_ANY, Device->sq_config.mac, &mac_size)) {
			u32_t hash = hash32(UDN);

			LOG_ERROR("[%p]: creating MAC %x", Device, Device->FriendlyName, hash);
			memcpy(Device->sq_config.mac + 2, &hash, 4);
		}
		memset(Device->sq_config.mac, 0xcc, 2);
	}

	// virtual players duplicate mac address
	MakeMacUnique(Device);

	Device->CastCtx = CreateCastDevice(Device, Device->Group, ip, port, Device->Config.MediaVolume);

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
	Device->Running = false;
	Device->InUse = false;
	pthread_mutex_unlock(&Device->Mutex);
	pthread_join(Device->Thread, NULL);

	StopCastDevice(Device->CastCtx);
	NFREE(Device->CurrentURI);
	NFREE(Device->NextURI);

	pthread_mutex_destroy(&Device->Mutex);
	memset(Device, 0, sizeof(struct sMR));
}

/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	struct in_addr addr;

	InitSSL();
	if (!Initialize()) return false;

	// initialize mDNS query
	addr.s_addr = inet_addr(glIPaddress);
	gl_mDNSId = init_mDNS(false, addr);

	/* start the main thread */
	ithread_create(&glMainThread, NULL, &MainThread, NULL);
	return true;
}

static bool Stop(void)
{
	LOG_DEBUG("flush renderers ...", NULL);
	FlushCastDevices();

	// this forces an ongoing search to end
	close_mDNS(gl_mDNSId);

	LOG_DEBUG("terminate main thread ...", NULL);
	ithread_join(glMainThread, NULL);

	LOG_DEBUG("terminate libupnp ...", NULL);
	Terminate();
	EndSSL();

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
		if (strstr("stxdfpib", opt) && optind < argc - 1) {
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
			strcpy(glDeviceParam.server, optarg);
			break;
		case 'b':
			strcpy(glUPnPSocket, optarg);
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
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto"))	slimproto_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    	stream_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    	decode_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    	output_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "web")) 	  	web_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "cast"))    	cast_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "slimmain"))    slimmain_loglevel = new;
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

	LOG_ERROR("Starting squeeze2cast version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_ERROR("Wrong GLIBC version, use -static build", NULL);
		exit(1);
	}

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

	sq_init();

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

		if (!strcmp(resp, "streamdbg"))	{
			char level[20];
			i = scanf("%s", level);
			stream_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "outputdbg"))	{
			char level[20];
			i = scanf("%s", level);
			output_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "slimprotodbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimproto_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "webdbg"))	{
			char level[20];
			i = scanf("%s", level);
			web_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "maindbg"))	{
			char level[20];
			i = scanf("%s", level);
			main_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "slimmainqdbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimmain_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "utildbg"))	{
			char level[20];
			i = scanf("%s", level);
			util_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "castdbg"))	{
			char level[20];
			i = scanf("%s", level);
			cast_loglevel = debug2level(level);
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




