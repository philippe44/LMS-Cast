/*
 *  Squeeze2upnp - LMS to uPNP gateway
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

#ifndef __CAST_UTIL_H
#define __CAST_UTIL_H

#include "util_common.h"

typedef enum { CAST_PLAY, CAST_PAUSE, CAST_STOP } tCastAction;

struct sq_metadata_s;
struct sMRConfig;

void  	CastInit(log_level level);
void	CastKeepAlive(void *Ctx);
bool 	CastPeerDisc(void *Ctx);
void	CastGetStatus(void *Ctx);
void	CastGetMediaStatus(void *Ctx);

#define CastStop(Ctx) CastBasic(Ctx, CAST_STOP, 2000)
#define CastPlay(Ctx) CastBasic(Ctx, CAST_PLAY, 1000)
#define CastPause(Ctx) CastBasic(Ctx, CAST_PAUSE, 1000)

void	CastBasic(void *Ctx, tCastAction Action, u32_t timeout);
bool	CastLoad(void *Ctx, char *URI, char *ContentType, struct sq_metadata_s *MetaData);
void 	SetVolume(void *p, u8_t Volume);

#endif

