# Code Context

## Files Retrieved
1. `source/ui.c` - Contains the UI rendering logic, including the album cover image display.

## Key Code
The album cover image resolution is set to 100x100 pixels in the `draw_now_playing` function:
```c
// Draw album cover if available
if (state->album_cover.tex) {
    float coverX = TOP_W - 100 - 8; // 100px wide, 8px margin
    float coverY = 40;
    float coverW = 100;
    float coverH = 100;
    C2D_DrawImageAt(state->album_cover, coverX, coverY, 0.5f, NULL, 1.0f, 1.0f);
}
```

## Architecture
The album cover image is rendered on the top screen of the Nintendo 3DS, which has a resolution of 400x240 pixels. The album cover is set to a resolution of 100x100 pixels, which is appropriate for the screen size and leaves room for other UI elements.

## Start Here
Review `source/ui.c` to understand how the album cover image is rendered and positioned on the top screen. The relevant code is in the `draw_now_playing` function.