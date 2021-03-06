/*
 * This file Copyright (C) 2010 Mnemosyne LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * $Id$
 */

#include <stdio.h>
#include <time.h>

#define CURL_DISABLE_TYPECHECK /* otherwise -Wunreachable-code goes insane */
#include <curl/curl.h> 

#include <event.h> /* struct evbuffer */

#include <libtransmission/transmission.h>
#include <libtransmission/bencode.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/web.h> /* tr_webGetResponseStr */
#include <libtransmission/version.h>

#define MY_NAME "transmission-show"
#define TIMEOUT_SECS 30

static tr_option options[] =
{
  { 's', "scrape", "Ask the torrent's trackers how many peers are in the torrent's swarm", "s", 0, NULL },
  { 0, NULL, NULL, NULL, 0, NULL }
};

static const char *
getUsage( void )
{
    return "Usage: " MY_NAME " [options] <.torrent file>";
}

static tr_bool scrapeFlag = FALSE;
const char * filename = NULL;

static int
parseCommandLine( int argc, const char ** argv )
{
    int c;
    const char * optarg;

    while(( c = tr_getopt( getUsage( ), argc, argv, options, &optarg )))
    {
        switch( c )
        {
            case 's': scrapeFlag = TRUE; break;
            case TR_OPT_UNK: filename = optarg; break;
            default: return 1;
        }
    }

    return 0;
}

static void
showInfo( const tr_info * inf )
{
    int i;
    char buf[128];
    int prevTier = -1;

    /**
    ***  General Info
    **/

    printf( "GENERAL\n\n" );
    printf( "  Name: %s\n", inf->name );
    printf( "  Hash: %s\n", inf->hashString );
    printf( "  Created by: %s\n", inf->creator ? inf->creator : "Unknown" );
    if( !inf->dateCreated )
        printf( "  Created on: Unknown\n" );
    else {
        struct tm tm = *localtime( &inf->dateCreated );
        printf( "  Created on: %s", asctime( &tm ) );
    }
    if( inf->comment && *inf->comment )
        printf( "  Comment: %s\n", inf->comment );
    printf( "  Piece Count: %d\n", inf->pieceCount );
    printf( "  Piece Size: %s\n", tr_formatter_size( buf, inf->pieceSize, sizeof( buf ) ) );
    printf( "  Total Size: %s\n", tr_formatter_size( buf, inf->totalSize, sizeof( buf ) ) );
    printf( "  Privacy: %s\n", inf->isPrivate ? "Private torrent" : "Public torrent" );

    /**
    ***  Trackers
    **/

    printf( "\nTRACKERS\n" );
    for( i=0; i<(int)inf->trackerCount; ++i ) 
    {
        if( prevTier != inf->trackers[i].tier )
        {
            prevTier = inf->trackers[i].tier;
            printf( "\n  Tier #%d\n", prevTier + 1 );
        }

        printf( "  %s\n", inf->trackers[i].announce );
    }

    /**
    ***  Files
    **/

    printf( "\nFILES\n\n" );
    for( i=0; i<(int)inf->fileCount; ++i )
        printf( "  %s (%s)\n", inf->files[i].name, tr_formatter_size( buf, inf->files[i].length, sizeof( buf ) ) );
}

static size_t
writeFunc( void * ptr, size_t size, size_t nmemb, void * buf )
{
    const size_t byteCount = size * nmemb;
    evbuffer_add( buf, ptr, byteCount );
    return byteCount;
}

static CURL*
tr_curl_easy_init( struct evbuffer * writebuf )
{
    CURL * curl = curl_easy_init( );
    curl_easy_setopt( curl, CURLOPT_USERAGENT, MY_NAME "/" LONG_VERSION_STRING );
    curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, writeFunc );
    curl_easy_setopt( curl, CURLOPT_WRITEDATA, writebuf );
    curl_easy_setopt( curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY );
    curl_easy_setopt( curl, CURLOPT_VERBOSE, getenv( "TR_CURL_VERBOSE" ) != NULL );
    curl_easy_setopt( curl, CURLOPT_ENCODING, "" );
    return curl;
}

static void
doScrape( const tr_info * inf )
{
    int i;

    for( i=0; i<inf->trackerCount; ++i )
    {
        CURL * curl;
        CURLcode res;
        struct evbuffer * buf;
        const char * scrape = inf->trackers[i].scrape;
        char * url;

        if( scrape == NULL )
            continue;

        url = tr_strdup_printf( "%s%cinfo_hash=%s",
                                scrape,
                                strchr( scrape, '?' ) ? '&' : '?',
                                inf->hashEscaped );

        printf( "%s ... ", url );
        fflush( stdout );

        buf = evbuffer_new( );
        curl = tr_curl_easy_init( buf );
        curl_easy_setopt( curl, CURLOPT_URL, url );
        curl_easy_setopt( curl, CURLOPT_TIMEOUT, TIMEOUT_SECS );

        if(( res = curl_easy_perform( curl )))
        {
            printf( "error: %s\n", curl_easy_strerror( res ) );
        }
        else
        {
            long response;
            curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, &response );
            if( response != 200 )
            {
                printf( "error: unexpected response %ld \"%s\"\n",
                        response,
                        tr_webGetResponseStr( response ) );
            }
            else /* HTTP OK */
            {
                tr_benc top;
                tr_benc * files;
                tr_bool matched = FALSE;
                const char * begin = (const char*) EVBUFFER_DATA( buf );
                const char * end = begin + EVBUFFER_LENGTH( buf );

                if( !tr_bencParse( begin, end, &top, NULL ) )
                {
                    if( tr_bencDictFindDict( &top, "files", &files ) )
                    {
                        int i = 0;
                        tr_benc * val;
                        const char * key;

                        while( tr_bencDictChild( files, i++, &key, &val ))
                        {
                            if( !memcmp( inf->hash, key, SHA_DIGEST_LENGTH ) )
                            {
                                int64_t seeders = -1;
                                int64_t leechers = -1;
                                tr_bencDictFindInt( val, "complete", &seeders );
                                tr_bencDictFindInt( val, "incomplete", &leechers );
                                printf( "%d seeders, %d leechers\n", (int)seeders, (int)leechers );
                                matched = TRUE;
                            }
                        }
                    }

                    tr_bencFree( &top );
                }

                if( !matched )
                    printf( "no match\n" );
            }
        }

        curl_easy_cleanup( curl );
        evbuffer_free( buf );
        tr_free( url );
    }
}

int
main( int argc, char * argv[] )
{
    int err;
    tr_info inf;
    tr_ctor * ctor;

    tr_setMessageLevel( TR_MSG_ERR );
    tr_formatter_size_init ( 1024, "B", "KiB", "MiB", "GiB" );
    tr_formatter_speed_init( 1024, "B/s", "KiB/s", "MiB/s", "GiB/s" );

    if( parseCommandLine( argc, (const char**)argv ) )
        return EXIT_FAILURE;

    /* make sure the user specified a filename */
    if( !filename )
    {
        fprintf( stderr, "ERROR: No .torrent file specified.\n" );
        tr_getopt_usage( MY_NAME, getUsage( ), options );
        fprintf( stderr, "\n" );
        return EXIT_FAILURE;
    }

    /* try to parse the .torrent file */
    ctor = tr_ctorNew( NULL );
    tr_ctorSetMetainfoFromFile( ctor, filename );
    err = tr_torrentParse( ctor, &inf );
    tr_ctorFree( ctor );
    if( err )
    {
        fprintf( stderr, "Error parsing .torrent file \"%s\"\n", filename );
        return 1;
    }

    printf( "Name: %s\n", inf.name );
    printf( "File: %s\n", filename );
    printf( "\n" );
    fflush( stdout );

    if( scrapeFlag )
        doScrape( &inf );
    else
        showInfo( &inf );

    /* cleanup */
    putc( '\n', stdout );
    tr_metainfoFree( &inf );
    return 0;
}
