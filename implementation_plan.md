# Implementation Plan - Remove All Search Functionality

## 1. Architecture & Patterns
- **Style**: Pure removal — delete code, no new logic to add.
- **Key Files**: `source/ui.h` (1 file), `source/ui.c` (1 file)
- **Data Model Impact**: Removes `SCREEN_SEARCH` enum value, removes 9 `UiState` search fields, removes `ui_search_toggle_field` declaration.
- **Side Effects**: 
  - `strcasestr_simple()` is exclusively used by search → safe to remove.
  - `swkbd` keyboard code is exclusively used for search → safe to remove.
  - `filter_fields` field removal also removes the search field-toggling mechanism.
  - No other file (`main.c`, `api.c`, `audio.c`) references search at all.

## 2. Step-by-Step Implementation Strategy

### Phase 1: Remove from `source/ui.h`
**Subagent 1**: Edit `source/ui.h` — 3 atomic changes (Step 1-3)

### Phase 2: Remove from `source/ui.c` — Helper functions & data structures
**Subagent 2**: Edit `source/ui.c` — Remove forward declarations (Step 4)
**Subagent 3**: Edit `source/ui.c` — Remove checkbox helpers (Step 5)
**Subagent 4**: Edit `source/ui.c` — Remove search matching helpers (Step 6)

### Phase 3: Remove from `source/ui.c` — `ui_draw()` search case
**Subagent 5**: Edit `source/ui.c` — Remove `case SCREEN_SEARCH:` (Step 7)

### Phase 4: Remove from `source/ui.c` — `ui_handle_input()` search code
**Subagent 6**: Edit `source/ui.c` — Remove KEY_Y handler (Step 8)
**Subagent 7**: Edit `source/ui.c` — Remove SEARCH screen input block (Step 9)

### Phase 5: Remove from `source/ui.c` — bottom-of-file search functions
**Subagent 8**: Edit `source/ui.c` — Remove `s_search_results` + `find_id_in_*` (Step 10)
**Subagent 9**: Edit `source/ui.c` — Remove `search_filter_all` (Step 11)
**Subagent 10**: Edit `source/ui.c` — Remove `draw_search_bar` (Step 12)
**Subagent 11**: Edit `source/ui.c` — Remove `draw_checkboxes` (Step 13)

## 3. Files Not Modified
- **`main.c`**: No search references.
- **`api.c` / `api.h`**: No search references.
- **`audio.c` / `audio.h`**: No search references.
- **`config.c` / `config.h`**: No search references.
- **`debug.c` / `debug.h`**: No search references.
