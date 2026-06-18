/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Album-art thumbnail cache for the database browser.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#include "aa_thumb.h"

#ifdef HAVE_DB_ALBUMART

#include <string.h>
#include "system.h"
#include "file.h"          /* MAX_PATH */
#include "kernel.h"        /* current_tick */
#include "string-extra.h"  /* strmemccpy */
#include "metadata.h"      /* struct mp3entry */
#include "albumart.h"      /* find_albumart */
#include "bmp.h"           /* read_bmp_file, BM_SCALED_SIZE, struct dim */
#ifdef HAVE_JPEG
#include "jpeg_load.h"     /* clip_jpeg_fd - decode embedded (JPEG) cover art */
#endif

/* How many distinct thumbnails to keep decoded at once. A few screens worth is
 * plenty: only the visible rows are ever requested per draw, so this just needs
 * to cover the working set while scrolling. */
#define AA_THUMB_COUNT 16

/* Bytes needed to hold one AA_THUMB_MAX-square native bitmap plus the scaler's
 * working overhead (see BM_SCALED_SIZE in bmp.h). */
#define AA_THUMB_BUFSZ BM_SCALED_SIZE(AA_THUMB_MAX, AA_THUMB_MAX, FORMAT_NATIVE, false)

#ifdef HAVE_JPEG
/* Embedded cover art (usually a JPEG inside the file's tags) is decoded
 * straight from the track. The JPEG decoder needs a scratch area of
 * JPEG_DECODE_OVERHEAD (38KB plus the decoder state struct) on top of the
 * scaled output - far more than the small per-entry buffers - so decode into
 * this one shared area and copy the finished thumbnail into the entry's buf.
 * Sized generously; clip_jpeg_fd() fails gracefully (rc <= 0) if it needs more
 * and we simply fall back to external art / no art. */
#define AA_DECODE_BUFSZ (160 * 1024)
#endif

enum thumb_state
{
    TH_EMPTY = 0,   /* slot unused */
    TH_LOADED,      /* bm holds a valid scaled thumbnail */
    TH_NONE,        /* searched, but this track has no usable cover art */
};

struct thumb_entry
{
    char track[MAX_PATH];   /* key: the track's file path */
    enum thumb_state state;
    long lru;               /* tick of last access, for eviction */
    struct bitmap bm;       /* points at buf */
    unsigned char buf[AA_THUMB_BUFSZ];
};

static struct thumb_entry cache[AA_THUMB_COUNT];
static int cached_size;          /* thumbnail box size currently in the cache */
static struct mp3entry probe;    /* transient; the browser is single-threaded */
#ifdef HAVE_JPEG
static unsigned char aa_decode_buf[AA_DECODE_BUFSZ]; /* shared JPEG decode scratch */
#endif

void aa_thumb_clear(void)
{
    for (int i = 0; i < AA_THUMB_COUNT; i++)
    {
        cache[i].track[0] = '\0';
        cache[i].state = TH_EMPTY;
        cache[i].lru = 0;
    }
    cached_size = 0;
}

static struct thumb_entry *find_entry(const char *trackpath)
{
    for (int i = 0; i < AA_THUMB_COUNT; i++)
    {
        if (cache[i].state != TH_EMPTY && !strcmp(cache[i].track, trackpath))
            return &cache[i];
    }
    return NULL;
}

/* Pick a free slot, or the least-recently-used one to recycle. */
static struct thumb_entry *alloc_entry(void)
{
    struct thumb_entry *victim = &cache[0];
    for (int i = 0; i < AA_THUMB_COUNT; i++)
    {
        if (cache[i].state == TH_EMPTY)
            return &cache[i];
        if (cache[i].lru < victim->lru)
            victim = &cache[i];
    }
    return victim;
}

struct bitmap *aa_thumb_get(const char *trackpath, int size)
{
    char coverpath[MAX_PATH];
    struct dim dim;
    struct thumb_entry *e;

    if (!trackpath || !trackpath[0])
        return NULL;

    if (size > AA_THUMB_MAX)
        size = AA_THUMB_MAX;
    if (size <= 0)
        return NULL;

    /* A change in requested size makes every cached bitmap the wrong size. */
    if (size != cached_size)
    {
        aa_thumb_clear();
        cached_size = size;
    }

    e = find_entry(trackpath);
    if (e)
    {
        e->lru = current_tick;
        return (e->state == TH_LOADED) ? &e->bm : NULL;
    }

    /* Not cached: claim a slot and resolve + load the cover once. */
    e = alloc_entry();
    strmemccpy(e->track, trackpath, sizeof(e->track));
    e->lru = current_tick;
    e->state = TH_NONE;   /* assume no art until proven otherwise */

    /* Read this track's tags: tells us whether it carries embedded art, and
     * fills the fields find_albumart() needs to match named external covers. */
    memset(&probe, 0, sizeof(probe));
    if (!get_metadata(&probe, -1, e->track))
        return NULL;

#ifdef HAVE_JPEG
    /* 1) Embedded album art - the common case: a JPEG stored in the file's
     *    tags. Decode it from the file at its recorded offset into the shared
     *    scratch, then keep the finished thumbnail in the entry's own buffer. */
    if (probe.has_embedded_albumart && probe.albumart.size > 0)
    {
        int fd = open(e->track, O_RDONLY);
        if (fd >= 0)
        {
            struct bitmap tmp;
            int rc;
            memset(&tmp, 0, sizeof(tmp));
            tmp.data = aa_decode_buf;
            tmp.width = size;       /* box; decoder scales to fit, keeps aspect */
            tmp.height = size;
            lseek(fd, probe.albumart.pos, SEEK_SET);
            rc = clip_jpeg_fd(fd, probe.albumart.type, probe.albumart.size,
                              &tmp, (int)sizeof(aa_decode_buf),
                              FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT
                              | FORMAT_DITHER, NULL);
            close(fd);
            if (rc > 0 && (size_t)rc <= sizeof(e->buf))
            {
                memcpy(e->buf, aa_decode_buf, rc);
                e->bm = tmp;
                e->bm.data = e->buf;
                /* clip_jpeg_fd() doesn't set bm->format, so it stays 0
                 * (FORMAT_MONO) from the memset above. lcd_bmp_part() would then
                 * draw this native RGB565 thumbnail as a 1-bpp mono bitmap, i.e.
                 * garbled noise. The external read_bmp_file() path sets the
                 * format itself; the embedded path must do the same. */
                e->bm.format = FORMAT_NATIVE;
                e->state = TH_LOADED;
                return &e->bm;
            }
        }
    }
#endif /* HAVE_JPEG */

    /* 2) Fall back to an external cover file (cover.bmp / folder.bmp / named). */
    dim.width = size;
    dim.height = size;
    if (find_albumart(&probe, coverpath, sizeof(coverpath), &dim))
    {
        memset(&e->bm, 0, sizeof(e->bm));
        e->bm.data = e->buf;
        e->bm.width = size;     /* target box; loader scales to fit, keeps aspect */
        e->bm.height = size;
        if (read_bmp_file(coverpath, &e->bm, sizeof(e->buf),
                          FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT
                          | FORMAT_DITHER, NULL) > 0)
        {
            e->state = TH_LOADED;
            return &e->bm;
        }
    }

    return NULL;
}

struct bitmap *aa_thumb_placeholder(int size)
{
    /* A neutral filled square (with a slightly darker 1px border) drawn in the
     * thumbnail gutter for tracks with no resolvable cover art. Generated once
     * per requested size into its own static buffer — independent of the LRU
     * cache above, so a missing-art row never evicts a real thumbnail. */
    static fb_data ph_buf[AA_THUMB_MAX * AA_THUMB_MAX];
    static struct bitmap ph_bm;
    static int ph_size = 0;

    if (size > AA_THUMB_MAX)
        size = AA_THUMB_MAX;
    if (size <= 0)
        return NULL;

    if (size != ph_size)
    {
        fb_data fill   = LCD_RGBPACK(0x40, 0x40, 0x40);
        fb_data border = LCD_RGBPACK(0x20, 0x20, 0x20);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                ph_buf[y * size + x] =
                    (x == 0 || y == 0 || x == size - 1 || y == size - 1)
                        ? border : fill;
        ph_bm.width  = size;
        ph_bm.height = size;
        ph_bm.format = FORMAT_NATIVE;
        ph_bm.data   = (unsigned char *)ph_buf;
        ph_size = size;
    }
    return &ph_bm;
}

#endif /* HAVE_DB_ALBUMART */
