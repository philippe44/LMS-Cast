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

#ifndef __SQUEEZE2CAST_H
#define __SQUEEZE2CAST_H

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

#include "upnp.h"
#include "ithread.h"
#include "squeezedefs.h"
#include "squeezeitf.h"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define REPOS_TIME 1

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
	bool		RoonMode;
	bool		StopReceiver;
	char		Name[_STR_LEN_];
	int 		VolumeOnPlay;		// change only volume when playing has started or disable volume commands
	bool		SendMetaData;
	bool		SendCoverArt;
	bool		AutoPlay;
	double		MediaVolume;
} tMRConfig;


struct sMR {
	u32_t Magic;
	bool  Running;
	tMRConfig Config;
	sq_dev_param_t	sq_config;
	bool on;
	char UDN			[RESOURCE_LENGTH];
	char FriendlyName	[RESOURCE_LENGTH];
	enum eMRstate 	State;
	char*			NextURI;
	char			NextMime[_STR_LEN_];
	metadata_t		NextMetaData;
	sq_action_t		sqState;
	u32_t			sqStamp;
	u32_t			Elapsed;
#if !defined(REPOS_TIME)
	u32_t			StartTime, LocalStartTime;
#endif
	unsigned		TrackPoll;
	bool			TimeOut;
	int	 			SqueezeHandle;
	void*			CastCtx;
	pthread_mutex_t Mutex;
	pthread_t 		Thread;
	double			Volume;
	u32_t			VolumeStamp;
	bool			Group;
	struct sGroupMember {
		struct sGroupMember	*Next;
		struct in_addr		Host;
		u16_t				Port;
   } *GroupMaster;
};

extern char 				glUPnPSocket[];
extern s32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern sq_dev_param_t		glDeviceParam;
extern struct sMR			glMRDevices[MAX_RENDERERS];

#endif
