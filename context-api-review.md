# Code Context

## Files Retrieved
1. `source/api.c` - Contains the implementation of the API functions for the Navidrome 3DS application.

## Key Code

### Internal State and Constants
```c
static NaviConfig g_cfg;
static char g_base_url[512];
#define API_VERSION "1.16.1"
#define API_CLIENT  "Navidrome3DS"
```

### HTTP Response Buffer
```c
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buffer;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    // Implementation
}

static Buffer buf_new(void) {
    // Implementation
}

static void buf_free(Buffer *b) {
    // Implementation
}
```

### HTTP GET Request
```c
static int http_get(const char *url, Buffer *out) {
    // Implementation
}
```

### URL Building
```c
static void build_url(char *out, size_t len, const char *endpoint, const char *extra_params) {
    // Implementation
}
```

### XML Parsing
```c
static const char *xml_attr(const char *xml, const char *attr, char *dst, size_t dst_len) {
    // Implementation
}

static const char *xml_next_tag(const char *xml, const char *tag) {
    // Implementation
}
```

### Album Cover Fetching
```c
int api_get_album_cover(const char *album_id, C2D_Image *out) {
    // Implementation
}
```

### Public API Functions
```c
void api_init(const NaviConfig *cfg) {
    // Implementation
}

void api_cleanup(void) {
    // Implementation
}

int api_ping(void) {
    // Implementation
}

int api_get_artists(NaviArtistList *out) {
    // Implementation
}

int api_get_albums(const char *artist_id, NaviAlbumList *out) {
    // Implementation
}

int api_get_tracks(const char *album_id, NaviTrackList *out) {
    // Implementation
}

void api_stream_url(const char *track_id, char *buf, size_t len) {
    // Implementation
}
```

## Architecture

The `source/api.c` file contains the implementation of the API functions for the Navidrome 3DS application. The key components include:

1. **Internal State and Constants**: Variables and constants to store configuration and base URL.
2. **HTTP Response Buffer**: Functions to handle HTTP response data.
3. **HTTP GET Request**: Function to perform HTTP GET requests.
4. **URL Building**: Function to build Subsonic API URLs.
5. **XML Parsing**: Functions to parse XML attributes and tags.
6. **Album Cover Fetching**: Function to fetch and load album cover images.
7. **Public API Functions**: Functions to initialize, clean up, ping, and fetch data from the Subsonic server.

## Start Here

Start with `source/api.c` to understand the API functions and the album cover fetching logic. This file contains the core functionality for interacting with the Subsonic API.
