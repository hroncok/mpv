/*
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

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

#include <libavutil/md5.h>

#include "config.h"
#include "talloc.h"

#include "osdep/io.h"

#include "common/msg.h"
#include "options/path.h"
#include "options/m_config.h"
#include "options/parse_configfile.h"
#include "common/playlist.h"
#include "options/options.h"
#include "options/m_property.h"

#include "stream/stream.h"

#include "core.h"
#include "command.h"

#define DEF_CONFIG "# Write your default config options here!\n\n\n"

bool mp_parse_cfgfiles(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    m_config_t *conf = mpctx->mconfig;
    char *conffile;
    if (!opts->load_config)
        return true;
    if (!m_config_parse_config_file(conf, MPLAYER_CONFDIR "/mpv.conf", 0) < 0)
        return false;
    mp_mk_config_dir(mpctx->global, NULL);
    if (!(conffile = mp_find_user_config_file(NULL, mpctx->global, "config")))
        MP_ERR(mpctx, "mp_find_user_config_file(\"config\") problem\n");
    else {
        int fd = open(conffile, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
        if (fd != -1) {
            MP_INFO(mpctx, "Creating config file: %s\n", conffile);
            write(fd, DEF_CONFIG, sizeof(DEF_CONFIG) - 1);
            close(fd);
        }
        if (m_config_parse_config_file(conf, conffile, 0) < 0)
            return false;
        talloc_free(conffile);
    }
    return true;
}

// Set options file-local, and don't set them if the user set them via the
// command line.
#define FILE_LOCAL_FLAGS (M_SETOPT_BACKUP | M_SETOPT_PRESERVE_CMDLINE)

#define PROFILE_CFG_PROTOCOL "protocol."

static void mp_load_per_protocol_config(struct MPContext *mpctx)
{
    char *str;
    const char *file = mpctx->filename;
    char protocol[strlen(PROFILE_CFG_PROTOCOL) + strlen(file) + 1];
    m_profile_t *p;

    /* does filename actually uses a protocol ? */
    if (!mp_is_url(bstr0(file)))
        return;
    str = strstr(file, "://");
    if (!str)
        return;

    sprintf(protocol, "%s%s", PROFILE_CFG_PROTOCOL, file);
    protocol[strlen(PROFILE_CFG_PROTOCOL) + strlen(file) - strlen(str)] = '\0';
    p = m_config_get_profile0(mpctx->mconfig, protocol);
    if (p) {
        MP_INFO(mpctx, "Loading protocol-related profile '%s'\n", protocol);
        m_config_set_profile(mpctx->mconfig, p, FILE_LOCAL_FLAGS);
    }
}

#define PROFILE_CFG_EXTENSION "extension."

static void mp_load_per_extension_config(struct MPContext *mpctx)
{
    char *str;
    const char *file = mpctx->filename;
    char extension[strlen(PROFILE_CFG_EXTENSION) + 8];
    m_profile_t *p;

    /* does filename actually have an extension ? */
    str = strrchr(file, '.');
    if (!str)
        return;

    sprintf(extension, PROFILE_CFG_EXTENSION);
    strncat(extension, ++str, 7);
    p = m_config_get_profile0(mpctx->mconfig, extension);
    if (p) {
        MP_INFO(mpctx, "Loading extension-related profile '%s'\n", extension);
        m_config_set_profile(mpctx->mconfig, p, FILE_LOCAL_FLAGS);
    }
}

static void mp_load_per_output_config(struct MPContext *mpctx, char *cfg, char *out)
{
    char profile[strlen(cfg) + strlen(out) + 1];
    m_profile_t *p;

    if (!out && !out[0])
        return;

    sprintf(profile, "%s%s", cfg, out);
    p = m_config_get_profile0(mpctx->mconfig, profile);
    if (p) {
        MP_INFO(mpctx, "Loading extension-related profile '%s'\n", profile);
        m_config_set_profile(mpctx->mconfig, p, FILE_LOCAL_FLAGS);
    }
}

void mp_load_auto_profiles(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;

    mp_load_per_protocol_config(mpctx);
    mp_load_per_extension_config(mpctx);
    mp_load_per_file_config(mpctx);

    if (opts->vo.video_driver_list)
        mp_load_per_output_config(mpctx, "vo.", opts->vo.video_driver_list[0].name);
    if (opts->audio_driver_list)
        mp_load_per_output_config(mpctx, "ao.", opts->audio_driver_list[0].name);
}

/**
 * Tries to load a config file (in file local mode)
 * @return 0 if file was not found, 1 otherwise
 */
static int try_load_config(struct MPContext *mpctx, const char *file, int flags)
{
    if (!mp_path_exists(file))
        return 0;
    MP_INFO(mpctx, "Loading config '%s'\n", file);
    m_config_parse_config_file(mpctx->mconfig, file, flags);
    return 1;
}

void mp_load_per_file_config(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    char *confpath;
    char cfg[MP_PATH_MAX];
    const char *name;
    const char *file = mpctx->filename;

    if (strlen(file) > MP_PATH_MAX - 14) {
        MP_WARN(mpctx, "Filename is too long, "
               "can not load file or directory specific config files\n");
        return;
    }
    sprintf(cfg, "%s.conf", file);

    name = mp_basename(cfg);
    if (opts->use_filedir_conf) {
        char dircfg[MP_PATH_MAX];
        strcpy(dircfg, cfg);
        strcpy(dircfg + (name - cfg), "mpv.conf");
        try_load_config(mpctx, dircfg, FILE_LOCAL_FLAGS);

        if (try_load_config(mpctx, cfg, FILE_LOCAL_FLAGS))
            return;
    }

    if ((confpath = mp_find_user_config_file(NULL, mpctx->global, name))) {
        try_load_config(mpctx, confpath, FILE_LOCAL_FLAGS);

        talloc_free(confpath);
    }
}

#define MP_WATCH_LATER_CONF "watch_later"

static char *mp_get_playback_resume_config_filename(struct mpv_global *global,
                                                    const char *fname)
{
    char *res = NULL;
    void *tmp = talloc_new(NULL);
    const char *realpath = fname;
    bstr bfname = bstr0(fname);
    if (!mp_is_url(bfname)) {
        char *cwd = mp_getcwd(tmp);
        if (!cwd)
            goto exit;
        realpath = mp_path_join(tmp, bstr0(cwd), bstr0(fname));
    }
#if HAVE_DVDREAD || HAVE_DVDNAV
    if (bstr_startswith0(bfname, "dvd://"))
        realpath = talloc_asprintf(tmp, "%s - %s", realpath, dvd_device);
#endif
#if HAVE_LIBBLURAY
    if (bstr_startswith0(bfname, "br://") || bstr_startswith0(bfname, "bd://") ||
        bstr_startswith0(bfname, "bluray://"))
        realpath = talloc_asprintf(tmp, "%s - %s", realpath, bluray_device);
#endif
    uint8_t md5[16];
    av_md5_sum(md5, realpath, strlen(realpath));
    char *conf = talloc_strdup(tmp, "");
    for (int i = 0; i < 16; i++)
        conf = talloc_asprintf_append(conf, "%02X", md5[i]);

    conf = talloc_asprintf(tmp, "%s/%s", MP_WATCH_LATER_CONF, conf);

    res = mp_find_user_config_file(NULL, global, conf);

exit:
    talloc_free(tmp);
    return res;
}

static const char *backup_properties[] = {
    "osd-level",
    //"loop",
    "speed",
    "edition",
    "pause",
    "volume-restore-data",
    "audio-delay",
    //"balance",
    "fullscreen",
    "colormatrix",
    "colormatrix-input-range",
    "colormatrix-output-range",
    "ontop",
    "border",
    "gamma",
    "brightness",
    "contrast",
    "saturation",
    "hue",
    "deinterlace",
    "vf",
    "af",
    "panscan",
    "aid",
    "vid",
    "sid",
    "sub-delay",
    "sub-pos",
    "sub-visibility",
    "sub-scale",
    "ass-use-margins",
    "ass-vsfilter-aspect-compat",
    "ass-style-override",
    0
};

// Should follow what parser-cfg.c does/needs
static bool needs_config_quoting(const char *s)
{
    for (int i = 0; s && s[i]; i++) {
        unsigned char c = s[i];
        if (!isprint(c) || isspace(c) || c == '#' || c == '\'' || c == '"')
            return true;
    }
    return false;
}

void mp_write_watch_later_conf(struct MPContext *mpctx)
{
    void *tmp = talloc_new(NULL);
    char *filename = mpctx->filename;
    if (!filename)
        goto exit;

    double pos = get_current_time(mpctx);
    if (pos == MP_NOPTS_VALUE)
        goto exit;

    mp_mk_config_dir(mpctx->global, MP_WATCH_LATER_CONF);

    char *conffile = mp_get_playback_resume_config_filename(mpctx->global,
                                                            mpctx->filename);
    talloc_steal(tmp, conffile);
    if (!conffile)
        goto exit;

    MP_INFO(mpctx, "Saving state.\n");

    FILE *file = fopen(conffile, "wb");
    if (!file)
        goto exit;
    fprintf(file, "start=%f\n", pos);
    for (int i = 0; backup_properties[i]; i++) {
        const char *pname = backup_properties[i];
        char *val = NULL;
        int r = mp_property_do(pname, M_PROPERTY_GET_STRING, &val, mpctx);
        if (r == M_PROPERTY_OK) {
            if (needs_config_quoting(val)) {
                // e.g. '%6%STRING'
                fprintf(file, "%s=%%%d%%%s\n", pname, (int)strlen(val), val);
            } else {
                fprintf(file, "%s=%s\n", pname, val);
            }
        }
        talloc_free(val);
    }
    fclose(file);

exit:
    talloc_free(tmp);
}

void mp_load_playback_resume(struct MPContext *mpctx, const char *file)
{
    char *fname = mp_get_playback_resume_config_filename(mpctx->global, file);
    if (fname && mp_path_exists(fname)) {
        // Never apply the saved start position to following files
        m_config_backup_opt(mpctx->mconfig, "start");
        MP_INFO(mpctx, "Resuming playback. This behavior can "
               "be disabled with --no-resume-playback.\n");
        try_load_config(mpctx, fname, M_SETOPT_PRESERVE_CMDLINE);
        unlink(fname);
    }
    talloc_free(fname);
}

// Returns the first file that has a resume config.
// Compared to hashing the playlist file or contents and managing separate
// resume file for them, this is simpler, and also has the nice property
// that appending to a playlist doesn't interfere with resuming (especially
// if the playlist comes from the command line).
struct playlist_entry *mp_check_playlist_resume(struct MPContext *mpctx,
                                                struct playlist *playlist)
{
    if (!mpctx->opts->position_resume)
        return NULL;
    for (struct playlist_entry *e = playlist->first; e; e = e->next) {
        char *conf = mp_get_playback_resume_config_filename(mpctx->global,
                                                            e->filename);
        bool exists = conf && mp_path_exists(conf);
        talloc_free(conf);
        if (exists)
            return e;
    }
    return NULL;
}

