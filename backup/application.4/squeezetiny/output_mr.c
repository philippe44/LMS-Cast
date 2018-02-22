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

#define MAX_CHUNK_SIZE	(256*1024)
#define MAX_BLOCK	32768

static bool handle_http(struct thread_ctx_s *ctx, int sock, char *mime);
static void mirror_header(key_data_t *src, key_data_t *rsp, char *key);
bool send_chunk_header(int sock, size_t len, int flags);
bool send_chunk_footer(int sock, int flags);

/*---------------------------------------------------------------------------*/
bool send_chunk_header(int sock, size_t len, int flags) {
	char buf[16];
	fd_set wfds;
	struct timeval timeout = {0, 50*1000};
	int n;

	FD_ZERO(&wfds);
	FD_SET(sock, &wfds);

	// need to make sure we can go, but must an all-or-nothing
	n = select(sock, NULL, &wfds, NULL, &timeout);
	if (n > 0) {
		set_block(sock);
		sprintf(buf, "%x\r\n", len);
		// should not block forever for less than 10 bytes with select ok
		n = send(sock, (void*) buf, strlen(buf), 0);
		set_nonblock(sock);
	}

	LOG_SDEBUG("sending chunk header %u %d", len, n);

	return n == strlen(buf);
}


/*---------------------------------------------------------------------------*/
bool send_chunk_footer(int sock, int flags) {
	fd_set wfds;
	struct timeval timeout = {0, 50*1000};
	int n;

	FD_ZERO(&wfds);
	FD_SET(sock, &wfds);

	// need to make sure we can go, but must an all-or-nothing
	n = select(sock, NULL, &wfds, NULL, &timeout);
	if (n > 0) {
		set_block(sock);
		// should not block forever on less 2 bytes
		n = send(sock, "\r\n", 2, 0);
		set_nonblock(sock);
	}

	LOG_SDEBUG("sending chunk footer %d", n);

	return n == 2;
}


/*---------------------------------------------------------------------------*/
bool finish_chunk(int sock, int flags) {
	char buf[16];
	fd_set wfds;
	struct timeval timeout = {0, 100*1000};
	int n;

	FD_ZERO(&wfds);
	FD_SET(sock, &wfds);

	// need to make sure we can go, but must an all-or-nothing
	n = select(sock, NULL, &wfds, NULL, &timeout);
	if (n > 0) {
		set_block(sock);
		send(sock, "0\r\n\r\n", 5, 0);
		set_nonblock(sock);
	}

	LOG_SDEBUG("finishing chunking %d", n);

	return n == 5;
}


/*---------------------------------------------------------------------------*/
static void output_thread(struct thread_ctx_s *ctx) {
	bool http_ready = false;
	struct timeval timeout = {0, 50*1000};
	int sock = -1;
	size_t bytes = 0;
	char *mime = NULL;
	bool done = false;
	int chunk = 0;

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
				chunk = 0;
			}

			if (sock != -1 && ctx->running) {
				LOG_INFO("[%p]: got HTTP connection %u", ctx, sock);
			} else continue;
		}

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		n = select(sock + 1, &rfds, NULL, NULL, &timeout);
		timeout.tv_usec = 50*1000;

		// need to wait till we get a codec, the very unlikely case of "codc"
		if (!mime && n > 0) {
			LOCK_D;
			if (ctx->decode.state < DECODE_READY) {
				LOG_INFO("[%p]: need to wait for codec", ctx);
				UNLOCK_D;
				// not very elegant but let's not consume all CPU
				usleep(50*1000);
				continue;
			}
			mime = ctx->codec->get_mime(ctx);
			UNLOCK_D;
		}

		// should be the HTTP headers (works with non-blocking socket)
		if (n > 0) http_ready = res = handle_http(ctx, sock, mime);

		// something wrong happened or master connection closed
		if (n < 0 || !res) {
			LOG_INFO("HTTP close %u", sock);
			closesocket(sock);
			sock = -1;
			continue;
		}

		// got a connection but a select timeout, so no HTTP headers yet
		if (!http_ready) continue;

		// re-trying to send a chunk footer, should not happen often!
		if (chunk == -1) {
			LOG_DEBUG("[%p]: re-trying to send chunk footer", ctx);
			if (send_chunk_footer(sock, 0)) chunk = 0;
			continue;
		}

		LOCK_O;

		// nothing more to do at this point
		if (ctx->output.state != OUTPUT_RUNNING) {
			UNLOCK_O;
			continue;
		}

		// we are surely running and socket is non_blocking, so this is fast
		if (_buf_used(ctx->outputbuf)) {
			ssize_t	space = min(_buf_cont_read(ctx->outputbuf), MAX_BLOCK);

			// if chunked mode start by sending the header
			if (chunk) space = min(space, chunk);
			else if (ctx->config.stream_length == HTTP_CHUNKED) {
				int len = min(space, MAX_CHUNK_SIZE);
				if (!send_chunk_header(sock, len, 0)) {
					UNLOCK_O;
					continue;
				} else chunk = len;
			}

			// non-blocking socket, so this should be fast, can keep LOCK_O
			space = send(sock, (void*) ctx->outputbuf->readp, space, 0);

			if (space > 0) {
				// first transmission, set flags for slimproto
				if (!bytes) {
					if (ctx->output.track_start != ctx->outputbuf->readp) {
						LOG_ERROR("[%p] not a track boundary %p:%p",
									ctx, ctx->output.track_start, ctx->outputbuf->readp);
					}
					ctx->output.track_started = true;
					ctx->output.track_start = NULL;
				}

				// check for end of chunk - space cannot be bigger than chunk!
				if (chunk) {
					chunk -= space;
					if (!chunk && !send_chunk_footer(sock, 0)) chunk = -1;
				}

				bytes += space;
				LOG_SDEBUG("[%p] sent %u bytes (total: %u)", ctx, space, bytes);

				_buf_inc_readp(ctx->outputbuf, (size_t) space);

				// more data to send, don't wait in select
				if (_buf_used(ctx->outputbuf)) timeout.tv_usec = 0;
			}
			UNLOCK_O;
		} else {
			// check if all sent - LOCK_D shall always be locked before LOCK_O
			// or locked alone
			UNLOCK_O;
			LOCK_D;
			if (ctx->decode.state != DECODE_RUNNING) {
				// sending final empty chunk
				if (ctx->config.stream_length == HTTP_CHUNKED) finish_chunk(sock, 0);
				LOG_INFO("[%p]: self-exit ", ctx);
				done = true;
			}
			UNLOCK_D;
		}

		// request to exit unconditionally
		if (done) break;
	}

	LOG_INFO("[%p]: completed: %u bytes ", ctx, bytes);

	NFREE(mime);
	if (sock != -1) shutdown_socket(sock);
}

/*---------------------------------------------------------------------------*/
bool output_start(struct thread_ctx_s *ctx) {
	// already running, stop the thread but at least we can re-use the port
	if (ctx->output_running) {
		LOCK_O;
		ctx->output_running = false;
		UNLOCK_O;
		pthread_join(ctx->output_thread, NULL);
	} else {
		int i = 0;

		// find a free port
		ctx->output.port = GetLocalPort();
		ctx->output.http = bind_socket(&ctx->output.port, SOCK_STREAM);
		do {
			ctx->output.http = bind_socket(&ctx->output.port, SOCK_STREAM);
		} while (ctx->output.http < 0 && ctx->output.port++ && i++ < MAX_PLAYER);

		// and listen to it
		if (ctx->output.http <= 0 || listen(ctx->output.http, 1)) {
			closesocket(ctx->output.http);
			ctx->output.http = -1;
			return false;
		}
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
	ctx->output.ms_played = 0;
	/*
	Don't know actually if it's stopped or not but it will be and that stop event
	does not matter as we are flushing the whole thing. But if we want the next
	playback to work, better force that status to RD_STOPPED
	*/
	ctx->output.render = RD_STOPPED;
	if (ctx->output.state != OUTPUT_OFF) ctx->output.state = OUTPUT_STOPPED;
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
static bool handle_http(struct thread_ctx_s *ctx, int sock, char *mime)
{
	char *body = NULL, *request = NULL, *str = NULL, *head, *p;
	key_data_t headers[64], resp[16] = { { NULL, NULL } };
	int len, index;
	bool res = true;

	if (!http_parse(sock, &request, headers, &body, &len)) return false;

	LOG_INFO("[%p]: received %s", ctx, request);
	sscanf(request, "%*[^/]/bridge-%u", &index);

	kd_add(resp, "Server", "squeezebox-bridge");
	kd_add(resp, "Connection", "close");

	// are we opening the expected file
	if (index != ctx->output.index) {
		LOG_INFO("wrong file requested, refusing %u %u", index, ctx->output.index);
		head = "HTTP/1.0 410 Gone";
		res = false;
	} else {
		kd_add(resp, "Content-Type", mime);
		kd_add(resp, "Accept-Ranges", "none");

		if (ctx->config.stream_length == HTTP_CHUNKED) {
			mirror_header(headers, resp, "TransferMode.DLNA.ORG");
			kd_add(resp, "Transfer-Encoding", "chunked");
			head = "HTTP/1.1 200 OK";
		} else head = "HTTP/1.0 200 OK";

		// a range request not starting at 0 - we said NO RANGE !!!
		if ((str = kd_lookup(headers, "Range")) != NULL) {
			int n;
			sscanf(str, "bytes=%u", &n);
			if (n) {
				LOG_INFO("[%p]: non-zero range request ignored %u", ctx, n);
				for (n = 0; headers[n].key; n++) {
					LOG_INFO("[%p]: %s: %s", headers[n].key, headers[n].data);
				}
			}
		}
	}

	str = http_send(sock, head, resp);

	LOG_INFO("[%p]: responding:\n%s", ctx, str);

	NFREE(body);
	NFREE(str);
	NFREE(request);
	kd_free(resp);
	kd_free(headers);

	return res;
}


/*----------------------------------------------------------------------------*/
static void mirror_header(key_data_t *src, key_data_t *rsp, char *key) {
	char *data;

	data = kd_lookup(src, key);
	if (data) kd_add(rsp, key, data);
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

void _checkfade(bool fade, struct thread_ctx_s *ctx) {
}

void wake_output(struct thread_ctx_s *ctx) {
	return;
}
