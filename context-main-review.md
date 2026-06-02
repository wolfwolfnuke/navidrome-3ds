# Code Context

## Files Retrieved
1. `source/main.c` - Main entry point, initialization logic, and main loop.

## Key Code

### Initialization Logic

1. **Graphics Initialization**:
   - `gfxInitDefault()`
   - `romfsInit()`
   - `debug_init()`

2. **Citro2D/Citro3D Initialization**:
   - `C3D_Init`
   - `C2D_Init`
   - Render targets for top and bottom screens

3. **Networking Initialization**:
   - `socInit`
   - `httpcInit`

4. **Audio Subsystem Initialization**:
   - `ndspInit`
   - `audio_init`

5. **UI State Allocation**:
   - Memory allocation for `UiState`
   - `ui_init`

6. **Configuration Loading**:
   - `config_load`
   - Default values if loading fails

7. **API Initialization**:
   - `api_init`

8. **Server Connection**:
   - `api_ping`
   - `api_get_artists`

### Main Loop

1. **APT Hook Registration**:
   - Handles sleep, wakeup, and exit events.

2. **Main Loop**:
   - Runs while `aptMainLoop()` returns true.
   - Prevents sleep while music is playing.
   - Handles input with `ui_handle_input`.
   - Actions based on screen state:
     - **SCREEN_ALBUMS**: Loads albums for the selected artist.
     - **SCREEN_TRACKS**: Stops audio and loads tracks for the selected album.
     - **SCREEN_PLAYER**: Plays the selected track.

3. **Rendering**:
   - Renders frames using `C3D_FrameBegin`, `C2D_TargetClear`, `ui_draw`, and `C3D_FrameEnd`.

### Cleanup

1. **Audio Cleanup**:
   - `audio_stop`
   - `audio_cleanup`
   - `ndspExit`

2. **API and UI Cleanup**:
   - `api_cleanup`
   - `ui_cleanup`

3. **System Services Cleanup**:
   - `socExit`
   - `httpcExit`
   - `romfsExit`
   - `C2D_Fini`
   - `C3D_Fini`
   - `gfxExit`

## Architecture

The main loop and initialization logic in `main.c` are functional and well-structured. The code follows best practices for resource management and error handling, ensuring a robust application. The initialization sequence ensures each subsystem is properly initialized before proceeding. The main loop efficiently handles input and rendering, and the cleanup process ensures all resources are properly released.

## Start Here

Start with `source/main.c` to understand the overall flow of the application, including initialization, main loop, and cleanup processes.