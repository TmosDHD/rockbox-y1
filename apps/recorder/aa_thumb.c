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

/* How many distinct thumbnails to keep decoded at once. A few screens worth is
 * plenty: only the visible rows are ever requested per draw, so this just needs
 * to cover the working set while scrolling. */
#define AA_THUMB_COUNT 16

/* Bytes needed to hold one AA_THUMB_MAX-square native bitmap plus the scaler's
 * working overhead (see BM_SCALED_SIZE in bmp.h). */
#define AA_THUMB_BUFSZ BM_SCALED_SIZE(AA_THUMB_MAX, AA_THUMB_MAX, FORMAT_NATIVE, false)

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

    memset(&probe, 0, sizeof(probe));
    strmemccpy(probe.path, e->track, sizeof(probe.path));

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

#endif /* HAVE_DB_ALBUMART */
