// The textgrid standard plugin: grids of characters in colours, which it draws into the pixels surface of another plugin's panel at any size (DESIGN 4).
// Terminals, consoles, logs and text editors build on it. It draws through the draw plugin, with draw's monospace font, so one plugin draws all text.
// Cells can be bold and underlined, and a wide character, such as a Chinese one, takes two cells.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief Columns a tab moves to: the next multiple of this.
#define TEXTGRID_TAB 8

/// @brief The colours a grid starts with if the theme's cannot be read.
#define TEXTGRID_FALLBACK_FOREGROUND 0xFFDCDEE2
#define TEXTGRID_FALLBACK_BACKGROUND 0xFF18191C

/// @brief The styles of a cell, as flags.
#define TEXTGRID_BOLD 1
#define TEXTGRID_UNDERLINE 2

/// @brief One cell of a grid: a character, its colours and its style.
typedef struct TextGridCell
{
    u32 character; // a Unicode code point; a space for an empty cell, and 0 for the right half of a wide character
    ECSColor foreground;
    ECSColor background;
    u8 style; // TEXTGRID_BOLD and TEXTGRID_UNDERLINE
} TextGridCell;

/// @brief A grid of characters, with the cursor where print writes and the colours it writes in.
typedef struct TextGridGrid
{
    i32 columns;
    i32 rows;
    TextGridCell *cells; // rows one after another
    bool *wrapped;       // one for each row: its line goes on in the next row, because print reached the edge; a resize joins them again
    i32 cursorColumn;
    i32 cursorRow;
    ECSColor foreground;
    ECSColor background;
    u8 style;
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

/// @brief Adds a character to a text in UTF-8.
static void TextGridAppend(char *text, usz *length, u32 character)
{
    char *end = SDL_UCS4ToUTF8(character, text + *length);
    *length = (usz)(end - text);
    text[*length] = '\0';
}

/// @brief An empty cell: a space in the grid's colours, without a style.
static TextGridCell TextGridBlank(const TextGridGrid *grid)
{
    return (TextGridCell){.character = ' ', .foreground = grid->foreground, .background = grid->background};
}

/// @brief Empties a row with the grid's colours; it starts a line of its own.
static void TextGridClearRow(TextGridGrid *grid, i32 row)
{
    for (i32 column = 0; column < grid->columns; column++)
    {
        *TextGridCellAt(grid, column, row) = TextGridBlank(grid);
    }

    grid->wrapped[row] = false;
}

/// @brief How many cells a character takes: 2 for the wide and full-width characters of East Asian scripts and for most emoji, and 1 for the others.
static i32 TextGridWidth(u32 character)
{
    // the ranges of wide (W) and full-width (F) characters, from Unicode 15.1's East Asian Width property, in order; a range goes on over unassigned code points
    static const u32 wide[][2] = {
        {0x1100, 0x115F},    {0x231A, 0x231B},    {0x2329, 0x232A},    {0x23E9, 0x23EC},    {0x23F0, 0x23F0},    {0x23F3, 0x23F3},    {0x25FD, 0x25FE},
        {0x2614, 0x2615},    {0x2648, 0x2653},    {0x267F, 0x267F},    {0x2693, 0x2693},    {0x26A1, 0x26A1},    {0x26AA, 0x26AB},    {0x26BD, 0x26BE},
        {0x26C4, 0x26C5},    {0x26CE, 0x26CE},    {0x26D4, 0x26D4},    {0x26EA, 0x26EA},    {0x26F2, 0x26F3},    {0x26F5, 0x26F5},    {0x26FA, 0x26FA},
        {0x26FD, 0x26FD},    {0x2705, 0x2705},    {0x270A, 0x270B},    {0x2728, 0x2728},    {0x274C, 0x274C},    {0x274E, 0x274E},    {0x2753, 0x2755},
        {0x2757, 0x2757},    {0x2795, 0x2797},    {0x27B0, 0x27B0},    {0x27BF, 0x27BF},    {0x2B1B, 0x2B1C},    {0x2B50, 0x2B50},    {0x2B55, 0x2B55},
        {0x2E80, 0x303E},    {0x3041, 0x3247},    {0x3250, 0x4DBF},    {0x4E00, 0xA4C6},    {0xA960, 0xA97C},    {0xAC00, 0xD7A3},    {0xF900, 0xFAFF},
        {0xFE10, 0xFE19},    {0xFE30, 0xFE6B},    {0xFF01, 0xFF60},    {0xFFE0, 0xFFE6},    {0x16FE0, 0x1B2FB},  {0x1F004, 0x1F004},  {0x1F0CF, 0x1F0CF},
        {0x1F18E, 0x1F18E},  {0x1F191, 0x1F19A},  {0x1F200, 0x1F320},  {0x1F32D, 0x1F335},  {0x1F337, 0x1F37C},  {0x1F37E, 0x1F393},  {0x1F3A0, 0x1F3CA},
        {0x1F3CF, 0x1F3D3},  {0x1F3E0, 0x1F3F0},  {0x1F3F4, 0x1F3F4},  {0x1F3F8, 0x1F43E},  {0x1F440, 0x1F440},  {0x1F442, 0x1F4FC},  {0x1F4FF, 0x1F53D},
        {0x1F54B, 0x1F54E},  {0x1F550, 0x1F567},  {0x1F57A, 0x1F57A},  {0x1F595, 0x1F596},  {0x1F5A4, 0x1F5A4},  {0x1F5FB, 0x1F64F},  {0x1F680, 0x1F6C5},
        {0x1F6CC, 0x1F6CC},  {0x1F6D0, 0x1F6D2},  {0x1F6D5, 0x1F6DF},  {0x1F6EB, 0x1F6EC},  {0x1F6F4, 0x1F6FC},  {0x1F7E0, 0x1F7F0},  {0x1F90C, 0x1F93A},
        {0x1F93C, 0x1F945},  {0x1F947, 0x1F9FF},  {0x1FA70, 0x1FAF8},  {0x20000, 0x3FFFD},
    };

    for (usz i = 0; i < (usz)SDL_arraysize(wide) && character >= wide[i][0]; i++)
    {
        if (character <= wide[i][1])
        {
            return 2;
        }
    }

    return 1;
}

/// @brief Whether a cell holds the left half of a wide character, whose right half is the next cell.
static bool TextGridIsWide(TextGridGrid *grid, i32 column, i32 row)
{
    return column + 1 < grid->columns && TextGridCellAt(grid, column, row)->character != 0 && TextGridCellAt(grid, column + 1, row)->character == 0;
}

/// @brief Writes a character in the grid's colours and style into a cell, and into the next one too for a wide character, which must fit.
/// A wide character that the writing cuts in half loses its other half, which becomes a space.
static void TextGridSet(TextGridGrid *grid, i32 column, i32 row, u32 character, i32 width)
{
    i32 last = column + width - 1;

    if (column > 0 && TextGridCellAt(grid, column, row)->character == 0)
    {
        TextGridCellAt(grid, column - 1, row)->character = ' ';
    }

    if (TextGridIsWide(grid, last, row))
    {
        TextGridCellAt(grid, last + 1, row)->character = ' ';
    }

    TextGridCell cell = {.character = character, .foreground = grid->foreground, .background = grid->background, .style = grid->style};
    *TextGridCellAt(grid, column, row) = cell;

    if (width == 2)
    {
        cell.character = 0;
        *TextGridCellAt(grid, column + 1, row) = cell;
    }
}

/// @brief Moves every row up by one, and empties the last.
static void TextGridScroll(TextGridGrid *grid)
{
    SDL_memmove(grid->cells, grid->cells + grid->columns, (usz)(grid->rows - 1) * (usz)grid->columns * sizeof(TextGridCell));
    SDL_memmove(grid->wrapped, grid->wrapped + 1, (usz)(grid->rows - 1) * sizeof(bool));
    TextGridClearRow(grid, grid->rows - 1);
}

/// @brief Frees a grid; the Destroy function of the handle type textgrid.grid.
static void TextGridDestroy(void *object)
{
    TextGridGrid *grid = object;
    SDL_free(grid->cells);
    SDL_free(grid->wrapped);
    SDL_free(grid);
}

/// @brief Whether a cell is empty as a blank one is: a space on the grid's background, without a style.
static bool TextGridIsBlank(const TextGridGrid *grid, const TextGridCell *cell)
{
    return cell->character == ' ' && cell->background == grid->background && cell->style == 0;
}

/// @brief Where a layout puts the cells of the old grid's lines: the new rows, the first ones of which it skips, so the last ones stay.
typedef struct TextGridLayout
{
    i32 columns;
    i32 skip;            // the rows before the first one that is kept
    i32 rows;            // the rows kept; the layout counts the rows when there are no cells
    TextGridCell *cells; // nullptr to count only
    bool *wrapped;
    TextGridCell blank;
    i32 row;    // the row being written, counted from the first row of the layout
    i32 column; // where the next cell goes in it
} TextGridLayout;

/// @brief Writes a cell, or both halves of a wide character, at the layout's place, if that row is kept.
static void TextGridLayOut(TextGridLayout *layout, const TextGridCell *cell, i32 width)
{
    i32 row = layout->row - layout->skip;

    for (i32 i = 0; layout->cells != nullptr && row >= 0 && row < layout->rows && i < width; i++)
    {
        layout->cells[(usz)row * (usz)layout->columns + (usz)(layout->column + i)] = cell[i];
    }

    layout->column += width;
}

/// @brief Ends a row of the layout; a row that the line goes on from is marked wrapped.
static void TextGridNextRow(TextGridLayout *layout, bool wrapped)
{
    i32 row = layout->row - layout->skip;

    // the rest of the row is blank
    while (layout->column < layout->columns)
    {
        TextGridLayOut(layout, &layout->blank, 1);
    }

    if (layout->wrapped != nullptr && row >= 0 && row < layout->rows)
    {
        layout->wrapped[row] = wrapped;
    }

    layout->row++;
    layout->column = 0;
}

/// @brief Lays the grid's lines out at a width: rows that wrap are joined into their lines, and each line is cut into rows again.
/// The blanks at the end of a line are left out, except before the cursor, whose new place it gives.
/// @return The number of rows the lines take.
static i32 TextGridReflow(TextGridGrid *grid, TextGridLayout *layout, i32 *retCursorRow, i32 *retCursorColumn)
{
    // rows below the cursor and the last written row are blank, and are left out
    i32 last = grid->cursorRow;

    for (i32 row = grid->rows - 1; row > last; row--)
    {
        for (i32 column = 0; column < grid->columns && row > last; column++)
        {
            last = TextGridIsBlank(grid, TextGridCellAt(grid, column, row)) ? last : row;
        }
    }

    *retCursorRow = 0;
    *retCursorColumn = 0;

    for (i32 first = 0; first <= last;)
    {
        // a line is its first row and the rows that its wrapped rows go on to
        i32 end = first;

        while (end < last && grid->wrapped[end])
        {
            end++;
        }

        // its cells are those rows, without the blanks at the end, but with those before the cursor
        usz length = (usz)(end - first + 1) * (usz)grid->columns;
        const TextGridCell *cells = TextGridCellAt(grid, 0, first);

        while (length > 0 && TextGridIsBlank(grid, &cells[length - 1]))
        {
            length--;
        }

        bool cursorHere = grid->cursorRow >= first && grid->cursorRow <= end;
        usz cursor = cursorHere ? (usz)(grid->cursorRow - first) * (usz)grid->columns + (usz)grid->cursorColumn : 0;
        length = cursorHere ? SDL_max(length, SDL_min(cursor, (usz)(end - first + 1) * (usz)grid->columns)) : length;

        for (usz i = 0; i < length;)
        {
            // a wide character moves whole, and is left out where it never fits; a right half without its left is a blank
            i32 width = i + 1 < length && cells[i].character != 0 && cells[i + 1].character == 0 ? 2 : 1;
            const TextGridCell *cell = cells[i].character == 0 ? &layout->blank : &cells[i];

            if (layout->column + width > layout->columns && width <= layout->columns)
            {
                TextGridNextRow(layout, true);
            }

            if (cursorHere && cursor >= i && cursor < i + (usz)width)
            {
                *retCursorRow = layout->row;
                *retCursorColumn = layout->column;
            }

            if (width <= layout->columns)
            {
                TextGridLayOut(layout, cell, width);
            }

            i += (usz)width;
        }

        if (cursorHere && cursor >= length)
        {
            *retCursorRow = layout->row;
            *retCursorColumn = layout->column;
        }

        TextGridNextRow(layout, false);
        first = end + 1;
    }

    return layout->row;
}

/// @brief Gives the cells to a new size. The lines are laid out again at the new width, so a line that no longer fits wraps instead of being cut;
/// when they take more rows than the grid has, the first ones go, so the cursor stays.
/// @return false if out of memory; the grid is unchanged.
static bool TextGridResizeCells(TextGridGrid *grid, i32 columns, i32 rows)
{
    columns = SDL_max(1, columns);
    rows = SDL_max(1, rows);

    if (grid->cells != nullptr && columns == grid->columns && rows == grid->rows)
    {
        return true;
    }

    TextGridCell *cells = SDL_malloc((usz)columns * (usz)rows * sizeof(TextGridCell));
    bool *wrapped = SDL_calloc((usz)rows, sizeof(bool));

    if (cells == nullptr || wrapped == nullptr)
    {
        SDL_free(cells);
        SDL_free(wrapped);
        return false;
    }

    TextGridLayout layout = {.columns = columns, .rows = rows, .blank = TextGridBlank(grid)};
    i32 cursorRow = 0;
    i32 cursorColumn = 0;
    i32 used = 0;

    // the lines are counted first, then written, without the rows that do not fit
    if (grid->cells != nullptr)
    {
        used = TextGridReflow(grid, &layout, &cursorRow, &cursorColumn);
        layout = (TextGridLayout){.columns = columns, .skip = SDL_max(0, used - rows), .rows = rows, .cells = cells, .wrapped = wrapped, .blank = layout.blank};
        TextGridReflow(grid, &layout, &cursorRow, &cursorColumn);
    }

    for (i32 row = SDL_max(0, used - layout.skip); row < rows; row++)
    {
        for (i32 column = 0; column < columns; column++)
        {
            cells[(usz)row * (usz)columns + (usz)column] = layout.blank;
        }
    }

    SDL_free(grid->cells);
    SDL_free(grid->wrapped);
    grid->cells = cells;
    grid->wrapped = wrapped;
    grid->columns = columns;
    grid->rows = rows;
    grid->cursorColumn = SDL_min(cursorColumn, columns);
    grid->cursorRow = SDL_clamp(cursorRow - layout.skip, 0, rows - 1);
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

static void TextGridStyle(TextGridGrid *grid, const char *style)
{
    u8 flags = 0;

    // the style is words with spaces between them
    for (const char *word = style; *word != '\0';)
    {
        usz length = 0;

        while (word[length] != '\0' && word[length] != ' ')
        {
            length++;
        }

        if (length == 4 && SDL_strncmp(word, "bold", 4) == 0)
        {
            flags |= TEXTGRID_BOLD;
        }
        else if (length == 9 && SDL_strncmp(word, "underline", 9) == 0)
        {
            flags |= TEXTGRID_UNDERLINE;
        }
        else if (length > 0)
        {
            ECS_LogWarn(TEXTGRID.plugin, "A grid's style is made of the words bold and underline, not '%.*s'.", (int)length, word);
        }

        word += length + (word[length] == ' ' ? 1 : 0);
    }

    grid->style = flags;
}

static const char *TextGridGet(TextGridGrid *grid, i32 column, i32 row, const char **retStyle)
{
    // the texts stay until the next call, and Lua copies them
    static char character[5];
    static const char *const styles[] = {"", "bold", "underline", "bold underline"};
    character[0] = '\0';
    *retStyle = "";

    if (row >= 0 && row < grid->rows && column >= 0 && column < grid->columns)
    {
        const TextGridCell *cell = TextGridCellAt(grid, column, row);
        usz length = 0;

        if (cell->character != 0)
        {
            TextGridAppend(character, &length, cell->character);
        }

        *retStyle = styles[cell->style & (TEXTGRID_BOLD | TEXTGRID_UNDERLINE)];
    }

    return character;
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

    // a wide character that does not fit at the edge is left out; the text stops at the edge, so the column never overflows
    for (u32 character = SDL_StepUTF8(&text, &length); character != 0 && column < grid->columns; character = SDL_StepUTF8(&text, &length))
    {
        i32 width = TextGridWidth(character);

        if (row >= 0 && row < grid->rows && column >= 0 && width <= grid->columns - column)
        {
            TextGridSet(grid, column, row, character, width);
        }

        column += width;
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

        // a wide character is left out of a grid of one column, and the cursor stays
        i32 width = character == '\t' ? 1 : TextGridWidth(character);

        if (width > grid->columns)
        {
            continue;
        }

        // a line ends at a new line; when the character does not fit in the rest of the row, the line goes on in the next row
        if (character == '\n' || grid->cursorColumn + width > grid->columns)
        {
            grid->wrapped[grid->cursorRow] = character != '\n';
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

        TextGridSet(grid, grid->cursorColumn, grid->cursorRow, character, width);
        grid->cursorColumn += width;
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

static void TextGridDraw(ECSSurface *surface, TextGridGrid *grid, f32 x, f32 y, f32 size, bool cursor)
{
    // a cell is measured at the size the surface draws at, so the characters of a run stay on their cells
    f32 scale = surface->scale > 0.0f ? surface->scale : 1.0f;
    f32 cellWidth = 0.0f;
    f32 cellHeight = 0.0f;
    TextGridCellSize(size * scale, &cellWidth, &cellHeight);
    cellWidth /= scale;
    cellHeight /= scale;

    // a run of a row's cells in one colour and style is drawn at once: its backgrounds in one fill, and its characters in one text
    char *text = SDL_malloc((usz)grid->columns * 4 + 1);
    f32 underline = SDL_max(1.0f / scale, cellHeight / 16.0f);

    for (i32 row = 0; text != nullptr && row < grid->rows; row++)
    {
        f32 top = y + (f32)row * cellHeight;

        for (i32 start = 0; start < grid->columns;)
        {
            TextGridCell *first = TextGridCellAt(grid, start, row);
            i32 end = start;
            usz length = 0;
            text[0] = '\0';

            // a wide character is drawn alone over its two cells, so the font's width for it does not move the characters after it
            if (TextGridIsWide(grid, start, row))
            {
                TextGridAppend(text, &length, first->character);
                end = start + 2;
            }

            while (end < grid->columns && !TextGridIsWide(grid, start, row))
            {
                const TextGridCell *cell = TextGridCellAt(grid, end, row);

                if (end > start && (TextGridIsWide(grid, end, row) || cell->background != first->background || cell->foreground != first->foreground || cell->style != first->style))
                {
                    break;
                }

                // the right half of a wide character whose left half is gone is a space
                TextGridAppend(text, &length, cell->character == 0 ? ' ' : cell->character);
                end++;
            }

            f32 left = x + (f32)start * cellWidth;
            f32 width = (f32)(end - start) * cellWidth;
            TEXTGRID.Fill(surface, left, top, width, cellHeight, first->background);
            TEXTGRID.Text(surface, TEXTGRID.font, text, left, top, size, first->foreground);

            // the monospace font has no bold face, so bold text is drawn again a pixel to the right
            if (first->style & TEXTGRID_BOLD)
            {
                TEXTGRID.Text(surface, TEXTGRID.font, text, left + 1.0f / scale, top, size, first->foreground);
            }

            if (first->style & TEXTGRID_UNDERLINE)
            {
                TEXTGRID.Fill(surface, left, top + cellHeight - 2.0f * underline, width, underline, first->foreground);
            }

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
        {"resize", (ECSFunction)TextGridResize, "void(handle<textgrid.grid> grid, int columns, int rows)", "Change a grid's size; its lines wrap again at the new width, and the first rows go when they do not fit"},
        {"size", (ECSFunction)TextGridSize, "void(handle<textgrid.grid> grid, out int columns, out int rows)", "Give a grid's size"},
        {"clear", (ECSFunction)TextGridClear, "void(handle<textgrid.grid> grid)", "Empty every cell, and move the cursor to the top left"},
        {"style", (ECSFunction)TextGridStyle, "void(handle<textgrid.grid> grid, string style)", "Set the style that later writing uses: the words bold and underline, or an empty text for none"},
        {"get", (ECSFunction)TextGridGet, "string(handle<textgrid.grid> grid, int column, int row, out string style)", "Give the character and the style of a cell; the right half of a wide character, and a cell outside the grid, give an empty text"},
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

    // Lua calls these on a handle, such as handle:print(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"grid", "print", "print"},
        {"grid", "put", "put"},
        {"grid", "clear", "clear"},
        {"grid", "style", "style"},
        {"grid", "colors", "colors"},
        {"grid", "cursor", "cursor"},
        {"grid", "setCursor", "setCursor"},
        {"grid", "resize", "resize"},
        {"grid", "size", "size"},
        {"grid", "get", "get"},
        {"grid", "free", "free"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function));
    }

    return SHUResult_Ok;
}
