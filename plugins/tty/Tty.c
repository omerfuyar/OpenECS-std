// The tty standard plugin: grids of characters, which it draws into the pixels surface of another plugin's panel at any size (DESIGN 4).
// Terminals, consoles and logs build on it. It calls SDL3 and SDL3_ttf itself, using the executable's copy.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

#pragma region Source Only

/// @brief A name of the plugin: "tty." and a local name.
#define TTY_NAME(localName) "tty." localName

/// @brief The font tty ships with, in its folder.
#define TTY_FONT "fonts/RobotoMono-Regular.ttf"

/// @brief Columns a tab moves to: the next multiple of this.
#define TTY_TAB 8

/// @brief The most glyphs kept drawn; the cache starts again when it is full.
#define TTY_GLYPHS_MAX 4096

/// @brief The colours a grid starts with: light grey on the colour of the core's theme behind panels.
#define TTY_FOREGROUND 0xFFDCDEE2
#define TTY_BACKGROUND 0xFF18191C

/// @brief One cell of a grid: a character and its colours.
typedef struct TtyCell
{
    u32 character; // a Unicode code point; a space for an empty cell
    u32 foreground;
    u32 background;
} TtyCell;

/// @brief A grid of characters, with the cursor where print writes and the colours it writes in.
typedef struct TtyGrid
{
    i32 columns;
    i32 rows;
    TtyCell *cells; // rows one after another
    i32 cursorColumn;
    i32 cursorRow;
    u32 foreground;
    u32 background;
} TtyGrid;

/// @brief A drawn glyph: a character in a colour at a size in pixels.
typedef struct TtyGlyph
{
    u32 character;
    u32 color;
    i32 size;
    SDL_Surface *surface; // NULL if the font has no glyph for the character
} TtyGlyph;

/// @brief A font at one size, in pixels, with the size of its cells.
typedef struct TtyFont
{
    i32 size;
    TTF_Font *font;
    i32 cellWidth;
    i32 cellHeight;
} TtyFont;

static struct
{
    ECSPlugin plugin;
    char *fontPath;
    TtyFont *fonts; // every size asked for so far
    usz fontCount;
    TtyGlyph *glyphs; // the glyphs drawn so far
    usz glyphCount;
    bool ttfStarted;
} TTY = {0};

/// @brief Forgets every opened font and drawn glyph, such as when the font changes.
static void TtyForgetFonts(void)
{
    for (usz i = 0; i < TTY.glyphCount; i++)
    {
        SDL_DestroySurface(TTY.glyphs[i].surface);
    }

    for (usz i = 0; i < TTY.fontCount; i++)
    {
        TTF_CloseFont(TTY.fonts[i].font);
    }

    SDL_free(TTY.glyphs);
    SDL_free(TTY.fonts);
    TTY.glyphs = NULL;
    TTY.glyphCount = 0;
    TTY.fonts = NULL;
    TTY.fontCount = 0;
}

/// @brief Reads the setting tty.font: a font file, or the one tty ships with when it is empty. A relative path starts at the executable's folder.
static void TtyReadFont(void *data)
{
    (void)data;

    const char *font = ECSValue_GetString(ECSSetting_Get(TTY_NAME("font")), "");
    const char *folder = font[0] == '\0' ? ECSPlugin_GetFolder(TTY.plugin) : font[0] == '/' ? "" : SDL_GetBasePath();
    char *path = NULL;

    if (SDL_asprintf(&path, "%s%s", folder, font[0] == '\0' ? TTY_FONT : font) < 0)
    {
        path = NULL;
    }

    TtyForgetFonts();
    SDL_free(TTY.fontPath);
    TTY.fontPath = path;
}

/// @brief Gets the font at a size in pixels, opening it the first time.
/// @return The font, or NULL if it cannot be opened; the reason is logged.
static TtyFont *TtyGetFont(f32 pixels)
{
    i32 size = (i32)SDL_max(1.0f, SDL_roundf(pixels));

    for (usz i = 0; i < TTY.fontCount; i++)
    {
        if (TTY.fonts[i].size == size)
        {
            return &TTY.fonts[i];
        }
    }

    TTF_Font *font = TTY.fontPath == NULL ? NULL : TTF_OpenFont(TTY.fontPath, (float)size);
    int advance = 0;
    TtyFont *fonts = font == NULL || !TTF_GetGlyphMetrics(font, 'M', NULL, NULL, NULL, NULL, &advance) ? NULL : SDL_realloc(TTY.fonts, (TTY.fontCount + 1) * sizeof(TtyFont));

    if (fonts == NULL)
    {
        ECS_Log(TTY.plugin, ECSLogLevel_Error, "Cannot open the font '%s' at %d pixels: %s", TTY.fontPath == NULL ? "" : TTY.fontPath, size, SDL_GetError());
        TTF_CloseFont(font);
        return NULL;
    }

    TTY.fonts = fonts;
    TTY.fonts[TTY.fontCount] = (TtyFont){.size = size, .font = font, .cellWidth = SDL_max(1, advance), .cellHeight = SDL_max(1, TTF_GetFontHeight(font))};
    return &TTY.fonts[TTY.fontCount++];
}

/// @brief Gets a character drawn in a colour with a font, drawing it the first time.
/// @return The glyph's surface, or NULL if the font has no glyph for it.
static SDL_Surface *TtyGetGlyph(TtyFont *font, u32 character, u32 color)
{
    for (usz i = 0; i < TTY.glyphCount; i++)
    {
        TtyGlyph *glyph = &TTY.glyphs[i];

        if (glyph->character == character && glyph->color == color && glyph->size == font->size)
        {
            return glyph->surface;
        }
    }

    // the cache starts again when it is full, so a grid with many colours cannot grow it without end
    if (TTY.glyphCount == TTY_GLYPHS_MAX)
    {
        for (usz i = 0; i < TTY.glyphCount; i++)
        {
            SDL_DestroySurface(TTY.glyphs[i].surface);
        }

        TTY.glyphCount = 0;
    }

    TtyGlyph *glyphs = SDL_realloc(TTY.glyphs, (TTY.glyphCount + 1) * sizeof(TtyGlyph));

    if (glyphs == NULL)
    {
        return NULL;
    }

    SDL_Color sdlColor = {(Uint8)(color >> 16), (Uint8)(color >> 8), (Uint8)color, (Uint8)(color >> 24)};
    TTY.glyphs = glyphs;
    TTY.glyphs[TTY.glyphCount] = (TtyGlyph){.character = character, .color = color, .size = font->size, .surface = TTF_FontHasGlyph(font->font, character) ? TTF_RenderGlyph_Blended(font->font, character, sdlColor) : NULL};
    return TTY.glyphs[TTY.glyphCount++].surface;
}

static TtyCell *TtyCellAt(TtyGrid *grid, i32 column, i32 row)
{
    return &grid->cells[(usz)row * (usz)grid->columns + (usz)column];
}

/// @brief Empties a row with the grid's colours.
static void TtyClearRow(TtyGrid *grid, i32 row)
{
    for (i32 column = 0; column < grid->columns; column++)
    {
        *TtyCellAt(grid, column, row) = (TtyCell){.character = ' ', .foreground = grid->foreground, .background = grid->background};
    }
}

/// @brief Moves every row up by one, and empties the last.
static void TtyScroll(TtyGrid *grid)
{
    SDL_memmove(grid->cells, grid->cells + grid->columns, (usz)(grid->rows - 1) * (usz)grid->columns * sizeof(TtyCell));
    TtyClearRow(grid, grid->rows - 1);
}

/// @brief Frees a grid; the Destroy function of the handle type tty.grid.
static void TtyDestroy(void *object)
{
    TtyGrid *grid = object;
    SDL_free(grid->cells);
    SDL_free(grid);
}

/// @brief Gives the cells to a new size, keeping the cells that still fit, at the same places.
/// @return false if out of memory; the grid is unchanged.
static bool TtyResizeCells(TtyGrid *grid, i32 columns, i32 rows)
{
    columns = SDL_max(1, columns);
    rows = SDL_max(1, rows);
    TtyCell *cells = SDL_malloc((usz)columns * (usz)rows * sizeof(TtyCell));

    if (cells == NULL)
    {
        return false;
    }

    for (i32 row = 0; row < rows; row++)
    {
        for (i32 column = 0; column < columns; column++)
        {
            bool kept = grid->cells != NULL && row < grid->rows && column < grid->columns;
            cells[(usz)row * (usz)columns + (usz)column] = kept ? *TtyCellAt(grid, column, row) : (TtyCell){.character = ' ', .foreground = grid->foreground, .background = grid->background};
        }
    }

    SDL_free(grid->cells);
    grid->cells = cells;
    grid->columns = columns;
    grid->rows = rows;
    grid->cursorColumn = SDL_min(grid->cursorColumn, columns - 1);
    grid->cursorRow = SDL_min(grid->cursorRow, rows - 1);
    return true;
}

static TtyGrid *TtyNew(i32 columns, i32 rows)
{
    TtyGrid *grid = SDL_calloc(1, sizeof(TtyGrid));

    if (grid == NULL)
    {
        return NULL;
    }

    grid->foreground = TTY_FOREGROUND;
    grid->background = TTY_BACKGROUND;

    if (!TtyResizeCells(grid, columns, rows))
    {
        SDL_free(grid);
        return NULL;
    }

    return grid;
}

static void TtyResize(TtyGrid *grid, i32 columns, i32 rows)
{
    if (!TtyResizeCells(grid, columns, rows))
    {
        ECS_Log(TTY.plugin, ECSLogLevel_Error, "Cannot resize a grid: out of memory.");
    }
}

static void TtySize(TtyGrid *grid, i32 *retColumns, i32 *retRows)
{
    *retColumns = grid->columns;
    *retRows = grid->rows;
}

static void TtyClear(TtyGrid *grid)
{
    for (i32 row = 0; row < grid->rows; row++)
    {
        TtyClearRow(grid, row);
    }

    grid->cursorColumn = 0;
    grid->cursorRow = 0;
}

static void TtyColors(TtyGrid *grid, i64 foreground, i64 background)
{
    grid->foreground = (u32)foreground;
    grid->background = (u32)background;
}

static void TtySetCursor(TtyGrid *grid, i32 column, i32 row)
{
    grid->cursorColumn = SDL_clamp(column, 0, grid->columns - 1);
    grid->cursorRow = SDL_clamp(row, 0, grid->rows - 1);
}

static void TtyGetCursor(TtyGrid *grid, i32 *retColumn, i32 *retRow)
{
    *retColumn = grid->cursorColumn;
    *retRow = grid->cursorRow;
}

static void TtyPut(TtyGrid *grid, i32 column, i32 row, const char *text)
{
    usz length = SDL_strlen(text);

    for (u32 character = SDL_StepUTF8(&text, &length); character != 0; character = SDL_StepUTF8(&text, &length), column++)
    {
        if (row >= 0 && row < grid->rows && column >= 0 && column < grid->columns)
        {
            *TtyCellAt(grid, column, row) = (TtyCell){.character = character, .foreground = grid->foreground, .background = grid->background};
        }
    }
}

static void TtyPrint(TtyGrid *grid, const char *text)
{
    usz length = SDL_strlen(text);

    for (u32 character = SDL_StepUTF8(&text, &length); character != 0; character = SDL_StepUTF8(&text, &length))
    {
        if (character == '\r')
        {
            grid->cursorColumn = 0;
            continue;
        }

        // a line ends at a new line, or when the row is full
        if (character == '\n' || grid->cursorColumn >= grid->columns)
        {
            grid->cursorColumn = 0;
            grid->cursorRow++;

            if (grid->cursorRow == grid->rows)
            {
                TtyScroll(grid);
                grid->cursorRow = grid->rows - 1;
            }

            if (character == '\n')
            {
                continue;
            }
        }

        if (character == '\t')
        {
            grid->cursorColumn = SDL_min(grid->columns, (grid->cursorColumn / TTY_TAB + 1) * TTY_TAB);
            continue;
        }

        *TtyCellAt(grid, grid->cursorColumn, grid->cursorRow) = (TtyCell){.character = character, .foreground = grid->foreground, .background = grid->background};
        grid->cursorColumn++;
    }
}

static void TtyCellSize(f32 size, f32 *retWidth, f32 *retHeight)
{
    TtyFont *font = TtyGetFont(size);
    *retWidth = font == NULL ? 0.0f : (f32)font->cellWidth;
    *retHeight = font == NULL ? 0.0f : (f32)font->cellHeight;
}

static void TtyFit(f32 width, f32 height, f32 size, i32 *retColumns, i32 *retRows)
{
    f32 cellWidth = 0.0f;
    f32 cellHeight = 0.0f;
    TtyCellSize(size, &cellWidth, &cellHeight);
    *retColumns = cellWidth <= 0.0f ? 0 : (i32)SDL_max(0.0f, SDL_floorf(width / cellWidth));
    *retRows = cellHeight <= 0.0f ? 0 : (i32)SDL_max(0.0f, SDL_floorf(height / cellHeight));
}

static void TtyDraw(ECSSurface *surface, TtyGrid *grid, f32 x, f32 y, f32 size, bool cursor)
{
    TtyFont *font = surface->type == ECSSurfaceType_Pixels ? TtyGetFont(size * surface->scale) : NULL;
    SDL_Surface *target = font == NULL ? NULL : SDL_CreateSurfaceFrom(surface->width, surface->height, SDL_PIXELFORMAT_ARGB8888, surface->pixels.data, surface->pitch);

    if (target == NULL)
    {
        return;
    }

    i32 left = (i32)SDL_roundf(x * surface->scale);
    i32 top = (i32)SDL_roundf(y * surface->scale);

    for (i32 row = 0; row < grid->rows; row++)
    {
        for (i32 column = 0; column < grid->columns; column++)
        {
            TtyCell *cell = TtyCellAt(grid, column, row);
            SDL_Rect rect = {left + column * font->cellWidth, top + row * font->cellHeight, font->cellWidth, font->cellHeight};
            SDL_FillSurfaceRect(target, &rect, cell->background);

            SDL_Surface *glyph = cell->character == ' ' ? NULL : TtyGetGlyph(font, cell->character, cell->foreground);

            if (glyph != NULL)
            {
                SDL_Rect place = {rect.x, rect.y, glyph->w, glyph->h};
                SDL_BlitSurface(glyph, NULL, target, &place);
            }
        }
    }

    // the cursor is a line under its cell
    if (cursor)
    {
        i32 line = SDL_max(2, font->cellHeight / 8);
        SDL_Rect rect = {left + grid->cursorColumn * font->cellWidth, top + (grid->cursorRow + 1) * font->cellHeight - line, font->cellWidth, line};
        SDL_FillSurfaceRect(target, &rect, grid->foreground);
    }

    SDL_DestroySurface(target);
}

static void TtyFree(void)
{
    TtyForgetFonts();
    SDL_free(TTY.fontPath);

    // SDL_ttf counts its starts, so the core's own use goes on
    if (TTY.ttfStarted)
    {
        TTF_Quit();
    }

    SDL_zero(TTY);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    TTY.plugin = plugin;
    TTY.ttfStarted = TTF_Init();

    if (!TTY.ttfStarted)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "Cannot start SDL_ttf: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    const ECSSettingDesc font = {
        .name = TTY_NAME("font"),
        .type = ECSSettingType_String,
        .description = "Monospace TrueType font of character grids; empty for the one tty ships with",
        .defaultString = "",
        .Changed = TtyReadFont,
    };

    SHU_ReturnResult(ECSSetting_Declare(plugin, &font), TtyFree(););
    TtyReadFont(NULL);
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, TTY_NAME("grid"), TtyDestroy), TtyFree(););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {TTY_NAME("new"), (ECSFunction)TtyNew, "handle<tty.grid>(int columns, int rows)", "Make a grid of empty cells"},
        {TTY_NAME("resize"), (ECSFunction)TtyResize, "void(handle<tty.grid> grid, int columns, int rows)", "Change a grid's size, keeping the cells that fit"},
        {TTY_NAME("size"), (ECSFunction)TtySize, "void(handle<tty.grid> grid, out int columns, out int rows)", "Give a grid's size"},
        {TTY_NAME("clear"), (ECSFunction)TtyClear, "void(handle<tty.grid> grid)", "Empty every cell, and move the cursor to the top left"},
        {TTY_NAME("colors"), (ECSFunction)TtyColors, "void(handle<tty.grid> grid, int64 foreground, int64 background)", "Set the colours that later writing uses"},
        {TTY_NAME("put"), (ECSFunction)TtyPut, "void(handle<tty.grid> grid, int column, int row, string text)", "Write text from a cell to the right, cut at the grid's edge"},
        {TTY_NAME("print"), (ECSFunction)TtyPrint, "void(handle<tty.grid> grid, string text)", "Write text at the cursor, as a terminal does"},
        {TTY_NAME("setCursor"), (ECSFunction)TtySetCursor, "void(handle<tty.grid> grid, int column, int row)", "Move the cursor"},
        {TTY_NAME("cursor"), (ECSFunction)TtyGetCursor, "void(handle<tty.grid> grid, out int column, out int row)", "Give where the cursor is"},
        {TTY_NAME("cell"), (ECSFunction)TtyCellSize, "void(float size, out float width, out float height)", "Give the size of a cell at a font size"},
        {TTY_NAME("fit"), (ECSFunction)TtyFit, "void(float width, float height, float size, out int columns, out int rows)", "Give how many cells fit a rectangle at a font size"},
        {TTY_NAME("draw"), (ECSFunction)TtyDraw, "void(handle<ecs.surface> surface, handle<tty.grid> grid, float x, float y, float size, bool cursor)", "Draw a grid into a surface at a font size"},
    };

    // a plugin whose Init fails gets no Shutdown, so it cleans up here
    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), TtyFree(););
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
    TtyFree();
}
