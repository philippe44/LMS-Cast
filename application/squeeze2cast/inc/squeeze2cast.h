/*
 *  Squeeze2cast - LMS to Cast gateway
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma once

#include <stdint.h>
#include <pthread.h>

#include "platform.h"
#include "squeezeitf.h"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define MAX_RENDERERS	32
#define	AV_TRANSPORT 	"urn:schemas-upnp-org:service:AVTransport:1"
#define	RENDERING_CTRL 	"urn:schemas-upnp-org:service:RenderingControl:1"
#define	CONNECTION_MGR 	"urn:schemas-upnp-org:service:ConnectionManager:1"
#define MAGIC			0xAABBCCDD
#define RESOURCE_LENGTH	250

enum 	eMRstate { STOPPED, BUFFERING, PLAYING, PAUSED };

typedef struct sMRConfig
{
	bool		Enabled;
	bool		StopReceiver;
	int 		VolumeOnPlay;		// change only volume when playing has started or disable volume commands
	bool		VolumeFeedback;
	bool		SendMetaData;
	bool		SendCoverArt;
	bool		AutoPlay;
	double		MediaVolume;
	int			RemoveTimeout;
	int			NextURI;
} tMRConfig;


struct sMR {
	uint32_t Magic;
	bool  Running;
	tMRConfig Config;
	sq_dev_param_t	sq_config;
	bool on;
	char UDN			[RESOURCE_LENGTH];
	char FriendlyName	[RESOURCE_LENGTH];
	enum eMRstate 	State;
	char*			NextURI;				// gapped next URI
	char			NextMime[STR_LEN];    // gapped next mimetype
	metadata_t		NextMetaData;           // gapped next metadata
	bool			ShortTrack;				// current or next track is short
	int16_t			ShortTrackWait;			// stop timeout when short track is last track
	sq_action_t		sqState;
	uint32_t			sqStamp;				// timestamp of slimproto state change to filter fast pause/play
	uint32_t			StartTime;				// for flac reposition issue (offset)
	uint32_t			TrackPoll;
	int32_t			IdleTimer;				// idle timer to disconnect SSL connection
	uint32_t 			Expired;				// timestamp when device was missing (used to keep it for a while)
	int	 			SqueezeHandle;
	void*			CastCtx;
	pthread_mutex_t Mutex;
	pthread_t 		Thread;
	double			Volume;
	uint32_t			VolumeStampRx, VolumeStampTx;	// timestamps to filter volume loopbacks
	bool			Group;
	struct sGroupMember {
		struct sGroupMember	*Next;
		struct in_addr		Host;
		uint16_t				Port;
   } *GroupMaster;
};

extern char 				glBinding[];
extern int32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern sq_dev_param_t		glDeviceParam;
extern struct sMR			glMRDevices[MAX_RENDERERS];
