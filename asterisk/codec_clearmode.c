/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    codec_clearmode.c - the CLEARMODE pseudo-codec for Asterisk: RFC
 *                        4040's 64 kbit/s clear channel (an ISDN data
 *                        call's B-channel) as an RTP payload, so that
 *                        chan_pjsip can offer and take it (allow =
 *                        clearmode) and DModem() can run such a call
 *
 *    The codec is the octets as they are, 8000 a second: a sample an
 *    octet, frames of 10 to 150 ms.  RTP has it at a dynamic payload
 *    type as CLEARMODE/8000.  There is no translator: a clear call is
 *    data, and nothing may turn it into audio or back, so a CLEARMODE
 *    channel bridges only with another CLEARMODE channel (or goes to an
 *    application that reads it as it is, such as DModem()).
 *
 *    Only Asterisk's public API is used (ast_codec_register(),
 *    ast_format_create(), ast_format_cache_set(),
 *    ast_rtp_engine_load_format()).  An idea from a gist by 7marcus9
 *    (codec_clearmode.c), written anew.
 *
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include "asterisk/codec.h"
#include "asterisk/format.h"
#include "asterisk/format_cache.h"
#include "asterisk/frame.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/rtp_engine.h"

/* the codec's and the format's name (allow = clearmode); RTP's MIME
   subtype is the name in capitals (RFC 4040: CLEARMODE) */
#define NAME "clearmode"

/* a frame's samples: one an octet */
static int clearmode_samples(struct ast_frame *frame)
{
	return frame->datalen;
}

/* the octets of samples samples */
static int clearmode_length(unsigned int samples)
{
	return (int)samples;
}

static struct ast_codec clearmode = {
	.name = NAME,
	.description = "RFC 4040 64 kbit/s clear channel (ISDN data)",
	.type = AST_MEDIA_TYPE_AUDIO,
	.sample_rate = 8000,
	.minimum_ms = 10,
	.maximum_ms = 150,
	.default_ms = 20,
	.minimum_bytes = 80,
	.samples_count = clearmode_samples,
	.get_length = clearmode_length,
	.smooth = 1,
};

/* the format, kept for unloading RTP's mapping */
static struct ast_format *format;

/* RTP's mapping gone (a registered codec stays until Asterisk stops) */
static int unload_module(void)
{
	if (format) {
		ast_rtp_engine_unload_format(format);
		ao2_ref(format, -1);
		format = NULL;
	}
	return 0;
}

/* the codec, its format in the cache (for allow = clearmode), RTP's
   mapping; declined if any fails.  Loaded again in the same Asterisk
   (the codec is still there), only the format and the mapping */
static int load_module(void)
{
	struct ast_codec *codec = ast_codec_get(NAME, AST_MEDIA_TYPE_AUDIO,
						8000);

	if (!codec) {
		if (ast_codec_register(&clearmode)) {
			ast_log(LOG_ERROR, "cannot register the %s codec\n",
				NAME);
			return AST_MODULE_LOAD_DECLINE;
		}
		codec = ast_codec_get(NAME, AST_MEDIA_TYPE_AUDIO, 8000);
		if (!codec) {
			ast_log(LOG_ERROR, "the %s codec is not there\n",
				NAME);
			return AST_MODULE_LOAD_DECLINE;
		}
	}
	format = ast_format_create(codec);
	ao2_ref(codec, -1);
	if (!format) {
		ast_log(LOG_ERROR, "cannot create the %s format\n", NAME);
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_format_cache_set(format) ||
	    ast_rtp_engine_load_format(format)) {
		ast_log(LOG_ERROR, "cannot set up the %s format\n", NAME);
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

/* first of all: an allow = clearmode anywhere (res_pjsip's endpoints
   load before the channel drivers) needs the format there */
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"CLEARMODE pseudo-codec (RFC 4040, ISDN data calls)",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_REALTIME_DEPEND,
);
