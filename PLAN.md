# Search — Dedicated Search Screen Implementation Plan

> **Goal:** Introduce a dedicated `SCREEN_SEARCH` list that launches when the user presses **Y** from any screen (including the starting menu). The search screen has a search bar + filter checkboxes at the top, and filtered results displayed as a navigable list below. Selecting a result navigates to the appropriate detail screen.

---

## 1. Architecture Overview

### New Screen

Add `SCREEN_SEARCH` to the screen enum. This is a **new dedicated list/screen**, not an overlay on the existing starting-menu screens.

```c
enum { SCREEN_ARTISTS, SCREEN_ALBUMS, SCREEN_TRACKS, SCREEN_PLAYER, SCREEN_SEARCH };
```

### Navigation Flow

```
Starting Menu (Artists) → Y → SCREEN_SEARCH (dedicated search list)
  SCREEN_SEARCH → A (select result) → SCREEN_ALBUMS / SCREEN_TRACKS (as appropriate)
  SCREEN_SEARCH → B → return to previous screen
```

### New State Fields

Add fields to `UiState` to track the search screen state independently:

```c
// ui.h — new fields in UiState:
int   search_selected;    // [NEW] index into filtered list on SCREEN_SEARCH
int   search_scroll;      // [NEW] scroll offset for the search results list
int   search_prev_screen; // [NEW] which screen we navigated from (for B-back)
```

These are **separate** from `selected_artist/album/track` and `scroll_offset`:
- `selected_artist/album/track` = position in the **full** list on their respective screens
- `search_selected` = position in the **filtered** results on `SCREEN_SEARCH`
- `search_scroll` = scroll offset for the search results list
- `search_prev_screen` = the screen we navigated from (Artists, Albums, or Tracks), so B can return correctly

Zero-initialized by `calloc(1, sizeof(UiState))` in `main.c`.

---

## 2. Search Screen Rendering

### Layout

The search screen draws on the **bottom screen** (same as other list screens):

```
┌─────────────────────────┐
│ [Search:        ]       │  ← Search bar (row 0-1)
│ ☑Name ☑Artist ☑Album    │  ← Filter checkboxes (row 2)
├─────────────────────────┤
│ The Beatles             │  ← Row 3+ (filtered results)
│ The Rolling Stones      │
│ Arctic Monkeys          │
│ ...                     │
└─────────────────────────┘
```

### Drawing Logic

The `SCREEN_SEARCH` case in `ui_draw()` follows the same pattern as other screens:

```c
case SCREEN_SEARCH:
    display_count = 0;
    int *names_ptr[MAX_ITEMS];
    
    // Filter ALL items (artists, albums, tracks) into the search results
    int search_result_count = 0;
    search_filter_all(&state->artists, &state->albums, 
                      &state->tracks, search_results, 
                      state->search_query, state->filter_fields,
                      MAX_ITEMS);
    
    // Populate display names from search_results
    for (int i = 0; i < search_result_count && i < MAX_ITEMS; i++) {
        names_ptr[i] = search_results[i].name;
        display_count = search_result_count;
    }
    
    draw_list(names_ptr, display_count,
              state->search_selected, state->search_scroll,
              "Search", true);
    
    // Draw search bar + checkboxes
    draw_search_bar(state, 0, 0);
    draw_checkboxes(state, 0, 1);
    break;
```

The `search_results` array is a **static** buffer inside `ui_draw()` (to avoid stack overflow):

```c
// Static buffer for search results — must be static, not local
typedef struct { char name[MAX_NAME_LEN]; char id[MAX_ID_LEN]; int type; } SearchResult;
static SearchResult s_search_results[MAX_ITEMS];
```

`type` distinguishes the result: `0` = artist, `1` = album, `2` = track. This is needed for the selection handler to know which screen to navigate to.

---

## 3. Detailed Changes

### Change 1: Add new search fields to UiState

**File:** `source/ui.h`

Add three fields after the existing search fields:

```c
typedef struct {
    // ... existing fields ...
    int   search_query[128];      // [already exists] current search string
    int   filter_fields;          // [already exists] bitmask of checked fields
    int   search_selected;        // [NEW] index into filtered results on SEARCH screen
    int   search_scroll;          // [NEW] scroll offset for search results list
    int   search_prev_screen;     // [NEW] which screen we came from (B-back target)
    // ... rest of struct ...
} UiState;
```

These are zero-initialized by `calloc(1, sizeof(UiState))` in `main.c`.

---

### Change 2: Add `SCREEN_SEARCH` to the screen enum

**File:** `source/ui.h`

Add `SCREEN_SEARCH` to the screen enum in `UiState`:

```c
enum {
    SCREEN_ARTISTS,
    SCREEN_ALBUMS,
    SCREEN_TRACKS,
    SCREEN_PLAYER,
    SCREEN_SEARCH    // [NEW] dedicated search screen
};
```

---

### Change 3: Update `ui_handle_input()` — Y launches search screen

**File:** `source/ui.c`, `ui_handle_input()`

On any of the starting-menu screens (Artists, Albums, Tracks), pressing **Y** navigates to `SCREEN_SEARCH`:

```c
if (down & KEY_Y && !state->search_active) {
    switch (state->screen) {
        case SCREEN_ARTISTS:
        case SCREEN_ALBUMS:
        case SCREEN_TRACKS:
            state->search_prev_screen = state->screen;
            state->search_query[0]  = '\0';
            state->filter_fields    = 0;
            state->search_selected  = 0;
            state->search_scroll    = 0;
            state->screen           = SCREEN_SEARCH;
            return true;  // Don't fall through to main block
    }
}
```

---

### Change 4: Implement `SCREEN_SEARCH` in `ui_draw()`

**File:** `source/ui.c`, `ui_draw()`

Add a new case for `SCREEN_SEARCH` that:
1. Runs the unified search filter across all three lists
2. Draws the search bar + filter checkboxes
3. Draws the filtered results as a list

```c
case SCREEN_SEARCH:
    // Run unified search across artists, albums, and tracks
    int result_count = search_filter_all(
        &state->artists, &state->albums, &state->tracks,
        search_results,          // static SearchResult[MAX_ITEMS]
        state->search_query,
        state->filter_fields,
        MAX_ITEMS
    );

    // Clamp selection to result count
    if (result_count > 0 && state->search_selected >= result_count)
        state->search_selected = result_count - 1;
    if (result_count == 0)
        state->search_selected = 0;

    draw_search_bar(state, 0, 0);       // Row 0-1: search input
    draw_checkboxes(state, 0, 1);       // Row 2: filter checkboxes
    draw_results_list(search_results, result_count,
                      state->search_selected, state->search_scroll); // Row 3+

    if (result_count == 0 && state->search_query[0]) {
        draw_text(8, 100, 0.5f, COL_DIM, "No results found.");
    }
    break;
```

The `search_results` array and `search_filter_all()` helper are implemented in `source/ui.c`.

---

### Change 5: Implement `search_filter_all()` — unified search

**File:** `source/ui.c` (new function)

Collates results from all three lists (artists, albums, tracks) into a single filtered list:

```c
typedef struct { char name[MAX_NAME_LEN]; char id[MAX_ID_LEN]; int type; }
    SearchResult;  // type: 0=artist, 1=album, 2=track

static int search_filter_all(NaviArtist *artists, NaviAlbum *albums,
                             NaviTrack *tracks, SearchResult *out,
                             const char *query, int filter_fields,
                             int max_count) {
    int count = 0;
    
    // Search artists if name or artist is checked
    if (!filter_fields || (filter_fields & 1) || (filter_fields & 2)) {
        for (int i = 0; i < artists->count && count < max_count; i++) {
            if (matches(artists->items[i].name, query, 1) ||
                (!filter_fields || (filter_fields & 2) && matches(artists->items[i].name, query, 2))) {
                // ... add to out[count++] ...
            }
        }
    }
    
    // Search albums if name or artist or album is checked
    // ... similar loop ...
    
    // Search tracks if name or artist or album is checked
    // ... similar loop ...
    
    return count;
}
```

---

### Change 6: B-back from search to previous screen

**File:** `source/ui.c`, `ui_handle_input()`

When on `SCREEN_SEARCH`, pressing **B** returns to the saved previous screen:

```c
if (down & KEY_B && state->screen == SCREEN_SEARCH) {
    state->screen = state->search_prev_screen;
    return true;  // Don't fall through
}
```

---

### Change 7: A-select on search result — navigate to detail screen

**File:** `source/ui.c`, `ui_handle_input()`

When on `SCREEN_SEARCH` and pressing **A** with a non-empty query, look up the result and navigate:

```c
if (down & KEY_A && state->screen == SCREEN_SEARCH && state->search_query[0]) {
    // Get the SearchResult at search_selected
    SearchResult *result = get_search_result_at(search_results, state->search_selected);
    
    if (result->type == 0) {
        // Artist result → find in full artist list, set selected_artist, go to albums
        state->selected_artist = find_artist_by_id(&state->artists, result->id);
        state->screen = SCREEN_ALBUMS;
    } else if (result->type == 1) {
        // Album result → find in full album list, set selected_album, go to tracks
        state->selected_album = find_album_by_id(&state->albums, result->id);
        state->screen = SCREEN_TRACKS;
    } else if (result->type == 2) {
        // Track result → find in full track list, set selected_track, go to player
        state->selected_track = find_track_by_id(&state->tracks, result->id);
        state->screen = SCREEN_PLAYER;
    }
    return true;  // Don't fall through
}
```

The `find_*_by_id()` helpers iterate the full list and return the index where `id` matches.

---

### Change 8: Add swkbd keyboard support on SEARCH screen

**File:** `source/ui.c`

When on `SCREEN_SEARCH` and pressing **A** with an empty query, open the keyboard:

```c
if (down & KEY_A && state->screen == SCREEN_SEARCH && !state->search_query[0]) {
    // Open swkbd with empty query
    // On input, store result in state->search_query
    // Query is applied in real-time on next draw cycle
    return true;
}
```

---

### Change 9: Optimize `strcasestr_simple()`

**File:** `source/ui.c`

Pre-compute the lowercase version of the needle to reduce `tolower()` calls.

```c
static int strcasestr_simple(const char *haystack, const char *needle) {
    if (!needle[0]) return 1;
    char needle_lower[128];
    int nlen = 0;
    for (; needle[nlen] && nlen < 127; nlen++)
        needle_lower[nlen] = tolower(needle[nlen]);
    needle_lower[nlen] = '\0';

    for (const char *h = haystack; *h; h++) {
        int i = 0;
        while (needle_lower[i] && h[i] && (tolower(h[i]) == needle_lower[i])) i++;
        if (!needle_lower[i]) return 1;
    }
    return 0;
}
```

---

## 4. Input Flow Diagram

### Search Screen — Full Flow
```
Starting screen (Artists/Albums/Tracks)
  ↓
Press Y → SCREEN_SEARCH
  → search_prev_screen = current_screen
  → search_query = ""
  → filter_fields = 0 (all checked)
  → search_selected = 0
  ↓
Press D-pad UP/DOWN → moves search_selected in filtered list
Press A (empty query) → opens keyboard (swkbd)
Press A (non-empty query) → selects result:
  → type=artist → find_artist_by_id() → SCREEN_ALBUMS
  → type=album  → find_album_by_id()  → SCREEN_TRACKS
  → type=track  → find_track_by_id()  → SCREEN_PLAYER
Press B → return to search_prev_screen
Press Y → clears query, keeps on search screen
Touch checkbox → toggles filter field
```

### Example: Select an Artist from Search
```
User is browsing Artists list
User presses Y → SCREEN_SEARCH opens
User types "rolling" in search bar
  → filtered results: ["The Rolling Stones", "Rolling Stones (compilation)", ...]
User navigates to result[0] ("The Rolling Stones"), presses A
  → find_artist_by_id() finds "The Rolling Stones" at artists[42]
  → selected_artist = 42
  → screen = SCREEN_ALBUMS (loaded with Rolling Stones' albums)
```

### Example: Exit Search
```
User is at Artists[15], presses Y → SCREEN_SEARCH
User navigates search results briefly, then presses B
  → screen = SCREEN_ARTISTS (search_prev_screen)
  → selected_artist = 15 (restored from before search)
  → scroll_offset restored to pre-search position
  → user is back exactly where they were
```

---

## 5. Testing Plan

### Functional testing (on 3DS hardware)

| # | Test | Steps | Expected |
|---|---|---|---|
| 1 | Launch search screen | From Artists/Albums/Tracks, press Y | SCREEN_SEARCH displays search bar + empty list |
| 2 | Navigate search results | Type query, D-pad UP/DOWN | List scrolls, highlight moves correctly |
| 3 | Select artist from search | Type query, select artist, press A | Navigates to albums screen for that artist |
| 4 | Select album from search | Type query, select album, press A | Navigates to tracks screen for that album |
| 5 | Select track from search | Type query, select track, press A | Enters player screen for that track |
| 6 | Exit without select | Type query, press B | Returns to previous screen at exact position |
| 7 | Filter by checkbox | Toggle checkboxes, verify results | Only matching fields shown |
| 8 | Empty results | Type "xyz123" (no match) | "No results found." message shown |
| 9 | Keyboard open | Press A with empty query on SEARCH | Keyboard opens with empty text |
| 10 | Keyboard pre-fill | Type "abc", B to exit, Y to re-enter | Keyboard shows "abc" pre-filled |

---

## 6. Summary

**~3 files to modify, ~120 lines of new/changed code.**

Key differences from the overlay approach:
- **`SCREEN_SEARCH` is a dedicated screen**, not an in-place filter overlay
- Navigation is **forward/back** (Y → search, B → back), not overlay toggle
- No need to save/restore `selected_artist/album/track` and `scroll_offset` — those stay untouched on their original screens
- The `search_selected` index is independent of any other selection index
- A-select on a search result uses ID lookup to find the item in the full list, then navigates directly to the appropriate detail screen
- The unified `search_filter_all()` function collates results from all three data sources (artists, albums, tracks) into one list
