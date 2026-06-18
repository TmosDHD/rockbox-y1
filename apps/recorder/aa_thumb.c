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

/* TEMPORARY on-device decode dump (remove before the feature commit). */
#define AA_DEV_DUMP

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
                e->state = TH_LOADED;
#ifdef AA_DEV_DUMP
                {
                    static int dn = 0;
                    if (dn < 4)
                    {
                        char nm[80];
                        snprintf(nm, sizeof(nm), "/sdcard/.rockbox/aa_dev%d.raw", dn);
                        int dfd = open(nm, O_CREAT | O_WRONLY | O_TRUNC, 0666);
                        if (dfd >= 0)
                        {
                            int meta[3] = { e->bm.width, e->bm.height, rc };
                            write(dfd, meta, sizeof(meta));
                            write(dfd, e->buf, rc);
                            close(dfd);
                        }
                        int lfd = open("/sdcard/.rockbox/aa_dev.txt",
                                       O_CREAT | O_WRONLY | O_APPEND, 0666);
                        if (lfd >= 0)
                        {
                            fdprintf(lfd, "dump%d type=%d pos=%ld size=%ld rc=%d %dx%d\n",
                                     dn, (int)probe.albumart.type,
                                     (long)probe.albumart.pos, (long)probe.albumart.size,
                                     rc, e->bm.width, e->bm.height);
                            close(lfd);
                        }
                        dn++;
                    }
                }
#endif
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

#if defined(SIMULATOR)
/* ===== TEMPORARY decode self-test (SIMULATOR only; excluded from device APK) =====
 * Reproduces the on-device "garbled noise" embedded-cover bug off-line: decodes a
 * known track's cover three ways and dumps each as a BMP plus a diagnostic log,
 * so we can see exactly where the corruption enters. Remove before shipping. */

static void aa_dump_bmp565(const char *path, struct bitmap *bm)
{
    int w = bm->width, h = bm->height;
    if (w <= 0 || h <= 0 || w > AA_THUMB_MAX)
        return;
    int rowsize = (w * 3 + 3) & ~3;
    long datasize = (long)rowsize * h;
    long filesize = 54 + datasize;
    unsigned char hdr[54];
    static unsigned char row[AA_THUMB_MAX * 3 + 4];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = filesize; hdr[3] = filesize >> 8; hdr[4] = filesize >> 16; hdr[5] = filesize >> 24;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w; hdr[19] = w >> 8; hdr[20] = w >> 16; hdr[21] = w >> 24;
    hdr[22] = h; hdr[23] = h >> 8; hdr[24] = h >> 16; hdr[25] = h >> 24;
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = datasize; hdr[35] = datasize >> 8; hdr[36] = datasize >> 16; hdr[37] = datasize >> 24;
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, hdr, 54);
    /* fb_data is 16-bit RGB565 on this target; BMP rows are bottom-up, BGR. */
    unsigned short *px = (unsigned short *)bm->data;
    for (int y = h - 1; y >= 0; y--)
    {
        unsigned char *p = row;
        unsigned short *src = px + (long)y * w;
        for (int x = 0; x < w; x++)
        {
            unsigned short v = src[x];
            int r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
            *p++ = (b5 << 3) | (b5 >> 2);
            *p++ = (g6 << 2) | (g6 >> 4);
            *p++ = (r5 << 3) | (r5 >> 2);
        }
        while ((p - row) < rowsize)
            *p++ = 0;
        write(fd, row, rowsize);
    }
    close(fd);
}

void aa_thumb_selftest(void)
{
    const char *path = "/aa_test.mp3";
    const int size = AA_THUMB_MAX;
    int lg = open("/aa_selftest.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (lg < 0)
        return;

    static struct mp3entry mp;
    memset(&mp, 0, sizeof(mp));
    bool gm = get_metadata(&mp, -1, path);
    fdprintf(lg, "get_metadata=%d has_embedded=%d aa.type=%d unsync=%d vorbis=%d pos=%ld size=%ld\n",
             (int)gm, (int)mp.has_embedded_albumart, (int)mp.albumart.type,
             (int)((mp.albumart.type & AA_FLAG_ID3_UNSYNC) != 0),
             (int)((mp.albumart.type & AA_FLAG_VORBIS_BASE64) != 0),
             (long)mp.albumart.pos, (long)mp.albumart.size);

#ifdef HAVE_JPEG
    if (gm && mp.has_embedded_albumart && mp.albumart.size > 0)
    {
        /* D1: embedded decode at several target sizes (the device uses
         * 2*font_height, not necessarily 64) to catch a size-dependent bug. */
        static const int sizes[] = {16, 24, 32, 40, 48, 56, 64};
        for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++)
        {
            int sz = sizes[si];
            struct bitmap bm;
            memset(&bm, 0, sizeof(bm));
            bm.data = aa_decode_buf;
            bm.width = sz;
            bm.height = sz;
            int fd = open(path, O_RDONLY);
            if (fd >= 0)
            {
                lseek(fd, mp.albumart.pos, SEEK_SET);
                int rc = clip_jpeg_fd(fd, mp.albumart.type, mp.albumart.size, &bm,
                                      (int)sizeof(aa_decode_buf),
                                      FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT | FORMAT_DITHER, NULL);
                close(fd);
                char nm[64];
                snprintf(nm, sizeof(nm), "/aa_d1_sz%d.bmp", sz);
                fdprintf(lg, "D1 sz=%d rc=%d out=%dx%d\n", sz, rc, bm.width, bm.height);
                if (rc > 0)
                    aa_dump_bmp565(nm, &bm);
            }
        }
        /* and once more at the default 64 box for the D2/D3 comparison */
        struct bitmap bm;
        memset(&bm, 0, sizeof(bm));
        bm.data = aa_decode_buf;
        bm.width = size;
        bm.height = size;
        int fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            lseek(fd, mp.albumart.pos, SEEK_SET);
            int rc = clip_jpeg_fd(fd, mp.albumart.type, mp.albumart.size, &bm,
                                  (int)sizeof(aa_decode_buf),
                                  FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT | FORMAT_DITHER, NULL);
            close(fd);
            fdprintf(lg, "D1 embedded clip_jpeg_fd rc=%d out=%dx%d expect_bytes=%d e_buf=%d\n",
                     rc, bm.width, bm.height, bm.width * bm.height * 2, (int)AA_THUMB_BUFSZ);
            if (rc > 0)
                aa_dump_bmp565("/aa_d1_embedded.bmp", &bm);
        }

        /* D2: copy the embedded JPEG out to a file, decode via the external path. */
        int infd = open(path, O_RDONLY);
        int jf = open("/aa_cover.jpg", O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (infd >= 0 && jf >= 0)
        {
            lseek(infd, mp.albumart.pos, SEEK_SET);
            static unsigned char cp[65536];
            long left = mp.albumart.size;
            while (left > 0)
            {
                int n = read(infd, cp, (left < (long)sizeof(cp)) ? (int)left : (int)sizeof(cp));
                if (n <= 0)
                    break;
                write(jf, cp, n);
                left -= n;
            }
        }
        if (infd >= 0) close(infd);
        if (jf >= 0) close(jf);

        memset(&bm, 0, sizeof(bm));
        bm.data = aa_decode_buf;
        bm.width = size;
        bm.height = size;
        int fd2 = open("/aa_cover.jpg", O_RDONLY);
        if (fd2 >= 0)
        {
            int rc = read_jpeg_fd(fd2, &bm, (int)sizeof(aa_decode_buf),
                                  FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT | FORMAT_DITHER, NULL);
            close(fd2);
            fdprintf(lg, "D2 external read_jpeg_fd rc=%d out=%dx%d\n", rc, bm.width, bm.height);
            if (rc > 0)
                aa_dump_bmp565("/aa_d2_external.bmp", &bm);
        }
    }
#endif /* HAVE_JPEG */

    /* D3: the full aa_thumb_get() path (cache + memcpy into the entry buffer). */
    struct bitmap *t = aa_thumb_get(path, size);
    if (t)
    {
        fdprintf(lg, "D3 aa_thumb_get OK %dx%d\n", t->width, t->height);
        aa_dump_bmp565("/aa_d3_full.bmp", t);
    }
    else
        fdprintf(lg, "D3 aa_thumb_get NULL\n");

    close(lg);
}
#endif /* SIMULATOR */

#endif /* HAVE_DB_ALBUMART */
