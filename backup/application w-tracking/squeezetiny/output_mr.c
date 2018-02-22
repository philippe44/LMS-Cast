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

#include "squeezelite.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define LOCK_O 	 mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)

static bool handle_http(struct thread_ctx_s *ctx, int sock);

/*---------------------------------------------------------------------------*/
#ifdef CHUNKED
int send_data(int sock, void *data, int len, int flags) {
	char *chunk;
	int sent;

	asprintf(&chunk, "%x\r\n", len);
	send(sock, chunk, strlen(chunk), flags);
	free(chunk);
	sent = send(sock, data, len, flags);
	send(sock, "\r\n", 2, flags);

	return sent;
}
#else
#define send_data send
#endif

/*---------------------------------------------------------------------------*/
static void output_thread(struct thread_ctx_s *ctx) {
	bool http_ready = false;
	struct timeval timeout = {0, 50*1000};
	int sock = -1;
	u32_t bytes;

	while (ctx->output_running) {
		fd_set rfds;
		bool res = true;
		int n;

		if (sock == -1) {
			struct timeval timeout = {0, 50*1000};

			FD_ZERO(&rfds);
			FD_SET(ctx->output.http, &rfds);

			if (select(ctx->output.http + 1, &rfds, NULL, NULL, &timeout) > 0) {
				sock = accept(ctx->output.http, NULL, NULL);
				set_nonblock(sock);
				http_ready = false;
			}

			if (sock != -1 && ctx->running) {
				LOG_INFO("[%p]: got HTTP connection %u", ctx, sock);
			} else continue;
		}

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		n = select(sock + 1, &rfds, NULL, NULL, &timeout);
		timeout.tv_usec = 50*1000;

		// something received, should be the HTTP headers
		if (n > 0) http_ready = res = handle_http(ctx, sock);

		// something wrong happened or master connection closed
		if (n < 0 || !res) {
			LOG_INFO("HTTP close %u", sock);
			closesocket(sock);
			sock = -1;
			continue;
		}

		// got a connection but a select timeout, so no HTTP headers yet
		if (!http_ready) continue;

		LOCK_O;

		// beginning of streaming, need to wait for something in the buffer
		if (ctx->output.state == OUTPUT_TRACKING) {
			if (ctx->output.track_start && ctx->output.track_start == ctx->outputbuf->readp) {
				ctx->output.track_started = true;
				ctx->output.track_start = NULL;
				ctx->output.state = OUTPUT_RUNNING;
				bytes = 0;
				LOG_INFO("[%p]: track start boundary ", ctx);
			} else {
				UNLOCK_O;
				if (ctx->output.track_start) {
					LOG_ERROR("[%p]: not starting at the boundary of a track", ctx);
				}
				continue;
			}
		}

		// nothing more to do at this point
		if (ctx->output.state != OUTPUT_RUNNING) {
			UNLOCK_O;
			continue;
		}

		// here we are sure we are running
		if (_buf_used(ctx->outputbuf)) {
			ssize_t sent;
			size_t	space = _buf_cont_read(ctx->outputbuf);

			// track end signaled in buffer, need to detect it
			if (ctx->output.track_start && ctx->output.track_start > ctx->outputbuf->readp) {
				// reduce frames so we find the next track below
				space = min(space, (ctx->output.track_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
			}

			// non-blocking socket, so this should be fast, can keep LOCK_O
			sent = send_data(sock, (void*) ctx->outputbuf->readp, space, 0);

			if (sent > 0) {
				_buf_inc_readp(ctx->outputbuf, (size_t) sent);
				timeout.tv_usec = 0;
				bytes += sent;
				LOG_SDEBUG("[%p] sent %u bytes (total: %u)", ctx, sent, bytes);
			}
		} else {
			LOCK_D;
			// check if all sent (track ended or beginning of next track) and exit
			if (ctx->output.track_start == ctx->outputbuf->readp ||	ctx->decode.state != DECODE_RUNNING) {
				LOG_INFO("[%p]: track sent: %u bytes ", ctx, bytes);
				UNLOCK_D;
				UNLOCK_O;
				break;
			}
			UNLOCK_D;
		}

		UNLOCK_O;
	}

	if (sock != -1) shutdown_socket(sock);
}

/*---------------------------------------------------------------------------*/
bool output_start(struct thread_ctx_s *ctx) {
	// find a free port
	ctx->output.port = gl_http_port;
	if (gl_http_port) ctx->output.port += MAX_PLAYER;

	do {
		ctx->output.http = bind_socket(&ctx->output.port, SOCK_STREAM);
	} while (ctx->output.http < 0 && ctx->output.port-- > gl_http_port);

	// and listen to it
	if (ctx->output.http <= 0 || listen(ctx->output.http, 1)) {
		closesocket(ctx->output.http);
		ctx->output.http = -1;
		return false;
	}

	ctx->output_running = true;
	pthread_create(&ctx->output_thread, NULL, (void *(*)(void*)) &output_thread, ctx);

	return true;
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	int i;
	bool running;

	LOCK_O;
	running = ctx->output_running;
	ctx->output_running = false;
	if (ctx->output.state != OUTPUT_OFF) ctx->output.state = OUTPUT_STOPPED;
	if (ctx->output.render != RD_STOPPED) ctx->output.render = RD_ACQUIRE;
	UNLOCK_O;

	if (running) {
		pthread_join(ctx->output_thread, NULL);
		LOG_INFO("[%p]: terminating output thread", ctx);
		if (ctx->output.http) {
			shutdown_socket(ctx->output.http);
			ctx->output.http = -1;
		}
	}

	LOCK_O;
	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	UNLOCK_O;

	LOG_DEBUG("[%p]: flush output buffer", ctx);
	buf_flush(ctx->outputbuf);

}

/*----------------------------------------------------------------------------*/
static bool handle_http(struct thread_ctx_s *ctx, int sock)
{
	char *body = NULL, method[16] = "", *str, request[1024];
	key_data_t headers[64], resp[16] = { { NULL, NULL } };
	int len;

	if (!http_parse(sock, request, method, headers, &body, &len)) return false;

	LOG_INFO("[%p]: received %s", ctx, request);

	kd_add(resp, "Server", "squeezebox-bridge");
	//TODO
	kd_add(resp, "Content-Type", "audio/mp3");

#ifdef CHUNKED
	mirror_header(headers, resp, "Connection");
	mirror_header(headers, resp, "TransferMode.DLNA.ORG");
	kd_add(resp, "Transfer-Encoding", "chunked");

	str = http_send(sock, "HTTP/1.1 200 OK", resp);
#else
	kd_add(resp, "Connection", "close");

	str = http_send(sock, "HTTP/1.0 200 OK", resp);
#endif

	LOG_INFO("[%p]: responding:\n%s", ctx, str);

	NFREE(body);
	NFREE(str);
	kd_free(resp);
	kd_free(headers);

	return true;
}

/*---------------------------------------------------------------------------*/
void output_thread_init(unsigned outputbuf_size, struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p] init output media renderer", ctx);

	ctx->outputbuf = &ctx->__o_buf;
	buf_init(ctx->outputbuf, outputbuf_size);

	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	ctx->output_running = false;
	ctx->output.http = -1;
	ctx->output.render = RD_STOPPED;

	if (!ctx->outputbuf->buf) {
		LOG_ERROR("[%p]: unable to malloc output buffer", ctx);
		exit(0);
	}
}

/*---------------------------------------------------------------------------*/
void output_close(struct thread_ctx_s *ctx) {
	bool running;

	LOG_INFO("[%p] close media renderer", ctx);

	LOCK_O;
	running = ctx->output_running;
	ctx->output_running = false;
	UNLOCK_O;

	// still a race condition if a stream just ended a bit before ...
	if (running) pthread_join(ctx->output_thread, NULL);
}


void _checkfade(void) {
}

void wake_output(struct thread_ctx_s *ctx) {
	return;
}
