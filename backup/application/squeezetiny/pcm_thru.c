/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Philippe, philippe_44@outlook.com for raop/multi-instance modifications
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

extern log_level decode_loglevel;
static log_level *loglevel = &decode_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_O_direct   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)

#define BYTE_1(n)	((u8_t) (n >> 24))
#define BYTE_2(n)	((u8_t) (n >> 16))
#define BYTE_3(n)	((u8_t) (n >> 8))
#define BYTE_4(n)	((u8_t) (n))

/*---------------------------------- WAVE ------------------------------------*/
static struct wave_header_s {
	u8_t 	chunk_id[4];
	u32_t	chunk_size;
	u8_t	format[4];
	u8_t	subchunk1_id[4];
	u8_t	subchunk1_size[4];
	u8_t	audio_format[2];
	u16_t	channels;
	u32_t	sample_rate;
	u32_t   byte_rate;
	u16_t	block_align;
	u16_t	bits_per_sample;
	u8_t	subchunk2_id[4];
	u32_t	subchunk2_size;
} wave_header = {
		{ 'R', 'I', 'F', 'F' },
		0,
		{ 'W', 'A', 'V', 'E' },
		{ 'f','m','t',' ' },
		{ 16, 0, 0, 0 },
		{ 1, 0 },
		0,
		0,
		0,
		0,
		0,
		{ 'd', 'a', 't', 'a' },
		0, 		//chunk_size - sizeof(struct wave_header_s) - 8 - 8
	};

/*---------------------------------- AIFF ------------------------------------*/
static struct aiff_header_s {			// need to use all u8 due to padding
	u8_t 	chunk_id[4];
	u8_t	chunk_size[4];
	u8_t	format[4];
	u8_t	common_id[4];
	u8_t	common_size[4];
	u8_t	channels[2];
	u8_t 	frames[4];
	u8_t	sample_size[2];
	u8_t	sample_rate_exp[2];
	u8_t	sample_rate_num[8];
	u8_t    data_id[4];
	u8_t	data_size[4];
	u32_t	offset;
	u32_t	blocksize;
#define AIFF_PAD_SIZE	2				// C compiler adds a padding to that structure, need to discount it
	u8_t	pad[2];
} aiff_header = {
		{ 'F', 'O', 'R', 'M' },
#ifdef AIFF_MARKER
		{ 0x3B, 0x9A, 0xCA, 0x48 },		// adding comm, mark, ssnd and AIFF sizes
#else
		{ 0x3B, 0x9A, 0xCA, 0x2E },		// adding comm, ssnd and AIFF sizes
#endif
		{ 'A', 'I', 'F', 'F' },
		{ 'C', 'O', 'M', 'M' },
		{ 0x00, 0x00, 0x00, 0x12 },
		{ 0x00, 0x00 },
		{ 0x0E, 0xE6, 0xB2, 0x80 },		// 250x10^6 frames of 2 channels and 2 bytes each
		{ 0x00, 0x00 },
		{ 0x40, 0x0E },
		{ 0x00, 0x00 },
		{ 'S', 'S', 'N', 'D' },
		{ 0x3B, 0x9A, 0xCA, 0x08 },		// one chunk of 10^9 bytes + 8
		0,
		0
	};

struct pcm {
	u32_t sample_rate;
	u8_t sample_size;
	u8_t channels;
	size_t bytes_per_frame;
	bool swap;
	u8_t out_sample_size;
	bool big_endian;
	char format;
	char mime[_STR_LEN_];
	u8_t *header;
	u64_t size;
};

static void swap(u8_t *src, u8_t *dst, size_t bytes, u8_t size);
static void truncate(u8_t *src, u8_t *dst, size_t bytes, bool swap, bool big);
static void apply_gain(void *p, u32_t gain, size_t bytes, u8_t size, bool big_endian);

static void little16(void *dst, u16_t src);
static void little32(void *dst, u32_t src);
static void big16(void *dst, u16_t src);
static void big32(void *dst, u32_t src);

/*---------------------------------------------------------------------------*/
static decode_state pcm_decode(struct thread_ctx_s *ctx) {
	size_t in, out;
	struct pcm *p = ctx->decode.handle;
	u8_t *iptr, *optr, ibuf[3*8], obuf[3*8];

	LOCK_S;
	LOCK_O_direct;

	in = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	if (ctx->stream.state <= DISCONNECT && in == 0) {
		UNLOCK_O_direct;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	iptr = (u8_t *)ctx->streambuf->readp;

	if (ctx->decode.new_stream) {
		ctx->output.track_start = ctx->outputbuf->writep;

		if (p->big_endian && !(*((u64_t*) iptr)) &&
		   (strstr(ctx->server_version, "7.7") || strstr(ctx->server_version, "7.8"))) {
			/*
			LMS < 7.9 does not remove 8 bytes when sending aiff files but it does
			when it is a transcoding ... so this does not matter for 16 bits samples
			but it is a mess for 24 bits ... so this tries to guess what we are
			receiving
			*/
			_buf_inc_readp(ctx->streambuf, 8);
			in = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

			LOG_INFO("[%p]: guessing a AIFF extra header", ctx);
		}
	}

	// the min in and out are enough to process a full header
	if (p->header) {
		size_t size;

		if (p->format == 'w') size = sizeof(struct wave_header_s);
		else size = sizeof(struct aiff_header_s);

		out = min(size, _buf_cont_write(ctx->outputbuf));
		memcpy(ctx->outputbuf->writep, p->header, out);
		memcpy(ctx->outputbuf->buf, p->header + out, size - out);
		_buf_inc_writep(ctx->outputbuf, size);

		free(p->header);
		p->header = NULL;
	}

	if (ctx->decode.new_stream) {
		LOG_INFO("[%p]: setting track_start", ctx);

		//FIXME: not in use for now, sample rate always same how to know starting rate when resamplign will be used
		ctx->output.current_sample_rate = decode_newstream(p->sample_rate, ctx->output.supported_rates, ctx);
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;
	}

	out = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf));

	// no enough cont'd place in input
	if (in < p->bytes_per_frame) {
		memcpy(ibuf, iptr, in);
		memcpy(ibuf + in, ctx->streambuf->buf, p->bytes_per_frame - in);
		iptr = ibuf;
		in = p->bytes_per_frame;
	}

	// not enough cont'd place in output
	if (out < p->bytes_per_frame) {
		optr = obuf;
		out = p->bytes_per_frame;
	} else optr = ctx->outputbuf->writep;

	// might be just bytes_per_frames
	in = out = (min(in, out) / p->bytes_per_frame) * p->bytes_per_frame;

	// truncate or swap
	if (p->sample_size == 24 && ctx->config.L24_format == L24_TRUNC_16) {
		truncate(optr, iptr, in, p->swap, p->format == 'w');
		out = (out * 2) / 3;
	} else {
		if (p->swap) swap(optr, iptr, in, p->sample_size);
		else memcpy(optr, iptr, in);
	}

	// apply gain if any
	apply_gain(optr, ctx->output.replay_gain, out, p->out_sample_size, p->format == 'w');

	// take the data from temporary buffer if needed
	if (optr == obuf) {
		size_t out = _buf_cont_write(ctx->outputbuf);
		memcpy(ctx->outputbuf->writep, optr, out);
		memcpy(ctx->outputbuf->buf, optr + out, p->bytes_per_frame - out);
	}

	_buf_inc_readp(ctx->streambuf, in);
	_buf_inc_writep(ctx->outputbuf, out);

	UNLOCK_O_direct;
	UNLOCK_S;

	return DECODE_RUNNING;
}


/*---------------------------------------------------------------------------*/
static void pcm_open(struct track_param *param, struct thread_ctx_s *ctx) {
	struct pcm *p = ctx->decode.handle;
	u64_t size;

	if (!p)	p = ctx->decode.handle = malloc(sizeof(struct pcm));

	if (!p) return;

	p->sample_size = param->sample_size;
	p->sample_rate = param->sample_rate,
	p->channels = param->channels;
	p->bytes_per_frame = (param->sample_size * param->channels) / 8;
	p->big_endian = (param->endianness == 0);
	strcpy(p->mime, param->mime);
	p->out_sample_size = (p->sample_size == 24 && ctx->config.L24_format == L24_TRUNC_16) ? 16 : p->sample_size;

	/*
	do not create a size (content-length) when we really don't know it but
	when we set a content-length (http_mode > 0) then at least the headers
	should be consistent
	*/
	if (param->duration) {
		div_t duration = div(param->duration, 1000);
		p->size = duration.quot * (u64_t) p->sample_rate * (u64_t) (p->out_sample_size/8) * (u64_t) p->channels;
		p->size += (duration.rem * (u64_t) p->sample_rate * (u64_t) (p->out_sample_size/8) * (u64_t) p->channels) / 1000;
		size = p->size;
	} else {
		// we will send a content-length, so set headers consistently
		if (ctx->config.stream_length > 0) {
			p->size = ctx->config.stream_length;
			size = p->size;
		} else {
			p->size = 0;
			size = MAX_FILE_SIZE;
		}
	}

	LOG_INFO("pcm size: %u rate: %u chan: %u bigendian: %u", p->sample_size, p->sample_rate, p->channels, param->endianness);

	if (strstr(p->mime, "wav")) {
		FILE *f;
		struct wave_header_s *header = malloc(sizeof(struct wave_header_s));

		memcpy(header, &wave_header, sizeof(struct wave_header_s));
		little16(&header->channels, p->channels);
		little16(&header->bits_per_sample, p->out_sample_size);
		little32(&header->sample_rate, p->sample_rate);
		little32(&header->byte_rate, p->sample_rate * p->channels * (p->out_sample_size / 8));
		little16(&header->block_align, p->channels * (p->out_sample_size / 8));
		little32(&header->subchunk2_size, size);
		little32(&header->chunk_size, 36 + header->subchunk2_size);
		p->header = (u8_t*) header;
		p->format = 'w';
		p->swap = (param->endianness == 0);
		if (p->size) p->size += 36 + 8;
	} else if (strstr(p->mime, "aiff")) {
		struct aiff_header_s *header = malloc(sizeof(struct aiff_header_s));

		memcpy(header, &aiff_header, sizeof(struct aiff_header_s));
		big16(header->channels, p->channels);
		big16(header->sample_size, p->out_sample_size);
		big16(header->sample_rate_num, p->sample_rate);
		big32(&header->data_size, size + 8);
		big32(&header->chunk_size, (size+8+8) + (18+8) + 4);
		big32(&header->frames, size / p->sample_rate);
		p->header = (u8_t*) header;
		// BEWARE TO WRITE JUST: sizeof(struct aiff_header_s) - AIFF_PAD_SIZE
		p->format = 'i';
		p->swap = (param->endianness == 1);
		if (p->size) p->size += (8+8) + (18+8) + 4 + 8;
	} else {
		p->format = 'p';
		p->header = NULL;
		p->swap = (param->endianness == 1);
	}

	LOG_INFO("[%p]: estimated size %u", ctx, p->size);
}


/*---------------------------------------------------------------------------*/
static char* pcm_mime(struct thread_ctx_s *ctx) {
	struct pcm *p = ctx->decode.handle;
	return strdup(p->mime);
}


/*---------------------------------------------------------------------------*/
static u64_t pcm_size(struct thread_ctx_s *ctx) {
	struct pcm *p = ctx->decode.handle;
	return p->size;
}

/*---------------------------------------------------------------------------*/
static void pcm_close(struct thread_ctx_s *ctx) {
	struct pcm *p = ctx->decode.handle;

	if (p) free(p);
	ctx->decode.handle = NULL;
}


/*---------------------------------------------------------------------------*/
struct codec *register_pcm(void) {
	static struct codec ret = {
		'p',         // id
		"pcm,wav,aif", 		 // types
		4096,        // min read
		16*1024,     // min space
		pcm_open,   // open
		pcm_close,  // close
		pcm_decode, // decode
		pcm_mime,	// get mime type
		pcm_size,	// get total length, 0 is unknown
	};

	LOG_INFO("using pcm", NULL);
	return &ret;
}


void deregister_pcm(void) {
}


/*---------------------------------------------------------------------------*/
static void swap(u8_t *dst, u8_t *src, size_t bytes, u8_t size)
{
	switch (size) {
	case 8:
		memcpy(src, dst, bytes);
		break;
	case 16:
		bytes /= 2;
		src += 1;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src;
			src += 3;
		}
		break;
	case 24:
		bytes /= 3;
		src += 2;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src--;
			*dst++ = *src;
			src += 5;
		}
		break;
	 case 32:
		bytes /= 4;
		src += 3;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src--;
			*dst++ = *src--;
			*dst++ = *src;
			src += 7;
		}
		break;
	}
}


/*---------------------------------------------------------------------------*/
static void truncate(u8_t *dst, u8_t *src, size_t bytes, bool swap, bool big)
{
	bytes /= 3;
	if (big) src++;

	if (swap) {
		if (!big) src += 2;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src;
			src += 4;
		}
	}
	else {
		while (bytes--) {
			*dst++ = *src++;
			*dst++ = *src++;
			src++;
		}
	}
}

#define MAX_VAL8  0x7fffffLL
#define MAX_VAL16 0x7fffffffLL
#define MAX_VAL24 0x7fffffffffLL
#define MAX_VAL32 0x7fffffffffffLL
// this probably does not work for little-endian CPU
/*---------------------------------------------------------------------------*/
static void apply_gain(void *p, u32_t gain, size_t bytes, u8_t size, bool big_endian)
{
	size_t i = bytes / (size / 8);
	s64_t sample;

	if (!gain || gain == 65536) return;

#if !SL_LITTLE_ENDIAN
	big_endian = !big_endian;
#endif

	if (size == 8) {
		u8_t *buf = p;
		while (i--) {
			sample = *buf * (s64_t) gain;
			if (sample > MAX_VAL8) sample = MAX_VAL8;
			else if (sample < -MAX_VAL8) sample = -MAX_VAL8;
			*buf++ = sample >> 16;
		}
		return;
	}

	// big endian target on big endian CPU or the opposite
	if (big_endian) {

		if (size == 16) {
			s16_t *buf = p;
			while (i--) {
				sample = *buf * (s64_t) gain;
				if (sample > MAX_VAL16) sample = MAX_VAL16;
				else if (sample < -MAX_VAL16) sample = -MAX_VAL16;
				*buf++ = sample >> 16;
		   }
		   return;
		}

		if (size == 24) {
			u8_t *buf = p;
			while (i--) {
				// for 24 bits samples, first put the sample in the 3 upper bytes
				sample = (s32_t) ((((u32_t) *buf) << 8) | ((u32_t) *(buf+1) << 16) | ((u32_t) *(buf+2) << 24)) * (s64_t) gain;
				if (sample > MAX_VAL24) sample = MAX_VAL24;
				else if (sample < -MAX_VAL24) sample = -MAX_VAL24;
				sample >>= 16;
				*buf++ = sample;
				*buf++ = sample >> 8;
				*buf++ = sample >> 16;
			}
			return;
		}

		if (size == 32) {
			s32_t *buf = p;
			while (i--) {
				sample = *buf * (s64_t) gain;
				if (sample > MAX_VAL32) sample = MAX_VAL32;
				else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
				*buf++ = sample >> 16;
			}
			return;
		}

	} else {

		if (size == 16) {
			s16_t *buf = p;
			while (i--) {
				sample = (s16_t) (((*buf << 8) & 0xff00) | ((*buf >> 8) & 0xff)) * (s64_t) gain;
				if (sample > MAX_VAL16) sample = MAX_VAL16;
				else if (sample < -MAX_VAL16) sample = -MAX_VAL16;
				sample >>= 16;
				*buf++ = ((sample >> 8) & 0xff) | ((sample << 8) & 0xff00);
			}
			return;
		}

		if (size == 24) {
			u8_t *buf = p;
			while (i--) {
				sample = (s32_t) ((((u32_t) *buf) << 24) | ((u32_t) *(buf+1) << 16) | ((u32_t) *(buf+2) << 8)) * (s64_t) gain;
				if (sample > MAX_VAL24) sample = MAX_VAL24;
				else if (sample < -MAX_VAL24) sample = -MAX_VAL24;
				sample >>= 16;
				*buf++ = sample >> 16;
				*buf++ = sample >> 8;
				*buf++ = sample;
		   }
		   return;
		}

		if (size == 32) {
			s32_t *buf = p;
			while (i--) {
				sample = *buf * (s64_t) gain;
				if (sample > MAX_VAL32) sample = MAX_VAL32;
				else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
				*buf++ = sample >> 16;
			}
			return;
		}
	}
}


/*---------------------------------------------------------------------------*/
static void little16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p = (u8_t) (src >> 8);
}

/*---------------------------------------------------------------------------*/
static void little32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p++ = (u8_t) (src >> 8);
	*p++ = (u8_t) (src >> 16);
	*p = (u8_t) (src >> 24);

}
/*---------------------------------------------------------------------------*/
static void big16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}

/*---------------------------------------------------------------------------*/
static void big32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 24);
	*p++ = (u8_t) (src >> 16);
	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}




