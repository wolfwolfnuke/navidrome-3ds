# Code Context

## Files Retrieved
1. `source/api.c` (lines 1-270) - API implementation for interacting with the Navidrome server.

## Key Code
The `api_get_album_cover` function is responsible for fetching album cover images:
```c
int api_get_album_cover(const char *album_id, C2D_Image *out) {
    char url[2048];
    snprintf(url, sizeof(url), "%s/rest/getCoverArt?u=%s&p=%s&v=1.16.1&c=Navidrome3DS&id=%s",
             g_base_url, g_cfg.username, g_cfg.password, album_id);

    debug_log("[API] Fetching album cover: %s", url);

    Buffer buf = buf_new();
    int http_code = http_get(url, &buf);
    if (http_code != 200 || !buf.data || buf.len == 0) {
        debug_log("[API] Failed to fetch album cover, HTTP code: %d", http_code);
        buf_free(&buf);
        return -1;
    }

    // Load the image data into a C2D_Image
    out->tex = C2D_SpriteSheetLoadImageMem(buf.data, buf.len);
    if (!out->tex) {
        debug_log("[API] Failed to load album cover image. Image format may not be supported.");
        buf_free(&buf);
        return -1;
    }

    // Set default dimensions if not available
    out->params.width = 100;
    out->params.height = 100;
    debug_log("[API] Successfully loaded album cover image");
    buf_free(&buf);
    return 0;
}
```

## Architecture
The `api_get_album_cover` function fetches the album cover image using a GET request to the Navidrome server. The image data is stored in a dynamically allocated buffer and loaded into a `C2D_Image` structure for rendering. The default dimensions for the image are set to 100x100 pixels if the actual dimensions are not available.

## Start Here
Start with `source/api.c` to understand how album cover images are fetched and managed. This file contains the implementation for interacting with the Navidrome server and handling image data.
