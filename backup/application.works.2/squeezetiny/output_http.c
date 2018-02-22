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
#define MAX_BLOCK		(40*1024)

static bool handle_http(struct thread_ctx_s *ctx, int sock, char *mime);
static void mirror_header(key_data_t *src, key_data_t *rsp, char *key);

/*---------------------------------------------------------------------------*/
static void output_thread(struct thread_ctx_s *ctx) {
	bool http_ready, done = false;
	int sock = -1, chunk_count = 0;
	char *mime = NULL, chunk_frame_buf[16] = "", *chunk_frame = chunk_frame_buf;
	ssize_t bytes = 0;

	/*
	This function is higly non-linear and painful to read at first, I agree
	but it's also much easier, at the end, than a series of intricated if/else.
	Read it carefully, and then it's pretty simple
	*/

	while (ctx->output_running) {
		fd_set rfds, wfds;
		struct timeval timeout = {0, 50*1000};
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
				chunk_count = 0;
				FD_ZERO(&wfds);
			}

			if (sock != -1 && ctx->running) {
				LOG_INFO("[%p]: got HTTP connection %u", ctx, sock);
			} else continue;
		}

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		n = select(sock + 1, &rfds, &wfds, NULL, &timeout);

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
			mime = strdup(ctx->output.mime);
			UNLOCK_D;
		}

		// should be the HTTP headers (works with non-blocking socket)
		if (n > 0 && FD_ISSET(sock, &rfds)) http_ready = res = handle_http(ctx, sock, mime);

		// something wrong happened or master connection closed
		if (n < 0 || !res) {
			LOG_INFO("HTTP close %u", sock);
			closesocket(sock);
			sock = -1;
			continue;
		}

		// got a connection but a select timeout, so no HTTP headers yet
		if (!http_ready) continue;

		// first send any chunk framing (header, footer)
		if (*chunk_frame) {
			if (FD_ISSET(sock, &wfds)) {
				int n = send(sock, chunk_frame, strlen(chunk_frame), 0);
				if (n > 0) chunk_frame += n;
			}
			FD_SET(sock, &wfds);
			continue;
		}

		// then exit if needed (must be after footer has been sent - if any)
		if (done) {
			LOG_INFO("[%p]: self-exit ", ctx);
			break;
		}

		LOCK_O;

		// slimproto has not released us yet or we have been stopped
		if (ctx->output.state != OUTPUT_RUNNING) {
			UNLOCK_O;
			continue;
		}

		// now are surely running - socket is non blocking, so this is fast
		if (_output_bytes(ctx)) {
			ssize_t	space;

			// we cannot write, so don't bother
			if (!FD_ISSET(sock, &wfds)) {
				FD_SET(sock, &wfds);
				UNLOCK_O;
				continue;
			}

			space = min(_output_cont_bytes(ctx), MAX_BLOCK);

			// if chunked mode start by sending the header
			if (chunk_count) space = min(space, chunk_count);
			else if (ctx->output.chunked) {
				chunk_count = min(space, MAX_CHUNK_SIZE);
				sprintf(chunk_frame_buf, "%x\r\n", chunk_count);
				chunk_frame = chunk_frame_buf;
				UNLOCK_O;
				continue;
			}

			space = send(sock, (void*) _output_readp(ctx), space, 0);

			if (space > 0) {
				// first transmission, set flags for slimproto
				if (!bytes) _output_boot(ctx);

				// check for end of chunk - space cannot be bigger than chunk!
				if (chunk_count) {
					chunk_count -= space;
					if (!chunk_count) {
						strcpy(chunk_frame_buf, "\r\n");
						chunk_frame = chunk_frame_buf;
					}
				}

				bytes += space;

				LOG_INFO("[%p] sent %u bytes (total: %u)", ctx, space, bytes);
			}

			UNLOCK_O;
		} else {
			// check if all sent - LOCK_D shall always be locked before LOCK_O
			// or locked alone
			UNLOCK_O;
			LOCK_D;
			if (ctx->decode.state != DECODE_RUNNING) {
				// sending final empty chunk
				if (ctx->output.chunked) {
					strcpy(chunk_frame_buf, "0\r\n\r\n");
					chunk_frame = chunk_frame_buf;
				}
				done = true;
			}
			UNLOCK_D;
			// we don't have anything to send, let select read or sleep
			FD_ZERO(&wfds);
		}
	}

	LOG_INFO("[%p]: completed: %u bytes (gap %d)", ctx, bytes, ctx->output.length ? bytes - ctx->output.length : 0);

	LOCK_O;
	if (ctx->output_running == THREAD_RUNNING) ctx->output_running = THREAD_EXITED;
	UNLOCK_O;

	NFREE(mime);
	if (sock != -1) shutdown_socket(sock);
}

/*---------------------------------------------------------------------------*/
bool output_start(struct thread_ctx_s *ctx) {
	// already running, stop the thread but at least we can re-use the port
	if (ctx->output_running) {
		LOCK_O;
		ctx->output_running = THREAD_KILLED;
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

	ctx->output_running = THREAD_RUNNING;
	pthread_create(&ctx->output_thread, NULL, (void *(*)(void*)) &output_thread, ctx);

	return true;
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	int i;
	thread_state running;

	LOCK_O;
	running = ctx->output_running;
	ctx->output_running = THREAD_KILLED;
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
/*
So far, the diversity of behavior of UPnP devices is too large to do anything
that work for enough of them and handle byte seeking. So, we are either with
chunking or not and that's it. All this works very well with player that simply
suspend the connection using TCP, but if they close it and want to resume (i.e.
they request a range, we'll restart from where we were and mostly it will not be
acceptable by the player, so then use the option seek_after_pause
*/
static bool handle_http(struct thread_ctx_s *ctx, int sock, char *mime)
{
	char *body = NULL, *request = NULL, *str = NULL, *p;
	key_data_t headers[64], resp[16] = { { NULL, NULL } };
	char *head = "HTTP/1.1 200 OK";
	int len, index, i;
	bool res = true;

	if (!http_parse(sock, &request, headers, &body, &len)) {
		LOG_WARN("[%p]: http parsing error %s", ctx, request);
		res = -1;
		goto cleanup;
	}

	LOG_INFO("[%p]: received %s", ctx, request);
	sscanf(request, "%*[^/]/bridge-%u", &index);

	for (i = 0; headers[i].key; i++) {
		LOG_INFO("[%p]: %s: %s", ctx, headers[i].key, headers[i].data);
	}

	if (strstr(request, "1.1") && ctx->output.length == -3) ctx->output.chunked = true;
	else ctx->output.chunked = false;


	kd_add(resp, "Server", "squeezebox-bridge");
	kd_add(resp, "Connection", "close");

	// are we opening the expected file
	if (index != ctx->output.index) {
		LOG_WARN("wrong file requested, refusing %u %u", index, ctx->output.index);
		head = "HTTP/1.1 410 Gone";
		res = false;
	} else {
		kd_add(resp, "Content-Type", mime);
		kd_add(resp, "Accept-Ranges", "none");
		// kd_add(resp, "Cache-Control", "no-cache, no-store");
		// kd_add(resp, "Pragma", "no-cache");
		mirror_header(headers, resp, "TransferMode.DLNA.ORG");

		// a range request - might happen even when we said NO RANGE !!!
		if ((str = kd_lookup(headers, "Range")) != NULL) {
			int offset = 0;
			sscanf(str, "bytes=%u", &offset);
			if (offset) head = "HTTP/1.1 206 Partial Content";
		}
	}

	// do not send body if request is HEAD
	if (strstr(request, "HEAD")) res = false;
	else if (ctx->output.chunked) kd_add(resp, "Transfer-Encoding", "chunked");

	str = http_send(sock, head, resp);

	LOG_INFO("[%p]: responding:\n%s", ctx, str);

cleanup:
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
void wake_output(struct thread_ctx_s *ctx) {
	return;
}
