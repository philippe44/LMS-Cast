/*
 *  Squeeze2cast - LMS to Cast gateway
 *
 *  (c) Philippe 2016-2017, philippe_44@outlook.com
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
#include "util_common.h"
#include "log_util.h"
#include "util.h"
#include "cast_util.h"
#include "castitf.h"
#include "cast_parse.h"
#include "mdnssd-itf.h"

#define DISCOVERY_TIME 20

/*----------------------------------------------------------------------------*/
/* globals 																	  */
/*----------------------------------------------------------------------------*/
s32_t		glLogLimit = -1;
char		glUPnPSocket[128] = "?";
struct sMR	glMRDevices[MAX_RENDERERS];

log_level	slimproto_loglevel = lINFO;
log_level	slimmain_loglevel = lWARN;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lWARN;
log_level	main_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	cast_loglevel = lINFO;

tMRConfig			glMRConfig = {
							true,	// enabled
							false,  // roon_mode
							false,	// stop_receiver
							"",		// name
							1,      // volume_on_play
							true,	// send_metadata
							true,   // send_coverart
							false,	// autoplay
							0.5,	// media_volume
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
					HTTP_INFINITE, 	 		// stream_length
					 // both are multiple of 3*4(2) for buffer alignement on sample
					(200 * 1024 * (4*3)),	// stream_buffer_size
					(16 * 1024 * (4*3)),    // output_buffer_size
					"pcm,aif,flc,mp3",		// codecs
					"thru",					// encode
					"wav",					// raw_audio_format
					"?",                    // server
					SQ_RATE_96000,          // sample_rate
					L24_PACKED_LPCM,		// L24_format
					FLAC_NORMAL_HEADER,     // flac_header
					"",			// name
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
					false,		// send_icy
					{ 	true,	// use_cli
						"" },   // server
				} ;

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 			*loglevel = &main_loglevel;
#if LINUX || FREEBSD
static bool					glDaemonize = false;
#endif
static bool					glMainRunning = true;
static pthread_t 			glMainThread, glmDNSsearchThread;
static struct mDNShandle_s	*glmDNSsearchHandle = NULL;
static char					*glLogFile;
static bool					glDiscovery = false;
static bool					glAutoSaveConfigFile = false;
static pthread_mutex_t		glMainMutex;
static pthread_cond_t		glMainCond;
static bool					glInteractive = true;
static char					*glPidFile = NULL;
static bool					glGracefullShutdown = true;
static unsigned int 		glPort = 0;
static char 				glIPaddress[128] = "";
static void					*glConfigID = NULL;
static char					glConfigName[_STR_LEN_] = "./config.xml";

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
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|slimmain|stream|decode|output|main|util|cast, level: error|warn|info|debug|sdebug\n"
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
	;

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void	RemoveCastDevice(struct sMR *Device);
static void *MRThread(void *args);
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool Group, struct in_addr ip, u16_t port);

// functions prefixed with _ require device's mutex to be locked
static void _SyncNotifyState(const char *State, struct sMR* Device);

/*----------------------------------------------------------------------------*/
bool sq_callback(sq_dev_handle_t handle, void *caller, sq_action_t action, u8_t *cookie, void *param)
{
	struct sMR *Device = caller;
	char *p = (char*) param;
	bool rc = true;

	pthread_mutex_lock(&Device->Mutex);

	// this is async, so player might have been deleted
	if (!Device->Running) {
		pthread_mutex_unlock(&Device->Mutex);
		LOG_WARN("[%p] device has been removed", Device);
		return false;
	}

	if (action == SQ_ONOFF) {
		Device->on = *((bool*) param);

		LOG_INFO("[%p]: device set %s", caller, Device->on ? "ON" : "OFF");

		if (Device->on) {
			CastPowerOn(Device->CastCtx);
			// candidate for busyraise/drop as it's using cli
			if (Device->Config.AutoPlay) sq_notify(Device->SqueezeHandle, Device, SQ_PLAY, NULL, &Device->on);
		} else {
			// cannot disconnect when LMS is configured for pause when OFF
			if (Device->sqState == SQ_STOP) CastPowerOff(Device->CastCtx);
		}
	}

	if (!Device->on && action != SQ_SETNAME && action != SQ_SETSERVER) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		pthread_mutex_unlock(&Device->Mutex);
		return false;
	}

	LOG_SDEBUG("callback for %s (%d)", Device->FriendlyName, action);

	switch (action) {

		case SQ_SET_TRACK: {
			struct track_param *p = (struct track_param*) param;
			bool Next = (Device->sqState != SQ_STOP);

			NFREE(Device->NextURI);
			sq_free_metadata(&Device->NextMetaData);
			if (!Device->Config.SendCoverArt) NFREE(p->metadata.artwork);

			LOG_INFO("[%p]:\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n\tduration:%d.%03d\n\tsize:%d\n\tcover:%s", Device,
				p->metadata.artist, p->metadata.album, p->metadata.title,
				p->metadata.genre, div(p->metadata.duration, 1000).quot,
				div(p->metadata.duration,1000).rem, p->metadata.file_size,
				p->metadata.artwork ? p->metadata.artwork : "");

			if (Next) {
				// to know what is expected next
				strcpy(Device->NextMime, p->mimetype);
				// this is a structure copy, pointers remains valid
				Device->NextMetaData = p->metadata;
				Device->NextURI = strdup(p->uri);
				LOG_INFO("[%p]: next URI %s", Device, Device->NextURI);
			}
			else {
				rc = CastLoad(Device->CastCtx, p->uri, p->mimetype, (
							  Device->Config.SendMetaData) ? &p->metadata : NULL);
				sq_free_metadata(&p->metadata);
				LOG_INFO("[%p]: current URI %s", Device, p->uri);
			}

			break;
		}
		case SQ_UNPAUSE:
			// got it, don't need to send it more than once ...
			if (Device->sqState == SQ_PLAY) break;

			if (Device->Config.VolumeOnPlay == 1)
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);

			CastPlay(Device->CastCtx);
			Device->sqState = SQ_PLAY;
			break;
		case SQ_PLAY:
#if !defined(REPOS_TIME)
			Device->StartTime = sq_get_time(Device->SqueezeHandle);
			Device->LocalStartTime = gettime_ms();
#endif
			if (Device->Config.VolumeOnPlay == 1)
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);

			CastPlay(Device->CastCtx);
			Device->sqState = SQ_PLAY;
			Device->sqStamp = gettime_ms();
			break;
		case SQ_STOP:
			CastStop(Device->CastCtx);
			NFREE(Device->NextURI);
			sq_free_metadata(&Device->NextMetaData);
			Device->sqState = action;
			break;
		case SQ_PAUSE:
			CastPause(Device->CastCtx);
			Device->sqState = action;
			Device->sqStamp = gettime_ms();
			break;
		case SQ_NEXT:
			break;
		case SQ_SEEK:
			break;
		case SQ_VOLUME: {
			u16_t Volume = *(u16_t*)p;
			u32_t now = gettime_ms();
			int i;

			for (i = 100; Volume < LMSVolumeMap[i] && i; i--);

			Device->Volume = (double) i / 100;
			LOG_INFO("Volume %d", i);

			if ((Device->VolumeStamp + 1000 - now > 0x7fffffff) &&
				(!Device->Config.VolumeOnPlay || (Device->Config.VolumeOnPlay == 1 && Device->sqState == SQ_PLAY)))
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);

			break;
		}
		case SQ_SETNAME:
			strcpy(Device->sq_config.name, param);
			break;
		case SQ_SETSERVER:
			strcpy(Device->sq_config.dynamic.server, inet_ntoa(*(struct in_addr*) param));
			break;
		default:
			break;
	}

	pthread_mutex_unlock(&Device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
static void _SyncNotifyState(const char *State, struct sMR* Device)
{
	sq_event_t Event = SQ_NONE;
	bool Param = false;
	u32_t now = gettime_ms();

	/*
	DEVICE MUTEX IS LOCKED
	*/

	if (!strcasecmp(State, "CLOSED")) {
		Device->State = STOPPED;
		Param = true;
		Event = SQ_STOP;
	}
	if (!strcasecmp(State, "BUFFERING") && Device->State != BUFFERING) {
		Event = SQ_TRANSITION;
		Device->State = BUFFERING;
	}

	if (!strcasecmp(State, "STOPPED") && Device->State != STOPPED) {
		LOG_INFO("[%p]: Cast stop", Device);
		if (Device->NextURI) {

			// fake a "SETURI" and a "PLAY" request
			CastLoad(Device->CastCtx, Device->NextURI, Device->NextMime,
					  (Device->Config.SendMetaData) ? &Device->NextMetaData : NULL);

			CastPlay(Device->CastCtx);

			LOG_INFO("[%p]: no gapless %s", Device, Device->NextURI);

			sq_free_metadata(&Device->NextMetaData);
			NFREE(Device->NextURI);
		} else {
			// Can be a user stop, an error or a normal stop
			Event = SQ_STOP;
		}

		Device->State = STOPPED;
	}

	if (!strcasecmp(State, "PLAYING") && Device->State != PLAYING) {
		LOG_INFO("[%p]: Cast playing", Device);
		switch (Device->sqState) {
		case SQ_PAUSE:
			Param = true;
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

	if (!strcasecmp(State, "PAUSED")) {
		/*
		Cast devices start "paused" so there is a first status received with
		that state, even if the play request has been sent. Make sure that
		this is filtered out and that "pause" state are only taken into account
		when already playing
		*/
		if (Device->State == PLAYING) {
			// detect unsollicited pause, but do not confuse it with a fast pause/play
			if (Device->sqState != SQ_PAUSE && (Device->sqStamp + 2000 - now > 0x7fffffff)) {
				Event = SQ_PAUSE;
				Param = true;
			}
			LOG_INFO("%s: Cast pause", Device->FriendlyName);

			Device->State = PAUSED;
		}
	}

	// candidate for busyraise/drop as it's using cli
	if (Event != SQ_NONE)
		sq_notify(Device->SqueezeHandle, Device, Event, NULL, &Param);
}


/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed;
	unsigned last = gettime_ms();
	struct sMR *p = (struct sMR*) args;
	json_t *data;

	while (p->Running) {
		double Volume = -1;

		// context is valid until this thread ends, no deletion issue
		data = GetTimedEvent(p->CastCtx, 500);
		elapsed = gettime_ms() - last;

		// need to protect against events from CC threads, not from deletion
		pthread_mutex_lock(&p->Mutex);

		LOG_SDEBUG("Cast thread timer %d", elapsed);

		// a message has been received
		if (data) {
			json_t *val = json_object_get(data, "type");
			const char *type = json_string_value(val);

			// a mediaSessionId has been acquired
			if (type && !strcasecmp(type, "MEDIA_STATUS")) {
				const char *url;
				const char *state = GetMediaItem_S(data, 0, "playerState");

				// so far, buffering and playing can be merged
				if (state && (!strcasecmp(state, "PLAYING") || !strcasecmp(state, "BUFFERING"))) {
					_SyncNotifyState("PLAYING", p);
				}

				if (state && !strcasecmp(state, "PAUSED")) {
					_SyncNotifyState("PAUSED", p);
				}

				if (state && !strcasecmp(state, "IDLE")) {
					const char *cause = GetMediaItem_S(data, 0, "idleReason");
					if (cause) _SyncNotifyState("STOPPED", p);
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
					if (gettime_ms() - p->LocalStartTime + 5000 - elapsed > 0x7fffffff) {
						if (elapsed > p->StartTime)	elapsed -= p->StartTime;
						else elapsed = 0;
					}
#endif
					sq_notify(p->SqueezeHandle, p, SQ_TIME, NULL, &elapsed);
				}

				url = GetMediaInfoItem_S(data, 0, "contentId");
				if (url) sq_notify(p->SqueezeHandle, p, SQ_TRACK_INFO, NULL, (void*) url);

			}

			// check for volume at the receiver level, but only record the change
			if (type && !strcasecmp(type, "RECEIVER_STATUS")) {
				double volume;
				bool muted;

				if (!p->Group && GetMediaVolume(data, 0, &volume, &muted)) {
					if (volume != -1 && !muted && volume != p->Volume) Volume = volume;
				}
			}

			// now apply the volume change if any
			if (Volume != -1 && fabs(Volume - p->Volume) >= 0.01) {
				u16_t VolFix = Volume * 100 + 0.5;
				p->VolumeStamp = gettime_ms();
				LOG_INFO("[%p]: Volume local change %u (%0.4lf)", p, VolFix, Volume);
				// candidate for busyraise/drop as it's using cli
				sq_notify(p->SqueezeHandle, p, SQ_VOLUME, NULL, &VolFix);
			}

			// Cast devices has closed the connection
			if (type && !strcasecmp(type, "CLOSE")) _SyncNotifyState("CLOSED", p);

			json_decref(data);
		}


		// get track position & CurrentURI
		p->TrackPoll += elapsed;
		if (p->TrackPoll > TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->State != STOPPED) CastGetMediaStatus(p->CastCtx);
		}

		pthread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static char *GetmDNSAttribute(txt_attr_t *p, int count, char *name)
{
	int j;

	for (j = 0; j < count; j++)
		if (!strcasecmp(p[j].name, name))
			return strdup(p[j].value);

	return NULL;
}


/*----------------------------------------------------------------------------*/
static struct sMR *SearchUDN(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && !strcmp(glMRDevices[i].UDN, UDN))
			return glMRDevices + i;
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool mDNSsearchCallback(mDNSservice_t *slist, void *cookie, bool *stop)
{
	struct sMR *Device;
	mDNSservice_t *s;

	if (*loglevel == lDEBUG) {
		LOG_DEBUG("----------------- round ------------------", NULL);
		for (s = slist; s && glMainRunning; s = s->next) {
			char *host = strdup(inet_ntoa(s->host));
			LOG_DEBUG("host %s, srv %s, name %s ", host, inet_ntoa(s->addr), s->name);
			free(host);
		}
	}

	/*
	cast groups creation is difficult - as storm of mDNS message is sent during
	master's election and many masters will claim the group then will "retract"
	one by one. The logic below works well if no announce is missed, which is
	not the case under high traffic, so in that case, either the actual master
	is missed and it will be discovered at the next 20s search or some retractions
	are missed and if the group is destroyed right after creation, then it will
	hang around	until the retractations timeout (2mins) - still correct as the
	end result is with the right master and group is ultimately removed, but not
	very user-friendy
	*/

	for (s = slist; s && glMainRunning; s = s->next) {
		char *UDN = NULL, *Name = NULL;
		char *Model;
		bool Group;
		char *MimeCaps[] = {"audio/flac", "audio/mpeg", "audio/wav", NULL };
		int j;

		// is the mDNS record usable announce made on behalf
		if ((UDN = GetmDNSAttribute(s->attr, s->attr_count, "id")) == NULL || s->host.s_addr != s->addr.s_addr) continue;

		// is that device already here
		if ((Device = SearchUDN(UDN)) != NULL) {
			// a service is being removed
			if (s->expired) {
				bool Remove = true;
				// groups need to find if the removed service is the master
				if (Device->Group) {
					// there are some other master candidates
					if (Device->GroupMaster->Next) {
						Remove = false;
						// changing the master, so need to update cast params
						if (Device->GroupMaster->Host.s_addr == s->host.s_addr) {
							free(pop_item((list_t**) &Device->GroupMaster));
							UpdateCastDevice(Device->CastCtx, Device->GroupMaster->Host, Device->GroupMaster->Port);
							Remove = false;
						} else {
							struct sGroupMember *Member = Device->GroupMaster;
							while (Member && (Member->Host.s_addr != s->host.s_addr)) Member = Member->Next;
							free(remove_item((list_t*) Member, (list_t**) &Device->GroupMaster));
						}
					}
				}
				if (Remove) {
					LOG_INFO("[%p]: removing renderer (%s) %d", Device, Device->Config.Name);
					sq_delete_device(Device->SqueezeHandle);
					RemoveCastDevice(Device);
				}
			// device update - when playing ChromeCast update their TXT records
			} else {
				// new master in election, update and put it in the queue
				if (Device->Group && Device->GroupMaster->Host.s_addr != s->addr.s_addr) {
					struct sGroupMember *Member = calloc(1, sizeof(struct sGroupMember));
					Member->Host = s->host;
					Member->Port = s->port;
					push_item((list_t*) Member, (list_t**) &Device->GroupMaster);
				}
				UpdateCastDevice(Device->CastCtx, s->addr, s->port);
			}
			NFREE(UDN);
			continue;
		}

		// disconnect of an unknown device
		if (!s->port && !s->addr.s_addr) {
			LOG_ERROR("Unknown device disconnected %s", s->name);
			NFREE(UDN);
			continue;
		}

		// device creation so search a free spot.
		for (j = 0; j < MAX_RENDERERS && glMRDevices[j].Running; j++);

		// no more room !
		if (j == MAX_RENDERERS) {
			LOG_ERROR("Too many Cast devices", NULL);
			NFREE(UDN);
			break;
		}

		Device = glMRDevices + j;

		// if model is a group
		Model = GetmDNSAttribute(s->attr, s->attr_count, "md");
		if (Model && !stristr(Model, "Group")) Group = false;
		else Group = true;
		NFREE(Model);

		Name = GetmDNSAttribute(s->attr, s->attr_count, "fn");
		if (!Name) Name = strdup(s->hostname);

		if (AddCastDevice(Device, Name, UDN, Group, s->addr, s->port) && !glDiscovery) {
			// create a new slimdevice
			Device->SqueezeHandle = sq_reserve_device(Device, Device->on, MimeCaps, &sq_callback);
			if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->FriendlyName);
			if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle, &Device->sq_config)) {
				sq_release_device(Device->SqueezeHandle);
				Device->SqueezeHandle = 0;
				LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
				RemoveCastDevice(Device);
			}
		}

		NFREE(UDN);
		NFREE(Name);
	}
	if (glAutoSaveConfigFile || glDiscovery) {
		LOG_DEBUG("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	// we have not released the slist
	return false;
}


/*----------------------------------------------------------------------------*/
static void *mDNSsearchThread(void *args)
{
	// launch the query,
	query_mDNS(glmDNSsearchHandle, "_googlecast._tcp.local", 120,
			   glDiscovery ? DISCOVERY_TIME : 0, &mDNSsearchCallback, NULL);
	return NULL;
}


/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	while (glMainRunning) {

		pthread_mutex_lock(&glMainMutex);
		pthread_cond_reltimedwait(&glMainCond, &glMainMutex, 30*1000);
		pthread_mutex_unlock(&glMainMutex);

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
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool group, struct in_addr ip, u16_t port)
{
	unsigned long mac_size = 6;

	// read parameters from default then config file
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, UDN, &Device->Config, &Device->sq_config);
	if (!Device->Config.Enabled) return false;

	Device->Magic 			= MAGIC;
	Device->TimeOut			= false;
	Device->SqueezeHandle 	= 0;
	Device->Running 		= true;
	Device->sqState 		= SQ_STOP;
	Device->State 			= STOPPED;
	Device->VolumeStamp    	= Device->TrackPoll = 0;
	Device->NextMime[0]	 	= '\0';
	Device->NextURI 		= NULL;
	Device->Group 			= group;

	if (group) {
		Device->GroupMaster	= calloc(1, sizeof(struct sGroupMember));
		Device->GroupMaster->Host = ip;
		Device->GroupMaster->Port = port;
	} else Device->GroupMaster = NULL;

	strcpy(Device->UDN, UDN);
	strcpy(Device->FriendlyName, Name);

	memset(&Device->NextMetaData, 0, sizeof(metadata_t));

	if (Device->Config.RoonMode) {
		Device->on = true;
		Device->sq_config.dynamic.use_cli = false;
	} else Device->on = false;

	// optional
	Device->sqStamp = Device->Elapsed = 0;
	Device->CastCtx = NULL;
	Device->Volume = 0;
#if !defined(REPOS_TIME)
	Device->StartTime = Device->LocalStartTime = 0;
#endif

	LOG_INFO("[%p]: adding renderer (%s)", Device, Name);

	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", mac_size)) {
		if (group || SendARP(ip.s_addr, INADDR_ANY, Device->sq_config.mac, &mac_size)) {
			u32_t hash = hash32(UDN);

			LOG_ERROR("[%p]: creating MAC %x", Device, Device->FriendlyName, hash);
			memcpy(Device->sq_config.mac + 2, &hash, 4);
		}
		memset(Device->sq_config.mac, 0xcc, 2);
	}

	// virtual players duplicate mac address
	MakeMacUnique(Device);

	Device->CastCtx = CreateCastDevice(Device, Device->Group, Device->Config.StopReceiver, ip, port, Device->Config.MediaVolume);

	pthread_create(&Device->Thread, NULL, &MRThread, Device);

	return true;
}


/*----------------------------------------------------------------------------*/
static void FlushCastDevices(void)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->Running) {
			if (p->sqState == SQ_PLAY || p->sqState == SQ_PAUSE) CastStop(p->CastCtx);
			RemoveCastDevice(p);
		}
	}
}


/*----------------------------------------------------------------------------*/
static void RemoveCastDevice(struct sMR *Device)
{
	pthread_mutex_lock(&Device->Mutex);
	sq_free_metadata(&Device->NextMetaData);
	Device->Running = false;
	pthread_mutex_unlock(&Device->Mutex);
	pthread_join(Device->Thread, NULL);

	clear_list((list_t**) &Device->GroupMaster, free);

	DeleteCastDevice(Device->CastCtx);
	NFREE(Device->NextURI);
}

/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	struct in_addr addr;
	int i, rc;

	memset(&glMRDevices, 0, sizeof(glMRDevices));
	for (i = 0; i < MAX_RENDERERS; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

	UpnpSetLogLevel(UPNP_ALL);

	if (!strstr(glUPnPSocket, "?")) sscanf(glUPnPSocket, "%[^:]:%u", glIPaddress, &glPort);

	if (*glIPaddress) rc = UpnpInit(glIPaddress, glPort);
	else rc = UpnpInit(NULL, glPort);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("UPnP init failed: %d\n", rc);
		UpnpFinish();
		return false;
	}

	UpnpSetMaxContentLength(60000);

	if (!*glIPaddress) strcpy(glIPaddress, UpnpGetServerIpAddress());
	if (!glPort) glPort = UpnpGetServerPort();

	// start squeeze piece
	sq_init(glIPaddress, glPort);

	LOG_INFO("Binding to %s:%d", glIPaddress, glPort);

	// init mutex & cond no matter what
	pthread_mutex_init(&glMainMutex, 0);
	pthread_cond_init(&glMainCond, 0);
	for (i = 0; i < MAX_RENDERERS; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

	InitSSL();

	/* start the mDNS devices discovery thread */
	addr.s_addr = inet_addr(glIPaddress);
	glmDNSsearchHandle = init_mDNS(false, addr);
	pthread_create(&glmDNSsearchThread, NULL, &mDNSsearchThread, NULL);

	/* start the main thread */
	pthread_create(&glMainThread, NULL, &MainThread, NULL);

	return true;
}

/*----------------------------------------------------------------------------*/
static bool Stop(void)
{
	int i;

	LOG_INFO("stopping squeezelite devices ...", NULL);
	sq_stop();

	glMainRunning = false;

	LOG_DEBUG("terminate search thread ...", NULL);
	// this forces an ongoing search to end
	close_mDNS(glmDNSsearchHandle);
	pthread_join(glmDNSsearchThread, NULL);

	LOG_INFO("stopping Cast devices ...", NULL);
	FlushCastDevices();

	LOG_DEBUG("terminate main thread ...", NULL);
	pthread_cond_signal(&glMainCond);
	pthread_join(glMainThread, NULL);
	pthread_mutex_destroy(&glMainMutex);
	pthread_cond_destroy(&glMainCond);
	for (i = 0; i < MAX_RENDERERS; i++) pthread_mutex_destroy(&glMRDevices[i].Mutex);

	LOG_DEBUG("end libupnp ...", NULL);
	UpnpFinish();

	EndSSL();

	if (glConfigID) ixmlDocument_free(glConfigID);

#if WIN
	winsock_close();
#endif

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	int i;

	if (!glGracefullShutdown) {
		for (i = 0; i < MAX_RENDERERS; i++) {
			struct sMR *p = &glMRDevices[i];
			if (p->Running && p->sqState == SQ_PLAY) CastStop(p->CastCtx);
		}
		LOG_INFO("forced exit", NULL);
		exit(EXIT_SUCCESS);
	}

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
		} else if (strstr("tzZIk", opt)) {
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
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			strcpy(glConfigName, optarg);
			glDiscovery = true;
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
					if (!strcmp(l, "all") || !strcmp(l, "slimmain"))	slimmain_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    	stream_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    	decode_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    	output_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "cast"))    	cast_loglevel = new;
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

#if WIN
	winsock_init();
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

	// just do device discovery and exit
	if (glDiscovery) {
		Start();
		sleep(DISCOVERY_TIME + 1);
		Stop();
		return(0);
	}

#if LINUX || FREEBSD
	if (glDaemonize) {
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

	if (!Start()) {
		LOG_ERROR("Cannot start uPnP", NULL);
		strcpy(resp, "exit");
	}

	while (strcmp(resp, "exit")) {

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

		if (!strcmp(resp, "slimmaindbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimmain_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "maindbg"))	{
			char level[20];
			i = scanf("%s", level);
			main_loglevel = debug2level(level);
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

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			bool all = !strcmp(resp, "dumpall");

			for (i = 0; i < MAX_RENDERERS; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [s:%u] [%p::%p]\n",
						p->Config.Name, p->Running, Locked, p->State,
						p, sq_get_ptr(p->SqueezeHandle));
			}
		}
	}

	Stop();
	LOG_INFO("all done", NULL);

	return true;
}




