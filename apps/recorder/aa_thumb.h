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
#ifndef _AA_THUMB_H_
#define _AA_THUMB_H_

#include "config.h"

/* Per-row album-art thumbnails in the database browser need a colour LCD,
 * external album-art support, and the on-load bitmap scaler. */
#if defined(HAVE_ALBUMART) && defined(HAVE_BMP_SCALING) && (LCD_DEPTH > 1)
#define HAVE_DB_ALBUMART

#include "lcd.h"   /* struct bitmap */

/* Largest thumbnail edge (square bounding box) we will scale to / cache. */
#define AA_THUMB_MAX 64

/* Return a cached thumbnail for the cover art of the track at trackpath,
 * scaled to fit a size x size box with aspect ratio preserved, or NULL if the
 * track has no cover art (or it could not be loaded).
 *
 * The first call for a given track resolves the cover file, decodes and scales
 * it, and stores it in a small LRU cache; subsequent calls (including the many
 * repeated redraws while browsing) are served from the cache without touching
 * the disk. size is clamped to AA_THUMB_MAX; changing size invalidates the
 * cache. Intended to be called from the list draw path. */
struct bitmap *aa_thumb_get(const char *trackpath, int size);

/* Drop every cached thumbnail. Call when entering/leaving the database browser
 * so stale entries from a previous view do not linger. */
void aa_thumb_clear(void);

#endif /* HAVE_ALBUMART && HAVE_BMP_SCALING && LCD_DEPTH > 1 */
#endif /* _AA_THUMB_H_ */
