Based on the code analysis, here's the context information for implementing album cover support on the top screen during playback:

# Code Context

## Files Retrieved
1. `source/ui.c` - Contains the UI rendering and input handling logic
2. `source/api.c` - Contains API functions for fetching data from the server
3. `source/api.h` - Contains API function declarations and data structures

## Key Code

### UI Rendering
The main UI rendering happens in `ui_draw()` function in `source/ui.c` (lines 312-532). The top screen is rendered by `draw_now_playing()` function (lines 280-310).

### Image Handling
The codebase doesn't currently have any image handling functionality. We'll need to add support for downloading and displaying album art images.

### API Functions
The API functions in `source/api.c` handle fetching data from the server. We'll need to add a new function to fetch album art.

## Architecture

The UI system uses citro2d for rendering. The top screen currently shows track information during playback. The bottom screen shows navigable lists of artists, albums, and tracks.

The API system uses libcurl to make HTTP requests to the Navidrome server's Subsonic API.

## Start Here

1. First, we need to add image handling capabilities to the UI system. This will involve:
   - Adding functions to download and decode images
   - Adding functions to render images using citro2d

2. Then, we need to extend the API to fetch album art:
   - Add a new function to `source/api.c` to fetch album art using the Subsonic API's `getCoverArt` endpoint
   - Update `source/api.h` with the new function declaration

3. Finally, modify the UI to display album art:
   - Update `draw_now_playing()` in `source/ui.c` to display the album art on the top screen
   - Add logic to download and cache album art when tracks are loaded

## Supervisor Coordination

No supervisor coordination is needed at this stage. The analysis is complete and the next step would be to create a plan for implementation.