/*
 * This file Copyright (C) 2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#ifndef __TRANSMISSION__
 #error only libtransmission should #include this header.
#endif

#ifndef TR_CACHE_H
#define TR_CACHE_H

typedef struct tr_cache tr_cache;

/***
****
***/

tr_cache * tr_cacheNew( double max_MiB );

void tr_cacheFree( tr_cache * );

/***
****
***/

int tr_cacheSetLimit( tr_cache * cache, double max_MiB );

double tr_cacheGetLimit( const tr_cache * );

int tr_cacheWriteBlock( tr_cache         * cache,
                        tr_torrent       * torrent,
                        tr_piece_index_t   piece,
                        uint32_t           offset,
                        uint32_t           len,
                        const uint8_t    * writeme );

int tr_cacheReadBlock( tr_cache         * cache,
                       tr_torrent       * torrent,
                       tr_piece_index_t   piece,
                       uint32_t           offset,
                       uint32_t           len,
                       uint8_t          * setme );

int tr_cachePrefetchBlock( tr_cache         * cache,
                           tr_torrent       * torrent,
                           tr_piece_index_t   piece,
                           uint32_t           offset,
                           uint32_t           len );

/***
****
***/

int tr_cacheFlushTorrent( tr_cache    * cache,
                          tr_torrent  * torrent );

int tr_cacheFlushFile( tr_cache         * cache,
                       tr_torrent       * torrent,
                       tr_file_index_t    file );

#endif
