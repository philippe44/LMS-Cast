/*
 *  Squeeze2cast - LMS to Cast bridge
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "platform.h"

#if WIN
#include <process.h>
#endif

#if USE_SSL
#include <openssl/ssl.h>
#include "cross_ssl.h"
#endif

#include "mdnssd.h"
#include "ixml.h"

#include "cross_util.h"
#include "cross_log.h"
#include "cross_net.h"
#include "cross_ssl.h"
#include "cross_thread.h"

#include "squeezedefs.h"
#include "squeeze2cast.h"

#include "castitf.h"
#include "cast_util.h"
#include "cast_parse.h"
#include "config_cast.h"


#define DISCOVERY_TIME 	20
#define MAX_IDLE_TIME	(30*1000)

#define SHORT_TRACK		(10*1000)

#define MODEL_NAME_STRING	"CastBridge"

enum { NEXT_GAPPED = 0, NEXT_UNDERRUN = 2 };

/*----------------------------------------------------------------------------*/
/* globals 																	  */
/*----------------------------------------------------------------------------*/
int32_t		glLogLimit = -1;
char		glBinding[128] = "?";
struct sMR	glMRDevices[MAX_RENDERERS];

log_level	slimproto_loglevel = lINFO;
log_level	slimmain_loglevel = lWARN;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lWARN;
log_level	main_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	cast_loglevel = lINFO;
bool		log_cmdline = false;

tMRConfig			glMRConfig = {
							true,	// enabled
							false,	// stop_receiver
							1,      // volume_on_play
							true,	// volume_feedback
							true,	// send_metadata
							true,   // send_coverart
							false,	// autoplay
							1.0,	// media_volume
							0,		// remove_timeout
					};

static uint8_t LMSVolumeMap[129] = {
			0, 3, 6, 7, 8, 10, 12, 13, 14, 16, 17, 18, 19, 20,
			21, 22, 24, 25, 26, 27, 28, 28, 29, 30, 31, 32, 33,
			34, 35, 36, 37, 37, 38, 39, 40, 41, 41, 42, 43, 44,
			45, 45, 46, 47, 48, 48, 49, 50, 51, 51, 52, 53, 53,
			54, 55, 55, 56, 57, 57, 58, 59, 60, 60, 61, 61, 62,
			63, 63, 64, 65, 65, 66, 67, 67, 68, 69, 69, 70, 70,
			71, 72, 72, 73, 73, 74, 75, 75, 76, 76, 77, 78, 78,
			79, 79, 80, 80, 81, 82, 82, 83, 83, 84, 84, 85, 86,
			86, 87, 87, 88, 88, 89, 89, 90, 91, 91, 92, 92, 93,
			93, 94, 94, 95, 95, 96, 96, 97, 98, 99, 100
		};

sq_dev_param_t glDeviceParam = {
					HTTP_LENGTH_NONE, 	 	// stream_length
					 // both are multiple of 3*4(2) for buffer alignement on sample
					STREAMBUF_SIZE,			// stream_buffer_size
					OUTPUTBUF_SIZE,			// output_buffer_size
					"aac,ogg,ops,ogf,flc,alc,wav,aif,pcm,mp3",		// codecs
					"thru",					// mode
					HTTP_CACHE_MEMORY,		// memory cache
					false,					// force_aac
					15,						// next_delay
					"wav",					// raw_audio_format
					"?",                    // server
					96000,					// sample_rate
					L24_PACKED_LPCM,		// L24_format
					FLAC_DEFAULT_HEADER,	// flac_header
					"",						// name
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
#ifdef RESAMPLE
					"",						// resample_options
#endif
					false, 					// roon_mode
					"",						// store_prefix
					"",						// coveart resolution
#if !WIN
					{
#endif
						true,				// use_cli
						"",     			// server
						ICY_NONE,           // icy mode
#if !WIN
					 },
#endif

				} ;

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 			*loglevel = &main_loglevel;
#if LINUX || FREEBSD || SUNOS
static bool					glDaemonize = false;
#endif
static bool					glMainRunning = true;
static pthread_t 			glMainThread, glmDNSsearchThread;
static pthread_mutex_t 		glUpdateMutex;
static struct mdnssd_handle_s	*glmDNSsearchHandle = NULL;
static char					*glLogFile;
static bool					glDiscovery = false;
static bool					glAutoSaveConfigFile = false;
static bool					glInteractive = true;
static char					*glPidFile = NULL;
static bool					glGracefullShutdown = true;
static void					*glConfigID = NULL;
static char					glConfigName[STR_LEN] = "./config.xml";
static char					glModelName[STR_LEN] = MODEL_NAME_STRING;
uint32_t					glNetmask;
static char*				glMimeCaps[] = { "audio/flac", "audio/mpeg", "audio/wav", "audio/aac", "audio/mp4",
#ifdef CODECS_STRICT
											 "audio/ogg;codecs=vorbis", "audio/ogg;codecs=opus",
											 "audio/ogg;codecs=flac", NULL };
#else										 
											  "audio/ogg", NULL };
#endif

static char usage[] =

			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <ip[:port]>        connect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -b <ip|iface[:port]>] network address (or interface name) and port to bind to\n"
	       "  -g -3|-2|-1|0	        HTTP content-length (-3:chunked, -2:if known, -1:none, 0:fixed large)\n"
		   "  -M <modelname>        set the squeezelite player model name sent to the server (default: " MODEL_NAME_STRING ")\n"
		   "  -x <config file>      read config from file (default is ./config.xml)\n"
		   "  -i <config file>      discover players, save <config file> and exit\n"
		   "  -I                    auto save config at every network scan\n"
	       "  -d <log>=<level>      set logging level\n"
		   "                        logs: all|slimproto|slimmain|stream|decode|output|main|util|cast\n"
		   "                        level: error|warn|info|debug| sdebug\n"
		   "  -f <logfile>          write debug to logfile\n"
		   "  -p <pid file>         write PID in file\n"
		   "  -C [-]<codec>,<codec> list of potential codecs (aac,ogg,ops,ogf,flc,alc,wav,aif,pcm,mp3). '-' removes codecs from default\n"
		   "  -4                    force aac/adts frames unwrapping from mp4 container\n"
		   "  -c (or -o) thru[|pcm|flc[:<q>]|aac[:<r>]|mp3[:<r>]][,r:[-]<rate>][,s:<8:16:24>][,flow]] transcode mode\n"
		   
#if LINUX || FREEBSD || SUNOS
		   "  -z                    Daemonize\n"
#endif
		   "  -Z                    NOT interactive\n"
		   "  -k                    immediate exit on SIGQUIT and SIGTERM\n"
		   "  -t                    license terms\n"
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
#if SUNOS
	" SUNOS"
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
#if LOOPBACK
		   " LOOPBACK"
#endif
#if FFMPEG
		   " FFMPEG"
#endif
#if RESAMPLE
		   " RESAMPLE"
#endif
#if CODECS
		   " CODECS"
#endif
#if USE_SSL
		   " SSL"
#endif
#if LINKALL
		   " LINKALL"
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
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n";

#define SET_LOGLEVEL(log) 		    \
	if (!strcmp(resp, #log"dbg")) { \
		char level[20];           	\
		(void)! scanf("%s", level);   	\
		log ## _loglevel = debug2level(level); \
	}

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void	RemoveCastDevice(struct sMR *Device);
static void *MRThread(void *args);
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool Group, struct in_addr ip, uint16_t port);
static void DeltaOptions(char* ref, char* src);
static void CheckCodecs(char* codecs, char** MimeCaps);

// functions prefixed with _ require device's mutex to be locked
static void _SyncNotifyState(const char *State, struct sMR* Device);

/*----------------------------------------------------------------------------*/
bool sq_callback(void *caller, sq_action_t action, ...)
{
	struct sMR *Device = caller;
	bool rc = true;

	pthread_mutex_lock(&Device->Mutex);

	// this is async, so player might have been deleted
	if (!Device->Running) {
		pthread_mutex_unlock(&Device->Mutex);
		LOG_WARN("[%p] device has been removed", Device);
		return false;
	}

	va_list args;
	va_start(args, action);

	if (action == SQ_ONOFF) {
		Device->on = va_arg(args, int);

		LOG_DEBUG("[%p]: device set %s", caller, Device->on ? "ON" : "OFF");

		if (Device->on) {
			rc = CastPowerOn(Device->CastCtx);
			if (rc) {
				// candidate for busyraise/drop as it's using cli
				if (Device->Config.AutoPlay) sq_notify(Device->SqueezeHandle, SQ_PLAY, (int) Device->on);
			} else {
				LOG_ERROR("[%p]: device %s unable to power on; connection failed.", caller, Device->FriendlyName);
			}
		} else {
			// cannot disconnect when LMS is configured for pause when OFF
			if (Device->sqState == SQ_STOP) {
				Device->IdleTimer = -1;
				CastPowerOff(Device->CastCtx);
			}
		}
	}

	if (!Device->on && action != SQ_SETNAME && action != SQ_SETSERVER && Device->sqState != SQ_PLAY) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		pthread_mutex_unlock(&Device->Mutex);
		va_end(args);
		return false;
	}

	LOG_SDEBUG("[%p]: callback for %s (%d)", Device, Device->FriendlyName, action);

	switch (action) {

		case SQ_SET_TRACK: {
			struct track_param *p = va_arg(args, struct track_param*);

			NFREE(Device->NextURI);
			metadata_free(&Device->NextMetaData);

			LOG_INFO("[%p]:\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n"
					 "\tduration:%d.%03d\n\tsize:%d\n\tcover:%s\n\toffset:%u", Device,
					p->metadata.artist, p->metadata.album, p->metadata.title,
					p->metadata.genre, div(p->metadata.duration, 1000).quot,
					div(p->metadata.duration,1000).rem, p->metadata.size,
					p->metadata.artwork ? p->metadata.artwork : "", p->offset);

			if (p->offset) {
				if (Device->State == STOPPED) {
					// could not get next URI before track stopped, restart
					Device->ShortTrackWait = 0;
					if (p->metadata.duration && p->metadata.duration < SHORT_TRACK) Device->ShortTrack = true;
					rc = CastLoad(Device->CastCtx, p->uri, p->mimetype, Device->FriendlyName, (Device->Config.SendMetaData) ? &p->metadata : NULL, 0);
					CastPlay(Device->CastCtx, NULL);
					Device->StartTime = 0;
					metadata_free(&p->metadata);
					LOG_WARN("[%p]: next URI (stopped) (s:%u) %s", Device, Device->ShortTrack, p->uri);
				 } else {
					strcpy(Device->NextMime, p->mimetype);
					// this is a structure copy, pointers remains valid
					metadata_clone(&p->metadata, &Device->NextMetaData);
					Device->NextURI = strdup(p->uri);
					LOG_INFO("[%p]: next URI (s:%u) %s", Device, Device->ShortTrack, Device->NextURI);
				 }
			} else {
				if (p->metadata.duration && p->metadata.duration < SHORT_TRACK) Device->ShortTrack = true;
				Device->StartTime = sq_get_time(Device->SqueezeHandle);
				rc = CastLoad(Device->CastCtx, p->uri, p->mimetype, Device->FriendlyName, (Device->Config.SendMetaData) ? &p->metadata : NULL, Device->StartTime);
				LOG_INFO("[%p]: current URI (s:%u) %s", Device, Device->ShortTrack, p->uri);
			}
			if (!rc) {
				LOG_ERROR("[%p]: unable to connect to/load device %s", Device, Device->FriendlyName);
			}
			break;
		}
		case SQ_NEW_METADATA: {
			struct metadata_s *MetaData = va_arg(args, struct metadata_s*);
			//uint64_t offset = gettime_us() / 1000 + sq_get_time(Device->SqueezeHandle);
			LOG_INFO("[%p]: received new metadata (%s)", Device, MetaData->title);
			CastPlay(Device->CastCtx, MetaData);
			break;
		}	
		case SQ_UNPAUSE:
			// got it, don't need to send it more than once ...
			if (Device->sqState == SQ_PLAY) break;

			if (Device->Config.VolumeOnPlay == 1 && Device->Volume != -1)
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);

			CastPlay(Device->CastCtx, NULL);
			Device->sqState = SQ_PLAY;
			break;
		case SQ_PLAY:
			// got it, don't need to send it more than once ...
			if (Device->sqState == SQ_PLAY) break;

			if (Device->Config.VolumeOnPlay == 1 && Device->Volume != -1)
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);

			CastPlay(Device->CastCtx, NULL);
			Device->sqState = SQ_PLAY;
			Device->sqStamp = gettime_ms();
			break;
		case SQ_STOP:
			CastStop(Device->CastCtx);
			NFREE(Device->NextURI);
			metadata_free(&Device->NextMetaData);
			Device->sqState = action;
			Device->ShortTrack = false;
			Device->ShortTrackWait = 0;
			break;
		case SQ_PAUSE:
			CastPause(Device->CastCtx);
			Device->sqState = action;
			Device->sqStamp = gettime_ms();
			break;
		case SQ_NEXT:
			break;
		case SQ_VOLUME: {
			int Volume = va_arg(args, int);
			Device->Volume = (double) LMSVolumeMap[Volume] / 100;
			LOG_INFO("[%p]: LMS volume (0..128) %d => CC %0.4lf", Device, Volume, Device->Volume);

			if (!Device->Config.VolumeOnPlay || (Device->Config.VolumeOnPlay == 1 && Device->sqState == SQ_PLAY)) {
				uint32_t now = gettime_ms();

				if (now > Device->VolumeStampRx + 1000) {
					CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);
					Device->VolumeStampTx = now;
				}
			}

			break;
		}
		case SQ_SETNAME:
			strcpy(Device->sq_config.name, va_arg(args, char*));
			 if (glAutoSaveConfigFile) {
				LOG_INFO("Updating configuration %s", glConfigName);
				pthread_mutex_lock(&glUpdateMutex);
				SaveConfig(glConfigName, glConfigID, false);
				pthread_mutex_unlock(&glUpdateMutex);
			}
			break;
		case SQ_SETSERVER: {
			struct in_addr server;
			server.s_addr = va_arg(args, uint32_t);
			strcpy(Device->sq_config.set_server, inet_ntoa(server));
			break;
		}
		default:
			break;
	}

	pthread_mutex_unlock(&Device->Mutex);
	va_end(args);
	return rc;
}


/*----------------------------------------------------------------------------*/
static void _SyncNotifyState(const char *State, struct sMR* Device)
{
	sq_event_t Event = SQ_NONE;
	bool Param = false;
	
	/*
	DEVICE MUTEX IS LOCKED
	*/

	if (!strcasecmp(State, "CLOSED") && Device->State != STOPPED) {
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
			if (Device->NextMetaData.duration && Device->NextMetaData.duration < SHORT_TRACK) Device->ShortTrack = true;
			else Device->ShortTrack = false;

			if (CastLoad(Device->CastCtx, Device->NextURI, Device->NextMime, Device->FriendlyName, 
					     (Device->Config.SendMetaData) ? &Device->NextMetaData : NULL, 0)) {
				CastPlay(Device->CastCtx, NULL);
				LOG_INFO("[%p]: gapped transition (s:%u) %s", Device, Device->ShortTrack, Device->NextURI);
			} else {
				LOG_ERROR("[%p]: Unable to perform stop; can't reach device: %s", Device, Device->FriendlyName);
			}
			metadata_free(&Device->NextMetaData);
			NFREE(Device->NextURI);
			Device->StartTime = 0;
		} else if (Device->ShortTrack) {
			// might not even have received next LMS's request, wait a bit
			Device->ShortTrackWait = 5000;
			LOG_WARN("[%p]: stop on short track (wait %hd ms for next URI)", Device, Device->ShortTrackWait);
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
			if (Device->sqState != SQ_PAUSE && ((Device->sqStamp + 2000) - gettime_ms() > 2000)) Param = true;
			Event = SQ_PAUSE;
			LOG_INFO("%s: Cast pause", Device->FriendlyName);

			Device->State = PAUSED;
		}
	}

	// candidate for busyraise/drop as it's using cli
	if (Event != SQ_NONE)
		sq_notify(Device->SqueezeHandle, Event, (int) Param);
}


/*----------------------------------------------------------------------------*/
// Per-renderer (device) status-monitoring thread
#define TRACK_POLL  (1000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed;
	unsigned last = gettime_ms();
	struct sMR *p = (struct sMR*) args;
	json_t *data;

	while (p->Running) {
		int wakeTimer;

		if (p->ShortTrack) wakeTimer = TRACK_POLL / 4;
		else if (p->sqState == SQ_STOP && p->IdleTimer == -1) wakeTimer = TRACK_POLL * 10;
		else wakeTimer = TRACK_POLL;

		// context is valid until this thread ends, no deletion issue
		data = GetTimedEvent(p->CastCtx, wakeTimer);
		uint32_t now = gettime_ms();
		elapsed = now - last;

		// need to protect against events from CC threads, not from deletion
		pthread_mutex_lock(&p->Mutex);

		// need to check status there, protected
		if (!p->Running) {
			pthread_mutex_unlock(&p->Mutex);
			break;
		}

		LOG_SDEBUG("[%p]: Cast thread timer %d %d", p, elapsed, wakeTimer);

		// a message has been received
		if (data) {
			json_t *val = json_object_get(data, "type");
			const char *type = json_string_value(val);

			// a mediaSessionId has been acquired
			if (type && !strcasecmp(type, "MEDIA_STATUS")) {
				const char *url;
				const char *state = GetMediaItem_S(data, 0, "playerState");

				// so far, buffering and playing can be merged
				if (state && !strcasecmp(state, "PLAYING")) {
					_SyncNotifyState("PLAYING", p);
				}

				if (state && !strcasecmp(state, "PAUSED")) {
					_SyncNotifyState("PAUSED", p);
				}

				if (state && !strcasecmp(state, "IDLE")) {
					const char *cause = GetMediaItem_S(data, 0, "idleReason");
					if (cause) {
						if (p->State != STOPPED) p->IdleTimer = 0;
						_SyncNotifyState("STOPPED", p);
					}
				}

				/*
				Discard any time info unless we are confirmed playing. For FLAC encoding Cast devices
				use frame number to estimate the elapsed time, so when LMS sends a file from a given
				byte offset, we will see a time offset that we need to remove as LMS expects a report
				from 0
				*/
				if (p->State == PLAYING && p->sqState == SQ_PLAY && CastIsMediaSession(p->CastCtx)) {
					uint32_t elapsed = 1000L * GetMediaItem_F(data, 0, "currentTime");
					int32_t gap = elapsed - sq_self_time(p->SqueezeHandle);

					LOG_DEBUG("elapsed %u, self %u, gap %u", elapsed, sq_self_time(p->SqueezeHandle), abs(gap));
					// no time correction in case of flow ... huh
					if (!strstr(p->sq_config.mode, "flow") && p->StartTime > 500 && abs(gap) > 2000) {
						if (elapsed > p->StartTime)	elapsed -= p->StartTime;
						else elapsed = 0;
					}
					sq_notify(p->SqueezeHandle, SQ_TIME, elapsed);
				}

				// LOAD sets the url but we should wait till we are PLAYING
				if (p->State == PLAYING) {
					url = GetMediaInfoItem_S(data, 0, "contentId");
					if (url) sq_notify(p->SqueezeHandle, SQ_TRACK_INFO, url);
				}

			}

			// check for volume at the receiver level, but only record the change
			if (type && p->Config.VolumeFeedback && !strcasecmp(type, "RECEIVER_STATUS")) {
				double Volume = -1;
				bool Muted;

				if (GetMediaVolume(data, 0, &Volume, &Muted) && Volume != -1 && now > p->VolumeStampTx + 1000) {
					if (!Muted && Volume != p->Volume && fabs(Volume - p->Volume) >= 0.01 ) {
						int VolFix = Volume * 100 + 0.5;
						p->VolumeStampRx = now;
						LOG_INFO("[%p]: Volume local change CC %0.4lf => LMS (0..100) %u ", p, Volume, VolFix);
						sq_notify(p->SqueezeHandle, SQ_VOLUME, VolFix);
					} else if (Muted) {
						// un-mute is detected by volume change, no need to detect it (and it fails anyway)
						p->VolumeStampRx = now;
						LOG_INFO("[%p]: setting mute", p);
						sq_notify(p->SqueezeHandle, SQ_MUTE, 1);
					}
				}
			}

			// Cast devices has closed the connection
			if (type && !strcasecmp(type, "CLOSE")) _SyncNotifyState("CLOSED", p);

			json_decref(data);
		}

		// was just waiting for a short track to end
		if (p->ShortTrackWait > 0 && ((p->ShortTrackWait -= elapsed) < 0)) {
			LOG_WARN("[%p]: stopping on short track timeout", p);
			p->ShortTrack = false;
			sq_notify(p->SqueezeHandle, SQ_STOP, (int) p->ShortTrack);
		}

		// get track position & CurrentURI
		p->TrackPoll += elapsed;
		if (p->TrackPoll >= TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->State != STOPPED) {
				CastGetMediaStatus(p->CastCtx);
				wakeTimer = TRACK_POLL;
            }
		}

		if (p->State == STOPPED && p->IdleTimer != -1) {
			p->IdleTimer += elapsed;
			if (p->IdleTimer > MAX_IDLE_TIME) {
				p->IdleTimer = -1;
				CastRelease(p->CastCtx);
				LOG_INFO("[%p]: Idle timeout, releasing cast device", p);
			}
		}

		pthread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static char *GetmDNSAttribute(mdnssd_txt_attr_t *p, int count, char *name) {
	for (int i = 0; i < count; i++)
		if (!strcasecmp(p[i].name, name))
			return strdup(p[i].value);

	return NULL;
}

/*----------------------------------------------------------------------------*/
static struct sMR *SearchUDN(char *UDN) {
	for (int i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && !strcmp(glMRDevices[i].UDN, UDN))
			return glMRDevices + i;
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
static void UpdateDevices() {
	uint32_t now = gettime_ms();

	pthread_mutex_lock(&glUpdateMutex);

	// walk through the list for device that expire on timeout
	for (int i = 0; i < MAX_RENDERERS; i++) {
		struct sMR* Device = Device = glMRDevices + i;
		if (Device->Running && Device->Config.RemoveTimeout >= 0 // active entry, but not a device which never expires
			&& !CastIsConnected(Device->CastCtx)
			&& Device->Expired && now > Device->Expired + Device->Config.RemoveTimeout * 1000) {

			LOG_INFO("[%p]: removing renderer (%s) on timeout", Device, Device->FriendlyName);
			sq_delete_device(Device->SqueezeHandle);
			RemoveCastDevice(Device);
		}
	}

	pthread_mutex_unlock(&glUpdateMutex);
}

/*----------------------------------------------------------------------------*/
static bool isMember(struct in_addr host) {
	for (int i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && CastGetAddr(glMRDevices[i].CastCtx).s_addr == host.s_addr) return true;
	}
	return false;
}

/*----------------------------------------------------------------------------*/
// Called periodically by mdnssd_query. If slist != null, a matching service 
// has broadcast a new or updated resource record, or a keep-alive for an 
// existing service did not arrive in time.
static bool mDNSsearchCallback(mdnssd_service_t *slist, void *cookie, bool *stop)
{
	struct sMR *Device;
	mdnssd_service_t *s;
	uint32_t now = gettime_ms();
	bool Updated = false;

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
		char *UDN = NULL, *Name = NULL, *Model;
		bool Group;

		// is the mDNS record usable announce made by other CC on behalf
		if ((UDN = GetmDNSAttribute(s->attr, s->attr_count, "id")) == NULL || (s->host.s_addr != s->addr.s_addr && isMember(s->host))) continue;

		// is that service already in our device list?
		if ((Device = SearchUDN(UDN)) != NULL) {
			LOG_DEBUG("[%p]: mDNS service update for existing device (%s)", Device, Device->FriendlyName);
			Device->Expired = 0;
			// a device to be removed
			if (s->expired) {
				bool Remove = true;
				// groups need to find if the removed service is the master
				if (Device->Group) {
					// there are some other master candidates
					if (Device->GroupMaster->Next) {
						Remove = false;
						// changing the master, so need to update cast params
						if (Device->GroupMaster->Host.s_addr == s->host.s_addr) {
							free(list_pop((cross_list_t**) &Device->GroupMaster));
							UpdateCastDevice(Device->CastCtx, Device->GroupMaster->Host, Device->GroupMaster->Port);
						} else {
							struct sGroupMember *Member = Device->GroupMaster;
							while (Member && (Member->Host.s_addr != s->host.s_addr)) Member = Member->Next;
							if (Member) free(list_remove((cross_list_t*) Member, (cross_list_t**) &Device->GroupMaster));
						}
					}
				}
				if (Remove) {
					if (!Device->Config.RemoveTimeout && !CastIsConnected(Device->CastCtx)) {
						// if currently connected, removal is delayed until the connection terminates (unless subsequently re-detected by mdns)
						LOG_INFO("[%p]: removing renderer (%s)", Device, Device->FriendlyName);
						sq_delete_device(Device->SqueezeHandle);
						RemoveCastDevice(Device);
					} else {
						LOG_INFO("[%p]: keeping missing renderer (%s) for now", Device, Device->FriendlyName);
						Device->Expired = now | 0x01;
					}
				}
			// device update - when playing ChromeCast update their TXT records
			} else {
				char *Name = GetmDNSAttribute(s->attr, s->attr_count, "fn");

				// new master in election, update and put it in the queue
				if (Device->Group && Device->GroupMaster->Host.s_addr != s->addr.s_addr) {
					struct sGroupMember *Member = calloc(1, sizeof(struct sGroupMember));
					Member->Host = s->host;
					Member->Port = s->port;
					list_push((cross_list_t*) Member, (cross_list_t**) &Device->GroupMaster);
				}

				if (UpdateCastDevice(Device->CastCtx, s->addr, s->port)) {
					LOG_INFO("[%p]: refreshing renderer (%s)", Device, Device->FriendlyName);
				}

				// update Device name if needed
				if (Name && strcmp(Name, Device->FriendlyName)) {
					// only update if LMS has not set its own name
					if (!strcmp(Device->sq_config.name, Device->FriendlyName)) {
						// by notifying LMS, we'll get an update later
						sq_notify(Device->SqueezeHandle, SQ_SETNAME, Name);
					}

					LOG_INFO("[%p]: Name update %s => %s (LMS:%s)", Device, Device->FriendlyName, Name, Device->sq_config.name);
					strcpy(Device->FriendlyName, Name);
					Updated = true;
				}
				NFREE(Name);

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

		// new device so search a free spot - as this function is not called
		// recursively, no need to lock the device's mutex
		for (Device = glMRDevices; Device->Running && Device < glMRDevices + MAX_RENDERERS; Device++);

		// no more room !
		if (Device == glMRDevices + MAX_RENDERERS) {
			LOG_ERROR("Too many devices (max:%u)", MAX_RENDERERS);
			NFREE(UDN);
			break;
		}

		// if model is a group
		Model = GetmDNSAttribute(s->attr, s->attr_count, "md");
		if (Model && !strcasestr(Model, "Group")) Group = false;
		else Group = true;
		NFREE(Model);

		Name = GetmDNSAttribute(s->attr, s->attr_count, "fn");
		if (!Name) Name = strdup(s->hostname);

		if (AddCastDevice(Device, Name, UDN, Group, s->addr, s->port) && !glDiscovery) {
			// create a new slimdevice
			Device->SqueezeHandle = sq_reserve_device(Device, Device->on, glMimeCaps, &sq_callback);
			if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->FriendlyName);
			if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle, &Device->sq_config)) {
				sq_release_device(Device->SqueezeHandle);
				Device->SqueezeHandle = 0;
				LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
				RemoveCastDevice(Device);
			} else {
				Updated = true;
			}
		}

		NFREE(UDN);
		NFREE(Name);
	}

	UpdateDevices();

	if ((Updated && glAutoSaveConfigFile) || glDiscovery) {
		if (!glDiscovery) LOG_INFO("Updating configuration %s", glConfigName);
		pthread_mutex_lock(&glUpdateMutex);
		SaveConfig(glConfigName, glConfigID, false);
		pthread_mutex_unlock(&glUpdateMutex);
	}

	// we have intentionally not released the slist
	return false;
}

/*----------------------------------------------------------------------------*/
static void *mDNSsearchThread(void *args)
{
	// launch the query,
	if (!mdnssd_query(glmDNSsearchHandle, "_googlecast._tcp.local", false,
			glDiscovery ? DISCOVERY_TIME : 0, &mDNSsearchCallback, NULL)) 
		LOG_WARN("mDNS search query has exited with an error. Should normally only exit when closing the bridge.");
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	while (glMainRunning) {

		crossthreads_sleep(30*1000);
		if (!glMainRunning) break;

		if (glLogFile && glLogLimit != - 1) {
			uint32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				uint32_t Sum, BufSize = 16384;
				uint8_t *buf = malloc(BufSize);

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

		UpdateDevices();
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool group, struct in_addr ip, uint16_t port) {
	// read parameters from default then config file
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, UDN, &Device->Config, &Device->sq_config);
	if (!Device->Config.Enabled) return false;

	DeltaOptions(glDeviceParam.codecs, Device->sq_config.codecs);
	DeltaOptions(glDeviceParam.raw_audio_format, Device->sq_config.raw_audio_format);
	if (strcasestr(Device->sq_config.mode, "thru")) CheckCodecs(Device->sq_config.codecs, glMimeCaps);

	Device->Magic 			= MAGIC;
	Device->IdleTimer		= -1;
	Device->SqueezeHandle 	= 0;
	Device->Running 		= true;
	Device->sqState 		= SQ_STOP;
	Device->State 			= STOPPED;
	Device->TrackPoll 		= 0;
	Device->VolumeStampRx = Device->VolumeStampTx = gettime_ms() - 2000;
	Device->NextMime[0]	 	= '\0';
	Device->NextURI 		= NULL;
	Device->Group 			= group;
	Device->Expired			= 0;
	Device->ShortTrack		= false;
	Device->ShortTrackWait	= 0;

	if (group) {
		Device->GroupMaster	= calloc(1, sizeof(struct sGroupMember));
		Device->GroupMaster->Host = ip;
		Device->GroupMaster->Port = port;
	} else Device->GroupMaster = NULL;

	strcpy(Device->UDN, UDN);
	strcpy(Device->FriendlyName, Name);

	memset(&Device->NextMetaData, 0, sizeof(metadata_t));

	if (Device->sq_config.roon_mode) {
		Device->on = true;
		Device->sq_config.use_cli = false;
	} else Device->on = false;

	// optional
	Device->sqStamp = 0;
	Device->CastCtx = NULL;
	Device->Volume = -1;
	Device->StartTime = 0;

	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", 6)) {
		uint32_t mac_size = 6;
		if (group || SendARP(ip.s_addr, INADDR_ANY, Device->sq_config.mac, &mac_size)) {
			*(uint32_t*)(Device->sq_config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: creating MAC", Device);
		}
		memset(Device->sq_config.mac, 0xcc, 2);
	}

	// virtual players duplicate mac address
	for (int i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && Device != glMRDevices + i && !memcmp(glMRDevices[i].sq_config.mac, Device->sq_config.mac, 6)) {
			memset(Device->sq_config.mac, 0xcc, 2);
			*(uint32_t*)(Device->sq_config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: duplicated mac ... updating", Device);
		}
	}

	LOG_INFO("[%p]: adding renderer (%s) with mac %hX-%X", Device, Device->FriendlyName, *(uint16_t*)Device->sq_config.mac, *(uint32_t*)(Device->sq_config.mac + 2));
	Device->CastCtx = CreateCastDevice(Device, Device->Group, Device->Config.StopReceiver, ip, port, Device->Config.MediaVolume);
	pthread_create(&Device->Thread, NULL, &MRThread, Device);

	return true;
}

/*----------------------------------------------------------------------------*/
static void FlushCastDevices(void) {
	for (int i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->Running) {
			if (p->sqState == SQ_PLAY || p->sqState == SQ_PAUSE) CastStop(p->CastCtx);
			RemoveCastDevice(p);
		}
	}
}

/*----------------------------------------------------------------------------*/
static void RemoveCastDevice(struct sMR *Device) {
	pthread_mutex_lock(&Device->Mutex);
	Device->Running = false;
	pthread_mutex_unlock(&Device->Mutex);

	DeleteCastDevice(Device->CastCtx);

	pthread_join(Device->Thread, NULL);

	list_clear((cross_list_t**) &Device->GroupMaster, free);
	metadata_free(&Device->NextMetaData);
	NFREE(Device->NextURI);
}

/*----------------------------------------------------------------------------*/
static bool Start(void) {
	struct in_addr Host;
	unsigned short Port = 0;
	char addr[128] = "";
	
#if USE_SSL
	if (!cross_ssl_load()) {
		LOG_ERROR("Cannot load SSL libraries", NULL);
		return false;
	}
#endif

	// sscanf does not capture empty strings
	if (!strchr(glBinding, '?') && !sscanf(glBinding, "%[^:]:%hu", addr, &Port)) sscanf(glBinding, ":%hu", &Port);

	char* iface = NULL;
	Host = get_interface(addr, &iface, &glNetmask);
	LOG_INFO("Binding to %s [%s] with mask 0x%08x (http port %hu)", inet_ntoa(Host), iface, ntohl(glNetmask), Port);
	NFREE(iface);

	// can't find a suitable interface
	if (Host.s_addr == INADDR_NONE) return false;

	memset(&glMRDevices, 0, sizeof(glMRDevices));
	for (int i = 0; i < MAX_RENDERERS; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

	// start squeezebox part
	sq_init(Host, Port, glModelName);

	// init mutex & cond no matter what
	for (int i = 0; i < MAX_RENDERERS; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

	/* start the mDNS devices discovery thread */
	if ((glmDNSsearchHandle = mdnssd_init(false, Host, true)) == NULL) {
		LOG_ERROR("Cannot start mDNS searcher", NULL);
		return false;
	}

	pthread_mutex_init(&glUpdateMutex, 0);
	pthread_create(&glmDNSsearchThread, NULL, &mDNSsearchThread, NULL);

	/* start the main thread */
	pthread_create(&glMainThread, NULL, &MainThread, NULL);

	return true;
}

/*----------------------------------------------------------------------------*/
static bool Stop(void) {
	LOG_INFO("stopping squeezelite devices ...", NULL);
	sq_stop();

	glMainRunning = false;

	LOG_DEBUG("terminate search thread ...", NULL);
	// this forces an ongoing search to end
	mdnssd_close(glmDNSsearchHandle);
	pthread_join(glmDNSsearchThread, NULL);
	pthread_mutex_destroy(&glUpdateMutex);

	LOG_INFO("stopping Cast devices ...", NULL);
	FlushCastDevices();

	LOG_DEBUG("terminate main thread ...", NULL);
	crossthreads_wake();
	pthread_join(glMainThread, NULL);
	for (int i = 0; i < MAX_RENDERERS; i++) pthread_mutex_destroy(&glMRDevices[i].Mutex);
	if (glConfigID) ixmlDocument_free(glConfigID);

	netsock_close();
#if USE_SSL
	cross_ssl_free();
#endif

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	static bool quit = false;

	// give it some time to finish ...
	if (quit) {
		LOG_INFO("Please wait for clean exit!", NULL);
		return;
	}

	quit = true;

	if (!glGracefullShutdown) {
		for (int i = 0; i < MAX_RENDERERS; i++) {
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
static void CheckCodecs(char* codecs, char** MimeCaps) {
	char _item[4];

	while (codecs && sscanf(codecs, "%3[^,]", _item) > 0) {
		char *item = _item;

		// do a bit of name mangling
		if (!strcasecmp(item, "ops") || !strcasecmp(item, "ogf")) item = "ogg";
		else if (!strcasecmp(item, "aif") || !strcasecmp(item, "pcm")) item = "wav";
		else if (!strcasecmp(item, "flc")) item = "flac";

		// search for codec in mimecaps
		bool found = false;
		for (char** p = MimeCaps; *p && !found; p++) if (strcasestr(*p, item)) found = true;

		// remove codecs if not found and continue
		if (!found) memmove(codecs, codecs + 4, strlen(codecs + 4) + 1);

		codecs = strchr(codecs, ',');
		if (codecs) codecs++;
	}
}

/*---------------------------------------------------------------------------*/
static void DeltaOptions(char* ref, char* src) {
	char item[4], * p;

	if (!strchr(src, '+') && !strchr(src, '-')) return;
	ref = strdup(ref);

	for (p = src; *p && (p = strchr(p, '+')) != NULL; p++) {
		if (sscanf(p, "+%3[^,]", item) <= 0) break;
		if (!strstr(ref, item)) {
			strcat(ref, ",");
			strcat(ref, item);
		}
	}

	for (p = src; *p && (p = strchr(p, '-')) != NULL; p++) {
		char* pos;
		if (sscanf(p, "-%3[^,]", item) <= 0) break;
		if ((pos = strstr(ref, item)) != NULL) {
			int n = strlen(item);
			if (pos[n]) n++;
			memmove(pos, pos + n, strlen(pos + n) + 1);
		}
	}

	strcpy(src, ref);
	free(ref);
}

/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1 ;
	char cmdline[256] = "";

	for (int i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < sizeof(cmdline)); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("sxdfpibcMogLC", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIk4", opt)) {
			optarg = NULL;
			optind += 1;
		} else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case '4':
			glDeviceParam.force_aac = true;
			break;
		case 'C':
			strcpy(glDeviceParam.codecs, optarg);
			break;
		case 'g':
			glDeviceParam.stream_length = atoll(optarg);
			break;
		case 'L':
			strcpy(glDeviceParam.store_prefix, optarg);
			break;
		case 'c':
		case 'o':
			strcpy(glDeviceParam.mode, optarg);
			break;
		case 's':
			strcpy(glDeviceParam.server, optarg);
			break;
		case 'M':
			strcpy(glModelName, optarg);
			break;
		case 'b':
			strcpy(glBinding, optarg);
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

#if LINUX || FREEBSD || SUNOS
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
					log_cmdline = true;
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
				} else {
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
int main(int argc, char *argv[]) {
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	// first try to find a config file on the command line
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) {
		netsock_close();
		exit(1);
	}

	// start network now
	netsock_init();

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_WARN("Starting squeeze2cast version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_WARN("weird GLIBC, try -static build in case of failure");
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

#if LINUX || FREEBSD || SUNOS
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
		} else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	if (!Start()) {
		LOG_ERROR("Cannot start, exiting", NULL);
		exit(0);
	}

	char resp[20] = "";
	
	while (strcmp(resp, "exit")) {

#if LINUX || FREEBSD || SUNOS
		if (!glDaemonize && glInteractive)
			(void)! scanf("%s", resp);
		else pause();
#else
		if (glInteractive)
			(void)! scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif

		SET_LOGLEVEL(stream);
		SET_LOGLEVEL(output);
		SET_LOGLEVEL(decode);
		SET_LOGLEVEL(slimproto);
		SET_LOGLEVEL(slimmain);
		SET_LOGLEVEL(main);
		SET_LOGLEVEL(util);
		SET_LOGLEVEL(cast);

		if (!strcmp(resp, "save"))	{
			char name[128];
			(void)! scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			bool all = !strcmp(resp, "dumpall");

			for (int i = 0; i < MAX_RENDERERS; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [s:%u] [%p::%p]\n",
						p->FriendlyName, p->Running, Locked, p->State,
						p, sq_get_ptr(p->SqueezeHandle));
			}
		}
	}

	Stop();
	LOG_INFO("all done", NULL);

	return true;
}




