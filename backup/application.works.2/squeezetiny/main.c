/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe 2015-2017, philippe_44@outlook.com
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

 /*
 TODO
 - Manage order of destruction fo the machine and potential race condition with
 objects like mutexes that are used in common. typically all other processes should
 wait for proto to be finshed before destroying their contextes
 */

#include "squeezelite.h"

#include <math.h>
#include <signal.h>
#include <ctype.h>

#define IMAGEPROXY "/imageproxy/"

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O
#define UNLOCK_O
#define LOCK_P   mutex_lock(ctx->mutex)
#define UNLOCK_P mutex_unlock(ctx->mutex)

struct thread_ctx_s thread_ctx[MAX_PLAYER];

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static void sq_wipe_device(struct thread_ctx_s *ctx);

extern log_level	 main_loglevel;
static log_level	*loglevel = &main_loglevel;

static pthread_t	 http_thread;

/*--------------------------------------------------------------------------*/
void sq_wipe_device(struct thread_ctx_s *ctx) {
	int i;

	ctx->callback = NULL;
	ctx->in_use = false;

	slimproto_close(ctx);
	output_close(ctx);
#if RESAMPLE
	process_end(ctx);
#endif
	decode_close(ctx);
	stream_close(ctx);

	for (i = 0; ctx->mimecaps[i]; i++) free(ctx->mimecaps[i]);
}

/*--------------------------------------------------------------------------*/
void sq_delete_device(sq_dev_handle_t handle) {
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	if (!handle) return;

	ctx = &thread_ctx[handle - 1];
	sq_wipe_device(ctx);
}

/*---------------------------------------------------------------------------*/
static char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*---------------------------------------------------------------------------*/
static char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_encode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) * 3 + 1), *pbuf = buf;
  while (*pstr) {
	if ( isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' ||
						  *pstr == '~' || *pstr == ' ' || *pstr == ')' ||
						  *pstr == '(' )
	  *pbuf++ = *pstr;
	else if (*pstr == '%') {
	  *pbuf++ = '%',*pbuf++ = '2', *pbuf++ = '5';
	}
	else
	  *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
	pstr++;
  }
  *pbuf = '\0';
  return buf;
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_decode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) + 1), *pbuf = buf;
  while (*pstr) {
	if (*pstr == '%') {
	  if (pstr[1] && pstr[2]) {
		*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
		pstr += 2;
	  }
	} else {
	  *pbuf++ = *pstr;
	}
	pstr++;
  }
  *pbuf = '\0';
  return buf;
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_find_tag(char *str, char *tag)
{
	char *p, *res = NULL;
	char *buf = malloc(max(strlen(str), strlen(tag)) + 4);

	strcpy(buf, tag);
	strcat(buf, "%3a");
	if ((p = stristr(str, buf)) != NULL) {
		int i = 0;
		p += strlen(buf);
		while (*(p+i) != ' ' && *(p+i) != '\n' && *(p+i)) i++;
		if (i) {
			strncpy(buf, p, i);
			buf[i] = '\0';
			res = url_decode(buf);
		}
	}
	free(buf);
	return res;
}

/*---------------------------------------------------------------------------*/
bool cli_open_socket(struct thread_ctx_s *ctx) {
	struct sockaddr_in addr;

	if (!ctx->config.dynamic.use_cli) return false;
	if (ctx->cli_sock > 0) return true;

	ctx->cli_sock = socket(AF_INET, SOCK_STREAM, 0);
	set_nonblock(ctx->cli_sock);
	set_nosigpipe(ctx->cli_sock);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ctx->slimproto_ip;
	addr.sin_port = htons(9090);

	if (connect_timeout(ctx->cli_sock, (struct sockaddr *) &addr, sizeof(addr), 50))  {
		LOG_ERROR("[%p] unable to connect to server with cli", ctx);
		ctx->cli_sock = -1;
		return false;
	}

	LOG_INFO("[%p]: opened CLI socket %d", ctx, ctx->cli_sock);
	return true;
}

#define CLI_SEND_SLEEP (10000)
#define CLI_SEND_TO (1*500000)
/*---------------------------------------------------------------------------*/
char *cli_send_cmd(char *cmd, bool req, bool decode, struct thread_ctx_s *ctx)
{
#define CLI_LEN 2048
	char packet[CLI_LEN];
	int wait;
	size_t len;
	char *rsp = NULL;

	mutex_lock(ctx->cli_mutex);
	if (!cli_open_socket(ctx)) {
		mutex_unlock(ctx->cli_mutex);
		return NULL;
	}
	ctx->cli_timestamp = gettime_ms();

	wait = CLI_SEND_TO / CLI_SEND_SLEEP;
	cmd = cli_encode(cmd);
	if (req) len = sprintf(packet, "%s ?\n", cmd);
	else len = sprintf(packet, "%s\n", cmd);

	LOG_SDEBUG("[%p]: cmd %s", ctx, packet);
	send_packet((u8_t*) packet, len, ctx->cli_sock);
	// first receive the tag and then point to the last '\n'
	len = 0;
	while (wait)	{
		int k;
		fd_set rfds;
		struct timeval timeout = {0, CLI_SEND_SLEEP};

		FD_ZERO(&rfds);
		FD_SET(ctx->cli_sock, &rfds);

		k = select(ctx->cli_sock + 1, &rfds, NULL, NULL, &timeout);

		if (!k) {
			wait--;
			continue;
		}

		if (k < 0) break;

		k = recv(ctx->cli_sock, packet + len, CLI_LEN-1 - len, 0);
		if (k <= 0) break;

		len += k;
		packet[len] = '\0';
		if (strchr(packet, '\n') && stristr(packet, cmd)) {
			rsp = packet;
			break;
		}
	}

	if (!wait) {
		LOG_WARN("[%p]: Timeout waiting for CLI reponse (%s)", ctx, cmd);
	}

	LOG_SDEBUG("[%p]: rsp %s", ctx, rsp);

	if (rsp && ((rsp = stristr(rsp, cmd)) != NULL)) {
		rsp += strlen(cmd);
		while (*rsp && *rsp == ' ') rsp++;

		if (decode) rsp = cli_decode(rsp);
		else rsp = strdup(rsp);
		*(strrchr(rsp, '\n')) = '\0';
	}

	NFREE(cmd);
	mutex_unlock(ctx->cli_mutex);
	return rsp;
}

/*--------------------------------------------------------------------------*/
u32_t sq_get_time(sq_dev_handle_t handle)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;
	u32_t time = 0;

	if (!ctx->config.dynamic.use_cli) return 0;

	if (!handle || !ctx->in_use) {
		LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		return 0;
	}

	sprintf(cmd, "%s time", ctx->cli_id);
	rsp = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp) {
		time = (u32_t) (atof(rsp) * 1000);
	}
	else {
		LOG_ERROR("[%p] cannot gettime", ctx);
	}

	NFREE(rsp);
	return time;
}


/*---------------------------------------------------------------------------*/
bool sq_set_time(sq_dev_handle_t handle, char *pos)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;

	if (!ctx->config.dynamic.use_cli) return false;

	if (!handle || !ctx->in_use) {
		LOG_ERROR("[%p]: no handle or cli socket %d", ctx, handle);
		return false;
	}

	sprintf(cmd, "%s time %s", ctx->cli_id, pos);
	LOG_INFO("[%p] time cmd %s", ctx, cmd);

	rsp = cli_send_cmd(cmd, false, true, ctx);
	if (!rsp) {
		LOG_ERROR("[%p] cannot settime %d", ctx, time);
		return false;
	}

	NFREE(rsp);
	return true;
}

/*--------------------------------------------------------------------------*/
static void sq_init_metadata(metadata_t *metadata)
{
	metadata->artist 	= NULL;
	metadata->album 	= NULL;
	metadata->title 	= NULL;
	metadata->genre 	= NULL;
	metadata->path 		= NULL;
	metadata->artwork 	= NULL;

	metadata->track 	= 0;
	metadata->index 	= 0;
	metadata->file_size = 0;
	metadata->duration 	= 0;
	metadata->remote 	= false;
}

/*--------------------------------------------------------------------------*/
void sq_default_metadata(metadata_t *metadata, bool init)
{
	if (init) sq_init_metadata(metadata);

	if (!metadata->title) metadata->title 	= strdup("[LMS]");
	if (!metadata->album) metadata->album 	= strdup("[no album]");
	if (!metadata->artist) metadata->artist = strdup("[no artist]");
	if (!metadata->genre) metadata->genre 	= strdup("[no genre]");
	/*
	if (!metadata->path) metadata->path = strdup("[no path]");
	if (!metadata->artwork) metadata->artwork = strdup("[no artwork]");
	*/
}


/*--------------------------------------------------------------------------*/
bool sq_get_metadata(sq_dev_handle_t handle, metadata_t *metadata, bool next)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[1024];
	char *rsp, *p;
	u16_t idx;

	if (!handle || !ctx->in_use || !ctx->config.dynamic.use_cli) {
		if (ctx->config.dynamic.use_cli) {
			LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		}
		sq_default_metadata(metadata, true);
		return false;
	}

	sprintf(cmd, "%s playlist index", ctx->cli_id);
	rsp = cli_send_cmd(cmd, true, true, ctx);

	if (!rsp || (rsp && !*rsp)) {
		LOG_ERROR("[%p]: missing index", ctx);
		NFREE(rsp);
		sq_default_metadata(metadata, true);
		return false;
	}

	sq_init_metadata(metadata);

	idx = atol(rsp);
	NFREE(rsp);
	metadata->index = idx;

	if (next) {
		sprintf(cmd, "%s playlist tracks", ctx->cli_id);
		rsp = cli_send_cmd(cmd, true, true, ctx);
		if (rsp && atol(rsp)) idx = (idx + 1) % atol(rsp);
		else idx = 0;
		NFREE(rsp);
	}

	sprintf(cmd, "%s playlist remote %d", ctx->cli_id, idx);
	rsp  = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp == '1') metadata->remote = true;
	else metadata->remote = false;
	NFREE(rsp)

	sprintf(cmd, "%s playlist path %d", ctx->cli_id, idx);
	rsp = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp) {
		metadata->path = rsp;
		metadata->track_hash = hash32(metadata->path);
		sprintf(cmd, "%s songinfo 0 10 url:%s tags:cfldatgrK", ctx->cli_id, metadata->path);
		rsp = cli_send_cmd(cmd, false, false, ctx);
	}

	if (rsp && *rsp) {
		metadata->title = cli_find_tag(rsp, "title");
		metadata->artist = cli_find_tag(rsp, "artist");
		metadata->album = cli_find_tag(rsp, "album");
		metadata->genre = cli_find_tag(rsp, "genre");

		if ((p = cli_find_tag(rsp, "duration")) != NULL) {
			metadata->duration = 1000 * atof(p);
			free(p);
		}

		if ((p = cli_find_tag(rsp, "filesize")) != NULL) {
			metadata->file_size = atol(p);
			/*
			at this point, LMS sends the original filesize, not the transcoded
			so it simply does not work
			*/
			metadata->file_size = 0;
			free(p);
		}

		if ((p = cli_find_tag(rsp, "tracknum")) != NULL) {
			metadata->track = atol(p);
			free(p);
		}

		metadata->artwork = cli_find_tag(rsp, "artwork_url");
		if (!metadata->artwork || !strlen(metadata->artwork)) {
			NFREE(metadata->artwork);
			if ((p = cli_find_tag(rsp, "coverid")) != NULL) {
				metadata->artwork = malloc(_STR_LEN_);
				snprintf(metadata->artwork, _STR_LEN_, "http://%s:%s/music/%s/cover.jpg", ctx->server_ip, ctx->server_port, p);
				free(p);
			}
		}
	}
	else {
		LOG_INFO("[%p]: no metadata using songinfo", ctx, idx);
		NFREE(rsp);

		sprintf(cmd, "%s playlist title %d", ctx->cli_id, idx);
		metadata->title = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s playlist album %d", ctx->cli_id, idx);
		metadata->album = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s playlist artist %d", ctx->cli_id, idx);
		metadata->artist = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s playlist genre %d", ctx->cli_id, idx);
		metadata->genre = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s status %d 1 tags:K", ctx->cli_id, idx);
		rsp = cli_send_cmd(cmd, false, false, ctx);
		if (rsp && *rsp) metadata->artwork = cli_find_tag(rsp, "artwork_url");
		NFREE(rsp);

		sprintf(cmd, "%s playlist duration %d", ctx->cli_id, idx);
		rsp = cli_send_cmd(cmd, true, true, ctx);
		if (rsp) metadata->duration = 1000 * atof(rsp);
	}
	NFREE(rsp);

	if (!next && metadata->duration) {
		sprintf(cmd, "%s time", ctx->cli_id);
		rsp = cli_send_cmd(cmd, true, true, ctx);
		if (rsp && *rsp) metadata->duration -= (u32_t) (atof(rsp) * 1000);
		NFREE(rsp);
	}

	if (metadata->artwork && !strncmp(metadata->artwork, IMAGEPROXY, strlen(IMAGEPROXY))) {
		char *artwork = malloc(_STR_LEN_);

		snprintf(artwork, _STR_LEN_, "http://%s:%s%s", ctx->server_ip, ctx->server_port, metadata->artwork);
		free(metadata->artwork);
		metadata->artwork = artwork;
	}

	sq_default_metadata(metadata, false);

	LOG_INFO("[%p]: idx %d\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n\tduration:%d.%03d\n\tsize:%d\n\tcover:%s", ctx, idx,
				metadata->artist, metadata->album, metadata->title,
				metadata->genre, div(metadata->duration, 1000).quot,
				div(metadata->duration,1000).rem, metadata->file_size,
				metadata->artwork ? metadata->artwork : "");

	return true;
}

/*--------------------------------------------------------------------------*/
void sq_free_metadata(metadata_t *metadata)
{
	NFREE(metadata->artist);
	NFREE(metadata->album);
	NFREE(metadata->title);
	NFREE(metadata->genre);
	NFREE(metadata->path);
	NFREE(metadata->artwork);
}

/*---------------------------------------------------------------------------*/
#if 0
void sq_reset_icy(struct out_ctx_s *p, bool init)
{
	if (init) {
		p->icy.last = gettime_ms();
		p->icy.remain = p->icy.interval;
		p->icy.update = false;
	}

	NFREE(p->icy.title);
	NFREE(p->icy.artist);
	NFREE(p->icy.artwork);
}

/*--------------------------------------------------------------------------*/
void sq_update_icy(struct out_ctx_s *p)
{
	char cmd[1024];
	char *rsp, *artist, *title, *artwork;
	u16_t idx;
	u32_t now = gettime_ms();
	struct thread_ctx_s *ctx = p->owner;

	if ((now - p->icy.last - 5000 > 0x7fffffff) || !p->icy.interval) return;
	p->icy.last = now;

	sprintf(cmd, "%s playlist index", ctx->cli_id);
	rsp = cli_send_cmd(cmd, true, true, ctx);

	if (!rsp || (rsp && !*rsp)) {
		LOG_ERROR("[%p]: missing index", ctx);
		NFREE(rsp);
		return;
	}

	idx = atol(rsp);
	NFREE(rsp);

	sprintf(cmd, "%s playlist path %d", ctx->cli_id, idx);
	rsp = cli_send_cmd(cmd, true, true, ctx);
	if (!rsp || hash32(rsp) != p->track_hash) {
		NFREE(rsp);
		return;
	}
	NFREE(rsp);

	sprintf(cmd, "%s playlist artist %d", ctx->cli_id, idx);
	artist = cli_send_cmd(cmd, true, true, ctx);
	if (artist && (!p->icy.artist || strcmp(p->icy.artist, artist))) {
		NFREE(p->icy.artist);
		p->icy.artist = strdup(artist);
		p->icy.update = true;
	}
	NFREE(artist);

	sprintf(cmd, "%s playlist title %d", ctx->cli_id, idx);
	title = cli_send_cmd(cmd, true, true, ctx);
	if (title && (!p->icy.title || strcmp(p->icy.title, title))) {
		NFREE(p->icy.title);
		p->icy.title = strdup(title);
		p->icy.update = true;
	}
	NFREE(title);

	sprintf(cmd, "%s status %d 1 tags:K", ctx->cli_id, idx);
	rsp = cli_send_cmd(cmd, false, false, ctx);
	if (rsp && *rsp) artwork = cli_find_tag(rsp, "artwork_url");
	else artwork = NULL;
	NFREE(rsp);

	if (artwork && (!p->icy.artwork || !strstr(p->icy.artwork, artwork)))  {
		NFREE(p->icy.artwork);

		if (!strncmp(artwork, IMAGEPROXY, strlen(IMAGEPROXY))) {
			p->icy.artwork = malloc(_STR_LEN_);
			snprintf(p->icy.artwork, _STR_LEN_, "http://%s:%s%s", ctx->server_ip, ctx->server_port, artwork);
		} else {
			p->icy.artwork = strdup(artwork);
		}

		p->icy.update = true;
	}

	NFREE(artwork);
}
#endif


/*---------------------------------------------------------------------------*/
void sq_notify(sq_dev_handle_t handle, void *caller_id, sq_event_t event, u8_t *cookie, void *param)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	LOG_SDEBUG("[%p] notif %d", ctx, event);

	// squeezelite device has not started yet or is off ...
	if (!ctx->running || !ctx->on || !handle || !ctx->in_use) return;

	switch (event) {
		case SQ_UNPAUSE:
		case SQ_PLAY:
			/*
			Whether it's sollicited or not, set the player's status. That
			covers the case where the player paused due to an error before
			the play state was actually captured. LMS will then received the
			unsollicited pause, pause the player then if the STMs was not
			already sent it will never be (case where gapless, the player will
			then issue a stop, which will make a self-play that will do an
			unsollicited play to which LMS answers by a play ... and that state
			change is missed)
			*/
			LOCK_O;
			ctx->output.render = RD_PLAYING;
			UNLOCK_O;
			if (* (bool*) param) {
				// unsollicited PLAY done on the player direclty
				char cmd[128], *rsp;

				LOG_WARN("[%p] unsollicited play", ctx);
				sprintf(cmd, "%s play", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			}
			else {
				LOG_INFO("[%p] playing notif", ctx);
				wake_controller(ctx);
			}
			break;
		case SQ_PAUSE: {
			char cmd[128], *rsp;

			LOG_WARN("[%p] unsollicited pause", ctx);
			sprintf(cmd, "%s pause", ctx->cli_id);
			rsp = cli_send_cmd(cmd, false, true, ctx);
			NFREE(rsp);
			break;
		}
		case SQ_STOP:
			if (* (bool*) param) {
				char cmd[128], *rsp;

				LOG_INFO("[%p] forced STOP", ctx);
				sprintf(cmd, "%s stop", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			}
			else {
				LOG_INFO("[%p] notify STOP", ctx);
				LOCK_O;
				ctx->output.render = RD_STOPPED;
				UNLOCK_O;
				wake_controller(ctx);
			}
			break;
		case SQ_SEEK:
			break;
		case SQ_VOLUME: {
			char cmd[128], *rsp;

			sprintf(cmd, "%s mixer volume %d", ctx->cli_id, *((u16_t*) param));
			rsp = cli_send_cmd(cmd, false, true, ctx);
			NFREE(rsp);
			break;
		}
		case SQ_TIME: {
			int time = *((unsigned*) param);

			LOG_DEBUG("[%p] time %d %d", ctx, ctx->output.ms_played, time);
			LOCK_O;
			ctx->output.ms_played = time;
			UNLOCK_O;
			break;
		}
		case SQ_TRACK_CHANGE:
			// so far nothing to be done
			LOCK_O;
			ctx->output.render = RD_PLAYING;
			UNLOCK_O;
			wake_controller(ctx);
			break;

		default: break;
	 }
 }

/*---------------------------------------------------------------------------*/
void sq_init(void)
{
	decode_init();
}

/*---------------------------------------------------------------------------*/
void sq_stop() {
	int i;

	for (i = 0; i < MAX_PLAYER; i++) {
		if (thread_ctx[i].in_use) {
			sq_wipe_device(&thread_ctx[i]);
		}
	}

	decode_end();
}

/*---------------------------------------------------------------------------*/
void sq_release_device(sq_dev_handle_t handle)
{
	if (handle) {
		struct thread_ctx_s *ctx = thread_ctx + handle - 1;
		int i;

		ctx->in_use = false;
		for (i = 0; ctx->mimecaps[i]; i++) free(ctx->mimecaps[i]);
	}
}

/*---------------------------------------------------------------------------*/
sq_dev_handle_t sq_reserve_device(void *MR, bool on, char *mimecaps[], sq_callback_t callback)
{
	int idx, i;
	struct thread_ctx_s *ctx;

	/* find a free thread context - this must be called in a LOCKED context */
	for  (idx = 0; idx < MAX_PLAYER; idx++)
		if (!thread_ctx[idx].in_use) break;

	if (idx < MAX_PLAYER) 	{
		// this sets a LOT of data to proper defaults (NULL, false ...)
		memset(&thread_ctx[idx], 0, sizeof(struct thread_ctx_s));
		thread_ctx[idx].in_use = true;
	}
	else return false;

	ctx = thread_ctx + idx;
	ctx->self = idx + 1;
	ctx->on = on;
	ctx->callback = callback;
	ctx->MR = MR;

	// copy the mime capabilities of the player
	for (i = 0; i < MAX_CAPS && mimecaps[i]; i++) ctx->mimecaps[i] = strdup(mimecaps[i]);

	return idx + 1;
}


/*---------------------------------------------------------------------------*/
bool sq_run_device(sq_dev_handle_t handle, sq_dev_param_t *param)
{
	int i;
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	memcpy(&ctx->config, param, sizeof(sq_dev_param_t));

	sprintf(ctx->cli_id, "%02x:%02x:%02x:%02x:%02x:%02x",
										  ctx->config.mac[0], ctx->config.mac[1], ctx->config.mac[2],
										  ctx->config.mac[3], ctx->config.mac[4], ctx->config.mac[5]);

	stream_thread_init(ctx->config.stream_buf_size, ctx);
	output_init(ctx->config.output_buf_size, ctx);
	decode_thread_init(ctx);
	slimproto_thread_init(ctx);

	return true;
}

/*--------------------------------------------------------------------------*/
void *sq_get_ptr(sq_dev_handle_t handle)
{
	if (!handle) return NULL;
	else return thread_ctx + handle - 1;
}

