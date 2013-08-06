/*
 * The Bauer stereophonic-to-binaural DSP using bs2b library:
 * http://bs2b.sourceforge.net/
 *
 * Copyright (c) 2009 Andrew Savchenko
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <bs2b.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "af.h"
#include "mpvcore/m_option.h"

/// Internal specific data of the filter
struct af_bs2b {
    int fcut;           ///< cut frequency in Hz
    int feed;           ///< feed level for low frequencies in 0.1*dB
    int profile  ;      ///< profile (available crossfeed presets)
    t_bs2bdp filter;    ///< instance of a library filter
};

#define PLAY(name, type) \
static struct mp_audio *play_##name(struct af_instance *af, struct mp_audio *data) \
{ \
    /* filter is called for all pairs of samples available in the buffer */ \
    bs2b_cross_feed_##name(((struct af_bs2b*)(af->priv))->filter, \
        (type*)(data->audio), data->len/data->bps/2); \
\
    return data; \
}

PLAY(f, float)
PLAY(fbe, float)
PLAY(fle, float)
PLAY(s32be, int32_t)
PLAY(u32be, uint32_t)
PLAY(s32le, int32_t)
PLAY(u32le, uint32_t)
PLAY(s24be, bs2b_int24_t)
PLAY(u24be, bs2b_uint24_t)
PLAY(s24le, bs2b_int24_t)
PLAY(u24le, bs2b_uint24_t)
PLAY(s16be, int16_t)
PLAY(u16be, uint16_t)
PLAY(s16le, int16_t)
PLAY(u16le, uint16_t)
PLAY(s8, int8_t)
PLAY(u8, uint8_t)


/// Initialization and runtime control
static int control(struct af_instance *af, int cmd, void *arg)
{
    struct af_bs2b *s = af->priv;

    switch (cmd) {
    case AF_CONTROL_REINIT: {
        int format;
        char buf[256];
        // Sanity check
        if (!arg) return AF_ERROR;

        format           = ((struct mp_audio*)arg)->format;
        af->data->rate   = ((struct mp_audio*)arg)->rate;
        mp_audio_set_num_channels(af->data, 2);     // bs2b is useful only for 2ch audio
        mp_audio_set_format(af->data, format);

        /* check for formats supported by libbs2b
           and assign corresponding handlers */
        switch (format) {
            case AF_FORMAT_FLOAT_BE:
                af->play = play_fbe;
                break;
            case AF_FORMAT_FLOAT_LE:
                af->play = play_fle;
                break;
            case AF_FORMAT_S32_BE:
                af->play = play_s32be;
                break;
            case AF_FORMAT_U32_BE:
                af->play = play_u32be;
                break;
            case AF_FORMAT_S32_LE:
                af->play = play_s32le;
                break;
            case AF_FORMAT_U32_LE:
                af->play = play_u32le;
                break;
            case AF_FORMAT_S24_BE:
                af->play = play_s24be;
                break;
            case AF_FORMAT_U24_BE:
                af->play = play_u24be;
                break;
            case AF_FORMAT_S24_LE:
                af->play = play_s24le;
                break;
            case AF_FORMAT_U24_LE:
                af->play = play_u24le;
                break;
            case AF_FORMAT_S16_BE:
                af->play = play_s16be;
                break;
            case AF_FORMAT_U16_BE:
                af->play = play_u16be;
                break;
            case AF_FORMAT_S16_LE:
                af->play = play_s16le;
                break;
            case AF_FORMAT_U16_LE:
                af->play = play_u16le;
                break;
            case AF_FORMAT_S8:
                af->play = play_s8;
                break;
            case AF_FORMAT_U8:
                af->play = play_u8;
                break;
            default:
                af->play = play_f;
                mp_audio_set_format(af->data, AF_FORMAT_FLOAT_NE);
                break;
        }

        // bs2b have srate limits, try to resample if needed
        if (af->data->rate > BS2B_MAXSRATE || af->data->rate < BS2B_MINSRATE) {
            af->data->rate = BS2B_DEFAULT_SRATE;
            mp_msg(MSGT_AFILTER, MSGL_WARN,
                   "[bs2b] Requested sample rate %d Hz is out of bounds [%d..%d] Hz.\n"
                   "[bs2b] Trying to resample to %d Hz.\n",
                   af->data->rate, BS2B_MINSRATE, BS2B_MAXSRATE, BS2B_DEFAULT_SRATE);
        }
        bs2b_set_srate(s->filter, (long)af->data->rate);
        mp_msg(MSGT_AFILTER, MSGL_V, "[bs2b] using format %s\n",
               af_fmt2str(af->data->format,buf,256));

        return af_test_output(af,(struct mp_audio*)arg);
    }
    }
    return AF_UNKNOWN;
}

/// Deallocate memory and close library
static void uninit(struct af_instance *af)
{
    struct af_bs2b *s = af->priv;
    free(af->data);
    if (s->filter)
        bs2b_close(s->filter);
}

/// Allocate memory, set function pointers and init library
static int af_open(struct af_instance *af)
{
    struct af_bs2b *s = af->priv;
    af->control = control;
    af->uninit  = uninit;
    af->mul     = 1;
    if (!(af->data = calloc(1, sizeof(struct mp_audio))))
        return AF_ERROR;

    // NULL means failed initialization
    if (!(s->filter = bs2b_open())) {
        free(af->data);
        return AF_ERROR;
    }

    if (s->profile)
        bs2b_set_level(s->filter, s->profile);
    // set fcut and feed only if specified, otherwise defaults will be used
    if (s->fcut)
        bs2b_set_level_fcut(s->filter, s->fcut);
    if (s->feed)
        bs2b_set_level_feed(s->filter, s->feed);
    return AF_OK;
}

#define OPT_BASE_STRUCT struct af_bs2b

/// Description of this filter
struct af_info af_info_bs2b = {
    "Bauer stereophonic-to-binaural audio filter",
    "bs2b",
    "Andrew Savchenko",
    "",
    AF_FLAGS_REENTRANT,
    af_open,
    .priv_size = sizeof(struct af_bs2b),
    .options = (const struct m_option[]) {
        OPT_INTRANGE("fcut", fcut, 0, BS2B_MINFCUT, BS2B_MAXFCUT),
        OPT_INTRANGE("feed", feed, 0, BS2B_MINFEED, BS2B_MAXFEED),
        OPT_CHOICE("profile", profile, 0,
                   ({"default", BS2B_DEFAULT_CLEVEL},
                    {"cmoy", BS2B_CMOY_CLEVEL},
                    {"jmeier", BS2B_JMEIER_CLEVEL})),
        {0}
    },
};
