#pragma once
#include "config.h"
#include <stddef.h>

#define MAX_ITEMS    200
#define MAX_NAME_LEN 64
#define MAX_ID_LEN   32

// ---- Data structures ----

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    char artist[MAX_NAME_LEN];
} NaviArtist;

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    char artist[MAX_NAME_LEN];
    int  year;
    int  songCount;
} NaviAlbum;

typedef struct {
    char id[MAX_ID_LEN];
    char title[MAX_NAME_LEN];
    char artist[MAX_NAME_LEN];
    char album[MAX_NAME_LEN];
    int  duration;   // seconds
    int  track;
} NaviTrack;

typedef struct {
    NaviArtist items[MAX_ITEMS];
    int count;
} NaviArtistList;

typedef struct {
    NaviAlbum items[MAX_ITEMS];
    int count;
} NaviAlbumList;

typedef struct {
    NaviTrack items[MAX_ITEMS];
    int count;
} NaviTrackList;

// ---- API functions ----

// Must be called once at startup
void api_init(const NaviConfig *cfg);
void api_cleanup(void);

// Ping the server — returns 0 on success
int api_ping(void);

// Fetch all artists (alphabetically)
int api_get_artists(NaviArtistList *out);

// Fetch albums for a given artist ID
int api_get_albums(const char *artist_id, NaviAlbumList *out);

// Fetch tracks for a given album ID
int api_get_tracks(const char *album_id, NaviTrackList *out);

// Build a stream URL for a track (written into buf, len bytes)
void api_stream_url(const char *track_id, char *buf, size_t len);
