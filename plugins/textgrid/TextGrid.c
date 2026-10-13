// The textgrid standard plugin: grids of characters in colours, which it draws into the pixels surface of another plugin's panel at any size (DESIGN 4).
// Terminals, consoles, logs and text editors build on it. It draws through the draw plugin, with draw's monospace font, so one plugin draws all text.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief Columns a tab moves to: the next multiple of this.
#define TEXTGRID_TAB 8

/// @brief The colours a grid starts with if the theme's cannot be read.
#define TEXTGRID_FALLBACK_FOREGROUND 0xFFDCDEE2
#define TEXTGRID_FALLBACK_BACKGROUND 0xFF18191C

/// @brief One cell of a grid: a character and its colours.
typedef struct TextGridCell
{
    u32 character; // a Unicode code point; a space for an empty cell
    ECSColor foreground;
    ECSColor background;
} TextGridCell;

/// @brief A grid of characters, with the cursor where print writes and the colours it writes in.
typedef struct TextGridGrid
{
    i32 columns;
    i32 rows;
    TextGridCell *cells; // rows one after another
    i32 cursorColumn;
    i32 cursorRow;
    ECSColor foreground;
    ECSColor background;
} TextGridGrid;

/// @brief The functions of draw that textgrid draws with.
typedef void *(*TextGridFontFunction)(const char *name);
typedef void (*TextGridFillFunction)(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, ECSColor color);
typedef f32 (*TextGridTextFunction)(ECSSurface *surface, void *font, const char *text, f32 x, f32 y, f32 size, ECSColor color);
typedef void (*TextGridMeasureFunction)(void *font, const char *text, f32 size, f32 *retWidth, f32 *retHeight);

static struct
{
    ECSPlugin plugin;
    void *font; // draw's monospace font
    TextGridFillFunction Fill;
    TextGridTextFunction Text;
    TextGridMeasureFunction Measure;
} TEXTGRID = {0};

static TextGridCell *TextGridCellAt(TextGridGrid *grid, i32 column, i32 row)
{
    return &grid->cells[(usz)row * (usz)grid->columns + (usz)column];
}

/// @brief Empties a row with the grid's colours.
static void TextGridClearRow(TextGridGrid *grid, i32 row)
{
    for (i32 column = 0; column < grid->columns; column++)
    {
        *TextGridCellAt(grid, column, row) = (TextGridCell){.character = ' ', .foreground = grid->foreground, .background = grid->background};
    }
}

/// @brief Moves every row up by one, and empties the last.
static void TextGridScroll(TextGridGrid *grid)
{
    SDL_memmove(grid->cells, grid->cells + grid->columns, (usz)(grid->rows - 1) * (usz)grid->columns * sizeof(TextGridCell));
    TextGridClearRow(grid, grid->rows - 1);
}

/// @brief Frees a grid; the Destroy function of the handle type textgrid.grid.
static void TextGridDestroy(void *object)
{
    TextGridGrid *grid = object;
    SDL_free(grid->cells);
    SDL_free(grid);
}

/// @brief Gives the cells to a new size, keeping the cells that still fit, at the same places.
/// @return false if out of memory; the grid is unchanged.
static bool TextGridResizeCells(TextGridGrid *grid, i32 columns, i32 rows)
{
    columns = SDL_max(1, columns);
    rows = SDL_max(1, rows);
    TextGridCell *cells = SDL_malloc((usz)columns * (usz)rows * sizeof(TextGridCell));

    if (cells == nullptr)
    {
        return false;
    }

    for (i32 row = 0; row < rows; row++)
    {
        for (i32 column = 0; column < columns; column++)
        {
            bool kept = grid->cells != nullptr && row < grid->rows && column < grid->columns;
            cells[(usz)row * (usz)columns + (usz)column] = kept ? *TextGridCellAt(grid, column, row) : (TextGridCell){.character = ' ', .foreground = grid->foreground, .background = grid->background};
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

static TextGridGrid *TextGridNew(i32 columns, i32 rows)
{
    TextGridGrid *grid = SDL_calloc(1, sizeof(TextGridGrid));

    if (grid == nullptr)
    {
        return nullptr;
    }

    // a grid starts in the theme's colours of text on the background
    grid->foreground = TEXTGRID_FALLBACK_FOREGROUND;
    grid->background = TEXTGRID_FALLBACK_BACKGROUND;
    ECSColor_Parse("text", &grid->foreground);
    ECSColor_Parse("background", &grid->background);

    if (!TextGridResizeCells(grid, columns, rows))
    {
        SDL_free(grid);
        return nullptr;
    }

    return grid;
}

static void TextGridResize(TextGridGrid *grid, i32 columns, i32 rows)
{
    if (!TextGridResizeCells(grid, columns, rows))
    {
        ECS_LogError(TEXTGRID.plugin, "Cannot resize a grid: out of memory.");
    }
}

static void TextGridSize(TextGridGrid *grid, i32 *retColumns, i32 *retRows)
{
    *retColumns = grid->columns;
    *retRows = grid->rows;
}

static void TextGridClear(TextGridGrid *grid)
{
    for (i32 row = 0; row < grid->rows; row++)
    {
        TextGridClearRow(grid, row);
    }

    grid->cursorColumn = 0;
    grid->cursorRow = 0;
}

static void TextGridColors(TextGridGrid *grid, ECSColor foreground, ECSColor background)
{
    grid->foreground = foreground;
    grid->background = background;
}

static void TextGridSetCursor(TextGridGrid *grid, i32 column, i32 row)
{
    grid->cursorColumn = SDL_clamp(column, 0, grid->columns - 1);
    grid->cursorRow = SDL_clamp(row, 0, grid->rows - 1);
}

static void TextGridGetCursor(TextGridGrid *grid, i32 *retColumn, i32 *retRow)
{
    *retColumn = grid->cursorColumn;
    *retRow = grid->cursorRow;
}

static void TextGridPut(TextGridGrid *grid, i32 column, i32 row, const char *text)
{
    usz length = SDL_strlen(text);

    for (u32 character = SDL_StepUTF8(&text, &length); character != 0; character = SDL_StepUTF8(&text, &length), column++)
    {
        if (row >= 0 && row < grid->rows && column >= 0 && column < grid->columns)
        {
            *TextGridCellAt(grid, column, row) = (TextGridCell){.character = character, .foreground = grid->foreground, .background = grid->background};
        }
    }
}

static void TextGridPrint(TextGridGrid *grid, const char *text)
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
                TextGridScroll(grid);
                grid->cursorRow = grid->rows - 1;
            }

            if (character == '\n')
            {
                continue;
            }
        }

        if (character == '\t')
        {
            grid->cursorColumn = SDL_min(grid->columns, (grid->cursorColumn / TEXTGRID_TAB + 1) * TEXTGRID_TAB);
            continue;
        }

        *TextGridCellAt(grid, grid->cursorColumn, grid->cursorRow) = (TextGridCell){.character = character, .foreground = grid->foreground, .background = grid->background};
        grid->cursorColumn++;
    }
}

/// @brief Gives the size of a cell at a font size, in layout units: the width of a character of the monospace font, and its line height.
static void TextGridCellSize(f32 size, f32 *retWidth, f32 *retHeight)
{
    TEXTGRID.Measure(TEXTGRID.font, "M", size, retWidth, retHeight);
}

static void TextGridFit(f32 width, f32 height, f32 size, i32 *retColumns, i32 *retRows)
{
    f32 cellWidth = 0.0f;
    f32 cellHeight = 0.0f;
    TextGridCellSize(size, &cellWidth, &cellHeight);
    *retColumns = cellWidth <= 0.0f ? 0 : (i32)SDL_max(0.0f, SDL_floorf(width / cellWidth));
    *retRows = cellHeight <= 0.0f ? 0 : (i32)SDL_max(0.0f, SDL_floorf(height / cellHeight));
}

/// @brief Adds a character to a text in UTF-8.
static void TextGridAppend(char *text, usz *length, u32 character)
{
    char *end = SDL_UCS4ToUTF8(character, text + *length);
    *length = (usz)(end - text);
    text[*length] = '\0';
}

static void TextGridDraw(ECSSurface *surface, TextGridGrid *grid, f32 x, f32 y, f32 size, bool cursor)
{
    // a cell is measured at the size the surface draws at, so the characters of a run stay on their cells
    f32 scale = surface->scale > 0.0f ? surface->scale : 1.0f;
    f32 cellWidth = 0.0f;
    f32 cellHeight = 0.0f;
    TextGridCellSize(size * scale, &cellWidth, &cellHeight);
    cellWidth /= scale;
    cellHeight /= scale;

    // a run of a row's cells in one colour is drawn at once: its backgrounds in one fill, and its characters in one text
    char *text = SDL_malloc((usz)grid->columns * 4 + 1);

    for (i32 row = 0; text != nullptr && row < grid->rows; row++)
    {
        f32 top = y + (f32)row * cellHeight;

        for (i32 start = 0; start < grid->columns;)
        {
            TextGridCell *first = TextGridCellAt(grid, start, row);
            i32 end = start;
            usz length = 0;
            text[0] = '\0';

            while (end < grid->columns && TextGridCellAt(grid, end, row)->background == first->background && TextGridCellAt(grid, end, row)->foreground == first->foreground)
            {
                TextGridAppend(text, &length, TextGridCellAt(grid, end, row)->character);
                end++;
            }

            f32 left = x + (f32)start * cellWidth;
            TEXTGRID.Fill(surface, left, top, (f32)(end - start) * cellWidth, cellHeight, first->background);
            TEXTGRID.Text(surface, TEXTGRID.font, text, left, top, size, first->foreground);
            start = end;
        }
    }

    SDL_free(text);

    // the cursor is a line under its cell
    if (cursor)
    {
        f32 line = SDL_max(2.0f / scale, cellHeight / 8.0f);
        TEXTGRID.Fill(surface, x + (f32)grid->cursorColumn * cellWidth, y + (f32)(grid->cursorRow + 1) * cellHeight - line, cellWidth, line, grid->foreground);
    }
}

/// @brief Frees a grid now, instead of when no one uses its handle.
static void TextGridFree(TextGridGrid *grid)
{
    ECSHandle_Forget(grid);
    TextGridDestroy(grid);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    TEXTGRID.plugin = plugin;

    // the grids are drawn with draw's functions and its monospace font
    TextGridFontFunction Font = nullptr;
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&Font, "draw.font", "handle<draw.font>(string name)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&TEXTGRID.Fill, "draw.fill", "void(handle<ecs.surface> surface, float x, float y, float width, float height, color color)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&TEXTGRID.Text, "draw.text", "float(handle<ecs.surface> surface, handle<draw.font> font, string text, float x, float y, float size, color color)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&TEXTGRID.Measure, "draw.measure", "void(handle<draw.font> font, string text, float size, out float width, out float lineHeight)"));
    TEXTGRID.font = Font("mono");

    if (TEXTGRID.font == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "grid", TextGridDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"new", (ECSFunction)TextGridNew, "handle<textgrid.grid>(int columns, int rows)", "Make a grid of empty cells, in the theme's colours of text"},
        {"free", (ECSFunction)TextGridFree, "void(handle<textgrid.grid> grid)", "Free a grid now, instead of when no one uses its handle"},
        {"resize", (ECSFunction)TextGridResize, "void(handle<textgrid.grid> grid, int columns, int rows)", "Change a grid's size, keeping the cells that fit"},
        {"size", (ECSFunction)TextGridSize, "void(handle<textgrid.grid> grid, out int columns, out int rows)", "Give a grid's size"},
        {"clear", (ECSFunction)TextGridClear, "void(handle<textgrid.grid> grid)", "Empty every cell, and move the cursor to the top left"},
        {"colors", (ECSFunction)TextGridColors, "void(handle<textgrid.grid> grid, color foreground, color background)", "Set the colours that later writing uses"},
        {"put", (ECSFunction)TextGridPut, "void(handle<textgrid.grid> grid, int column, int row, string text)", "Write text from a cell to the right, cut at the grid's edge"},
        {"print", (ECSFunction)TextGridPrint, "void(handle<textgrid.grid> grid, string text)", "Write text at the cursor, as a terminal does"},
        {"setCursor", (ECSFunction)TextGridSetCursor, "void(handle<textgrid.grid> grid, int column, int row)", "Move the cursor"},
        {"cursor", (ECSFunction)TextGridGetCursor, "void(handle<textgrid.grid> grid, out int column, out int row)", "Give where the cursor is"},
        {"cell", (ECSFunction)TextGridCellSize, "void(float size, out float width, out float height)", "Give the size of a cell at a font size"},
        {"fit", (ECSFunction)TextGridFit, "void(float width, float height, float size, out int columns, out int rows)", "Give how many cells fit a rectangle at a font size"},
        {"draw", (ECSFunction)TextGridDraw, "void(handle<ecs.surface> surface, handle<textgrid.grid> grid, float x, float y, float size, bool cursor)", "Draw a grid into a surface at a font size"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
