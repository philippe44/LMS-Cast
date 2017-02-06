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

#ifndef __CASTITF_H
#define __CASTITF_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "squeezedefs.h"
#include "squeeze2cast.h"
#include "util_common.h"

#include <pb_encode.h>
#include <pb_decode.h>
#include "jansson.h"
#include "castmessage.pb.h"

void InitSSL(void);
void EndSSL(void);

json_t 	*GetTimedEvent(void *p, u32_t msWait);
void 	*CreateCastDevice(void *owner, bool group, struct in_addr ip, u16_t port, u8_t MediaVolume);
void 	UpdateCastDevice(void *Ctx, struct in_addr ip, u16_t port);
void 	StopCastDevice(void *Ctx);
bool	CastIsConnected(void *Ctx);
bool 	CastIsMediaSession(void *Ctx);

#endif
