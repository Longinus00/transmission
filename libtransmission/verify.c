/*
 * This file Copyright (C) 2007-2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#include <unistd.h> /* S_ISREG */
#include <sys/stat.h>

#ifdef HAVE_POSIX_FADVISE
 #define _XOPEN_SOURCE 600
#endif
#if defined(HAVE_POSIX_FADVISE) || defined(SYS_DARWIN)
 #include <fcntl.h> /* posix_fadvise() / fcntl() */
#endif

#include <openssl/sha.h>

#include "transmission.h"
#include "completion.h"
#include "fdlimit.h"
#include "inout.h"
#include "list.h"
#include "platform.h"
#include "resume.h" /* tr_torrentLoadProgress */
#include "torrent.h"
#include "utils.h" /* tr_buildPath */
#include "verify.h"


/***
****
***/

enum
{
    MSEC_TO_SLEEP_PER_SECOND_DURING_VERIFY = 200
};

/* #define STOPWATCH */

static tr_bool
verifyTorrent( tr_torrent * tor, tr_bool * stopFlag )
{
    SHA_CTX sha;
    int fd = -1;
    int64_t filePos = 0;
    tr_bool changed = 0;
    tr_bool hadPiece = 0;
    time_t lastSleptAt = 0;
    uint32_t piecePos = 0;
    uint32_t pieceBytesRead = 0;
    tr_file_index_t fileIndex = 0;
    tr_file_index_t prevFileIndex = !fileIndex;
    tr_piece_index_t pieceIndex = 0;
    const time_t begin = tr_time( );
    time_t end;
    const int64_t buflen = 16384;
    uint8_t * buffer = tr_valloc( buflen );

    SHA1_Init( &sha );

    while( !*stopFlag && ( pieceIndex < tor->info.pieceCount ) )
    {
        int64_t leftInPiece;
        int64_t leftInFile;
        int64_t bytesThisPass;
        const tr_file * file = &tor->info.files[fileIndex];

        /* if we're starting a new piece... */
        if( piecePos == 0 )
        {
            hadPiece = tr_cpPieceIsComplete( &tor->completion, pieceIndex );
            /* fprintf( stderr, "starting piece %d of %d\n", (int)pieceIndex, (int)tor->info.pieceCount ); */
        }

        /* if we're starting a new file... */
        if( !filePos && (fd<0) && (fileIndex!=prevFileIndex) )
        {
            char * filename = tr_torrentFindFile( tor, fileIndex );
            fd = filename == NULL ? -1 : tr_open_file_for_scanning( filename );
            /* fprintf( stderr, "opening file #%d (%s) -- %d\n", fileIndex, filename, fd ); */
            tr_free( filename );
            prevFileIndex = fileIndex;
        }

        /* figure out how much we can read this pass */
        leftInPiece = tr_torPieceCountBytes( tor, pieceIndex ) - piecePos;
        leftInFile = file->length - filePos;
        bytesThisPass = MIN( leftInFile, leftInPiece );
        bytesThisPass = MIN( bytesThisPass, buflen );
        /* fprintf( stderr, "reading this pass: %d\n", (int)bytesThisPass ); */

        /* read a bit */
        if( fd >= 0 ) {
            const ssize_t numRead = tr_pread( fd, buffer, bytesThisPass, filePos );
            if( numRead == bytesThisPass )
                SHA1_Update( &sha, buffer, numRead );
            if( numRead > 0 ) {
                pieceBytesRead += numRead;
#if defined HAVE_POSIX_FADVISE && defined POSIX_FADV_DONTNEED
                posix_fadvise( fd, filePos, bytesThisPass, POSIX_FADV_DONTNEED );
#endif
            }
        }

        /* move our offsets */
        leftInPiece -= bytesThisPass;
        leftInFile -= bytesThisPass;
        piecePos += bytesThisPass;
        filePos += bytesThisPass;

        /* if we're finishing a piece... */
        if( leftInPiece == 0 )
        {
            time_t now;
            tr_bool hasPiece;
            uint8_t hash[SHA_DIGEST_LENGTH];

            SHA1_Final( hash, &sha );
            hasPiece = !memcmp( hash, tor->info.pieces[pieceIndex].hash, SHA_DIGEST_LENGTH );
            /* fprintf( stderr, "do the hashes match? %s\n", (hasPiece?"yes":"no") ); */

            if( hasPiece ) {
                tr_torrentSetHasPiece( tor, pieceIndex, TRUE );
                if( !hadPiece )
                    changed = TRUE;
            } else if( hadPiece ) {
                tr_torrentSetHasPiece( tor, pieceIndex, FALSE );
                changed = TRUE;
            }
            tr_torrentSetPieceChecked( tor, pieceIndex, TRUE );
            now = tr_time( );
            tor->anyDate = now;

            /* sleeping even just a few msec per second goes a long
             * way towards reducing IO load... */
            if( lastSleptAt != now ) {
                lastSleptAt = now;
                tr_wait_msec( MSEC_TO_SLEEP_PER_SECOND_DURING_VERIFY );
            }

            SHA1_Init( &sha );
            ++pieceIndex;
            piecePos = 0;
            pieceBytesRead = 0;
        }

        /* if we're finishing a file... */
        if( leftInFile == 0 )
        {
            /* fprintf( stderr, "closing file\n" ); */
            if( fd >= 0 ) { tr_close_file( fd ); fd = -1; }
            ++fileIndex;
            filePos = 0;
        }
    }

    /* cleanup */
    if( fd >= 0 )
        tr_close_file( fd );
    free( buffer );

    /* stopwatch */
    end = tr_time( );
    tr_tordbg( tor, "it took %d seconds to verify %"PRIu64" bytes (%"PRIu64" bytes per second)",
               (int)(end-begin), tor->info.totalSize, (uint64_t)(tor->info.totalSize/(1+(end-begin))) );

    return changed;
}

/***
****
***/

struct verify_node
{
    tr_torrent *         torrent;
    tr_verify_done_cb    verify_done_cb;
};

static void
fireCheckDone( tr_torrent * tor, tr_verify_done_cb verify_done_cb )
{
    assert( tr_isTorrent( tor ) );

    if( verify_done_cb )
        verify_done_cb( tor );
}

static struct verify_node currentNode;
static tr_list * verifyList = NULL;
static tr_thread * verifyThread = NULL;
static tr_bool stopCurrent = FALSE;

static tr_lock*
getVerifyLock( void )
{
    static tr_lock * lock = NULL;

    if( lock == NULL )
        lock = tr_lockNew( );
    return lock;
}

static void
verifyThreadFunc( void * unused UNUSED )
{
    for( ;; )
    {
        int                  changed = 0;
        tr_torrent         * tor;
        struct verify_node * node;

        tr_lockLock( getVerifyLock( ) );
        stopCurrent = FALSE;
        node = (struct verify_node*) verifyList ? verifyList->data : NULL;
        if( node == NULL )
        {
            currentNode.torrent = NULL;
            break;
        }

        currentNode = *node;
        tor = currentNode.torrent;
        tr_list_remove_data( &verifyList, node );
        tr_free( node );
        tr_lockUnlock( getVerifyLock( ) );

        tr_torinf( tor, "%s", _( "Verifying torrent" ) );
        tr_torrentSetVerifyState( tor, TR_VERIFY_NOW );
        changed = verifyTorrent( tor, &stopCurrent );
        tr_torrentSetVerifyState( tor, TR_VERIFY_NONE );
        assert( tr_isTorrent( tor ) );

        if( !stopCurrent )
        {
            if( changed )
                tr_torrentSetDirty( tor );
            if( tr_torrentCountUncheckedPieces( tor ) == 0 )
                tor->failedState = TR_FAILED_NONE;
            fireCheckDone( tor, currentNode.verify_done_cb );
        }
    }

    verifyThread = NULL;
    tr_lockUnlock( getVerifyLock( ) );
}

static tr_bool
torrentHasAnyLocalData( const tr_torrent * tor )
{
    tr_file_index_t i;
    tr_bool hasAny = FALSE;
    const tr_file_index_t n = tor->info.fileCount;

    assert( tr_isTorrent( tor ) );

    for( i=0; i<n && !hasAny; ++i )
    {
        struct stat sb;
        char * path = tr_torrentFindFile( tor, i );
        if( ( path != NULL ) && !stat( path, &sb ) && ( sb.st_size > 0 ) )
            hasAny = TRUE;
        tr_free( path );
    }

    return hasAny;
}

void
tr_verifyAdd( tr_torrent *      tor,
              tr_verify_done_cb verify_done_cb )
{
    assert( tr_isTorrent( tor ) );

    if( !torrentHasAnyLocalData( tor ) )
    {
        /* Check to see if any data has previously been downloaded. */
        const tr_bool hadAny = tr_cpHaveTotal( &tor->completion ) != 0;

        if( hadAny ){
            /* There used to be data and it is now gone */

            if( tor->failedState != TR_FAILED_FILE ){
                /* Complain about missing files  */
                tr_torerr( tor, "Can't find local data" );
                tor->failedState = TR_FAILED_FILE;
            }
            else {
                /* This is the second time the file has been missing so assume
                 * that the user wants to start from scratch */  
                tr_piece_index_t i;
                tr_torinf( tor, "Reseting progress" );
                for( i=0; i<tor->info.pieceCount; ++i ) {
                    tr_torrentSetHasPiece( tor, i, FALSE );
                    tr_torrentSetPieceChecked( tor, i, TRUE );
                }
                tor->failedState = TR_FAILED_NONE;
                tr_torrentSetDirty( tor );
            }
        }
        else
        {
            /* Never had any data to begin */
            tr_torrentUncheck( tor );
            tor->failedState = TR_FAILED_NONE;
        }

        fireCheckDone( tor, verify_done_cb );
    }
    else if( tor->failedState == TR_FAILED_FILE ){
        /* The previously missing files and are now back.
         * Reload the progress from the resume file. */
        tr_torinf( tor, "Reloading local data" );
        tor->failedState = TR_FAILED_NONE;

        if( tr_torrentLoadProgress( tor ) != TR_FR_PROGRESS )
        {
            /* LoadProgress failed so we should zero the progress to be safe */
            tr_piece_index_t i;
            tr_torerr( tor, "Reloading local data failed, resetting all progress" );
            for( i=0; i<tor->info.pieceCount; ++i ) {
                tr_torrentSetHasPiece( tor, i, FALSE );
                tr_torrentSetPieceChecked( tor, i, TRUE );
            }
        }

        tr_torrentSetDirty( tor );
        fireCheckDone( tor, verify_done_cb );
    }
    else if( tr_torrentCountUncheckedPieces( tor ) == 0
        || ( tor->failedState == TR_UNCHECKED_PIECES ) )
    {
        /* Torrent doesn't need to be checked, yet... */
        fireCheckDone( tor, verify_done_cb );
    }
    else
    {
        struct verify_node * node;

        tr_torinf( tor, "%s", _( "Queued for verification" ) );

        node = tr_new( struct verify_node, 1 );
        node->torrent = tor;
        node->verify_done_cb = verify_done_cb;

        tr_lockLock( getVerifyLock( ) );
        tr_torrentSetVerifyState( tor, TR_VERIFY_WAIT );
        tr_list_append( &verifyList, node );
        if( verifyThread == NULL )
            verifyThread = tr_threadNew( verifyThreadFunc, NULL );
        tr_lockUnlock( getVerifyLock( ) );
    }
}

static int
compareVerifyByTorrent( const void * va,
                        const void * vb )
{
    const struct verify_node * a = va;
    const tr_torrent *         b = vb;

    return a->torrent - b;
}

tr_bool
tr_verifyInProgressTorrent( const tr_torrent * tor )
{
    tr_bool found = FALSE;
    tr_lock * lock = getVerifyLock( );
    tr_lockLock( lock );

    assert( tr_isTorrent( tor ) );

    found = ( tor == currentNode.torrent )
         || ( tr_list_find( verifyList, tor, compareVerifyByTorrent ) != NULL );

    tr_lockUnlock( lock );
    return found;
}

tr_bool
tr_verifyInProgress( void )
{
    return verifyThread != NULL;
}

void
tr_verifyRemove( tr_torrent * tor )
{
    tr_lock * lock = getVerifyLock( );
    tr_lockLock( lock );

    assert( tr_isTorrent( tor ) );

    if( tor == currentNode.torrent )
    {
        stopCurrent = TRUE;
        while( stopCurrent )
        {
            tr_lockUnlock( lock );
            tr_wait_msec( 100 );
            tr_lockLock( lock );
        }
    }
    else
    {
        tr_free( tr_list_remove( &verifyList, tor, compareVerifyByTorrent ) );
        tr_torrentSetVerifyState( tor, TR_VERIFY_NONE );
    }

    tr_lockUnlock( lock );
}

void
tr_verifyClose( tr_session * session UNUSED )
{
    tr_lockLock( getVerifyLock( ) );

    stopCurrent = TRUE;
    tr_list_free( &verifyList, tr_free );

    tr_lockUnlock( getVerifyLock( ) );
}
