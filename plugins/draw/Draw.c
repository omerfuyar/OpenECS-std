// The draw standard plugin: draws rectangles, text and images into the pixels surface of another plugin's panel (DESIGN 2).
// It is built like a third-party plugin, against the plugin interface; it calls SDL3 and SDL3_ttf itself, using the executable's copy.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

#pragma region Source Only

/// @brief A name of the plugin: "draw." and a local name.
#define DRAW_NAME(localName) "draw." localName

/// @brief The colour of a mistake, so it shows: opaque magenta.
#define DRAW_BAD_COLOR 0xFFFF00FF

/// @brief The clip rectangle of a surface, in pixels.
typedef struct DrawClip
{
    const ECSSurface *surface;
    SDL_Rect rect;
} DrawClip;

/// @brief A font at one size, in pixels.
typedef struct DrawFont
{
    i32 size;
    TTF_Font *font;
} DrawFont;

/// @brief An image file, read once.
typedef struct DrawImageFile
{
    char *path;
    SDL_Surface *surface; // NULL if the file cannot be read, so it is reported once
} DrawImageFile;

static struct
{
    ECSPlugin plugin;
    char *fontPath;
    DrawFont *fonts; // every size asked for so far, opened once
    usz fontCount;
    DrawImageFile *images; // every image asked for so far
    usz imageCount;
    DrawClip *clips; // the surfaces that have a clip rectangle
    usz clipCount;
    TTF_TextEngine *engine; // draws text into surfaces, and keeps the glyphs it drew
    bool ttfStarted;
} DRAW = {0};

/// @brief Gets the font at a size in pixels, opening it the first time.
/// @return The font, or NULL if it cannot be opened; the reason is logged.
static TTF_Font *DrawGetFont(f32 pixels)
{
    i32 size = (i32)SDL_max(1.0f, SDL_roundf(pixels));

    for (usz i = 0; i < DRAW.fontCount; i++)
    {
        if (DRAW.fonts[i].size == size)
        {
            return DRAW.fonts[i].font;
        }
    }

    TTF_Font *font = TTF_OpenFont(DRAW.fontPath, (float)size);
    DrawFont *fonts = font == NULL ? NULL : SDL_realloc(DRAW.fonts, (DRAW.fontCount + 1) * sizeof(DrawFont));

    if (fonts == NULL)
    {
        ECS_Log(DRAW.plugin, ECSLogLevel_Error, "Cannot open the font '%s' at %d pixels: %s", DRAW.fontPath, size, SDL_GetError());
        TTF_CloseFont(font);
        return NULL;
    }

    DRAW.fonts = fonts;
    DRAW.fonts[DRAW.fontCount++] = (DrawFont){.size = size, .font = font};
    return font;
}

/// @brief Wraps a panel's pixels in an SDL surface, for one call.
/// @return The surface, or NULL; free it with SDL_DestroySurface.
static SDL_Surface *DrawWrap(ECSSurface *surface)
{
    if (surface->type != ECSSurfaceType_Pixels)
    {
        ECS_Log(DRAW.plugin, ECSLogLevel_Warning, "ui draws only into pixels surfaces.");
        return NULL;
    }

    SDL_Surface *target = SDL_CreateSurfaceFrom(surface->width, surface->height, SDL_PIXELFORMAT_ARGB8888, surface->pixels.data, surface->pitch);

    for (usz i = 0; target != NULL && i < DRAW.clipCount; i++)
    {
        if (DRAW.clips[i].surface == surface)
        {
            SDL_SetSurfaceClipRect(target, &DRAW.clips[i].rect);
        }
    }

    return target;
}

/// @brief Fills a rectangle in pixels with an ARGB colour, blending by its alpha.
static void DrawFillPixels(SDL_Surface *target, SDL_Rect rect, u32 argb)
{
    SDL_Rect bounds = {0};
    SDL_GetSurfaceClipRect(target, &bounds);
    u32 alpha = argb >> 24;

    if (alpha == 0xFF)
    {
        SDL_FillSurfaceRect(target, &rect, argb);
        return;
    }

    if (alpha == 0 || !SDL_GetRectIntersection(&rect, &bounds, &rect))
    {
        return;
    }

    // panels are opaque, so each channel blends and the alpha stays
    for (int row = rect.y; row < rect.y + rect.h; row++)
    {
        u32 *pixel = (u32 *)((u8 *)target->pixels + (usz)row * (usz)target->pitch) + rect.x;

        for (int column = 0; column < rect.w; column++, pixel++)
        {
            u32 blended = *pixel & 0xFF000000;

            for (u32 shift = 0; shift < 24; shift += 8)
            {
                u32 source = (argb >> shift) & 0xFF;
                u32 destination = (*pixel >> shift) & 0xFF;
                blended |= ((source * alpha + destination * (255 - alpha)) / 255) << shift;
            }

            *pixel = blended;
        }
    }
}

/// @brief Turns a rectangle in layout units into pixels.
static SDL_Rect DrawPixels(f32 scale, f32 x, f32 y, f32 width, f32 height)
{
    return (SDL_Rect){(int)SDL_roundf(x * scale), (int)SDL_roundf(y * scale), (int)SDL_roundf(width * scale), (int)SDL_roundf(height * scale)};
}

/// @brief Draws text with its line's top left at a position in layout units, inside the target's clip rectangle.
/// @return The text's width in layout units, or 0 if it was not drawn.
static f32 DrawDrawText(SDL_Surface *target, f32 scale, const char *text, f32 x, f32 y, f32 size, i64 color)
{
    TTF_Font *font = scale <= 0.0f ? NULL : DrawGetFont(size * scale);
    TTF_Text *drawn = font == NULL ? NULL : TTF_CreateText(DRAW.engine, font, text, 0);
    int width = 0;
    u32 argb = (u32)color;

    if (drawn == NULL)
    {
        return 0.0f;
    }

    TTF_SetTextColor(drawn, (Uint8)(argb >> 16), (Uint8)(argb >> 8), (Uint8)argb, (Uint8)(argb >> 24));
    TTF_DrawSurfaceText(drawn, (int)SDL_roundf(x * scale), (int)SDL_roundf(y * scale), target);
    TTF_GetTextSize(drawn, &width, NULL);
    TTF_DestroyText(drawn);
    return (f32)width / scale;
}

/// @brief Fills a rectangle, in layout units, with an ARGB colour, blending by its alpha.
static void DrawFill(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, i64 color)
{
    SDL_Surface *target = DrawWrap(surface);

    if (target != NULL)
    {
        DrawFillPixels(target, DrawPixels(surface->scale, x, y, width, height), (u32)color);
        SDL_DestroySurface(target);
    }
}

/// @brief Draws the edge of a rectangle, in layout units, with a thickness and a colour.
static void DrawOutline(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, f32 thickness, i64 color)
{
    SDL_Surface *target = DrawWrap(surface);

    if (target == NULL)
    {
        return;
    }

    SDL_Rect rect = DrawPixels(surface->scale, x, y, width, height);
    int edge = SDL_max(1, (int)SDL_roundf(thickness * surface->scale));
    edge = SDL_min(edge, SDL_min(rect.w, rect.h) / 2 + 1);

    DrawFillPixels(target, (SDL_Rect){rect.x, rect.y, rect.w, edge}, (u32)color);
    DrawFillPixels(target, (SDL_Rect){rect.x, rect.y + rect.h - edge, rect.w, edge}, (u32)color);
    DrawFillPixels(target, (SDL_Rect){rect.x, rect.y + edge, edge, rect.h - 2 * edge}, (u32)color);
    DrawFillPixels(target, (SDL_Rect){rect.x + rect.w - edge, rect.y + edge, edge, rect.h - 2 * edge}, (u32)color);
    SDL_DestroySurface(target);
}

/// @brief Draws text with its line's top left at a position, in layout units.
/// @return The text's width in layout units, or 0 if it was not drawn.
static f32 DrawText(ECSSurface *surface, const char *text, f32 x, f32 y, f32 size, i64 color)
{
    SDL_Surface *target = DrawWrap(surface);
    f32 width = target == NULL ? 0.0f : DrawDrawText(target, surface->scale, text, x, y, size, color);

    SDL_DestroySurface(target);
    return width;
}

/// @brief Gives the width and the line height of a text in a size, without drawing it, in layout units.
static void DrawMeasure(const char *text, f32 size, f32 *retWidth, f32 *retHeight)
{
    TTF_Font *font = DrawGetFont(size);
    int width = 0;

    if (font != NULL)
    {
        TTF_GetStringSize(font, text, 0, &width, NULL);
    }

    *retWidth = (f32)width;
    *retHeight = font == NULL ? 0.0f : (f32)TTF_GetFontHeight(font);
}

/// @brief Reads "#RRGGBB" or "#RRGGBBAA" as an ARGB colour.
/// @return true if the text is a colour.
static bool DrawParseColor(const char *text, u32 *retColor)
{
    usz length = SDL_strlen(text);
    char *end = NULL;

    if (text[0] != '#' || (length != 7 && length != 9))
    {
        return false;
    }

    u32 value = (u32)SDL_strtoul(text + 1, &end, 16);

    if (*end != '\0')
    {
        return false;
    }

    *retColor = length == 7 ? 0xFF000000 | value : (value >> 8) | (value << 24);
    return true;
}

/// @brief Reads a colour: "#RRGGBB", "#RRGGBBAA", or the name of a colour of the core's theme, such as "text" for ecs.colorText.
/// @return The ARGB colour, or opaque magenta if it cannot be read.
static i64 DrawColor(const char *name)
{
    u32 color = DRAW_BAD_COLOR;
    char *setting = NULL;

    if (DrawParseColor(name, &color))
    {
        return color;
    }

    // a theme colour is the core's setting ecs.color and the name with a capital
    if (name[0] != '\0' && SDL_asprintf(&setting, "ecs.color%c%s", SDL_toupper((unsigned char)name[0]), name + 1) >= 0 &&
        DrawParseColor(ECSValue_GetString(ECSSetting_Get(setting), ""), &color))
    {
        SDL_free(setting);
        return color;
    }

    ECS_Log(DRAW.plugin, ECSLogLevel_Warning, "'%s' is neither a colour nor a colour of the theme.", name);
    SDL_free(setting);
    return DRAW_BAD_COLOR;
}

/// @brief Gets an image file, reading it the first time; a relative path starts at the executable's folder.
/// @return The image in ARGB, or NULL if it cannot be read; the reason is logged the first time.
static SDL_Surface *DrawGetImage(const char *path)
{
    for (usz i = 0; i < DRAW.imageCount; i++)
    {
        if (SDL_strcmp(DRAW.images[i].path, path) == 0)
        {
            return DRAW.images[i].surface;
        }
    }

    char *full = NULL;
    DrawImageFile *images = SDL_realloc(DRAW.images, (DRAW.imageCount + 1) * sizeof(DrawImageFile));

    if (images == NULL || SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
    {
        DRAW.images = images == NULL ? DRAW.images : images;
        ECS_Log(DRAW.plugin, ECSLogLevel_Error, "Cannot read the image '%s': out of memory.", path);
        return NULL;
    }

    DRAW.images = images;
    SDL_Surface *loaded = SDL_LoadSurface(full);
    SDL_Surface *image = loaded == NULL ? NULL : SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);

    if (image == NULL)
    {
        ECS_Log(DRAW.plugin, ECSLogLevel_Error, "Cannot read the image '%s': %s", full, SDL_GetError());
    }

    SDL_DestroySurface(loaded);
    SDL_free(full);
    DRAW.images[DRAW.imageCount++] = (DrawImageFile){.path = SDL_strdup(path), .surface = image};

    if (DRAW.images[DRAW.imageCount - 1].path == NULL)
    {
        SDL_DestroySurface(image);
        DRAW.imageCount--;
        return NULL;
    }

    return image;
}

/// @brief Draws an image file, PNG, JPG or BMP, stretched to a rectangle in layout units.
/// @return true if it was drawn.
static bool DrawImage(ECSSurface *surface, const char *path, f32 x, f32 y, f32 width, f32 height)
{
    SDL_Surface *image = DrawGetImage(path);
    SDL_Surface *target = image == NULL ? NULL : DrawWrap(surface);

    if (target == NULL)
    {
        return false;
    }

    SDL_Rect rect = DrawPixels(surface->scale, x, y, width, height);
    bool drawn = SDL_BlitSurfaceScaled(image, NULL, target, &rect, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(target);
    return drawn;
}

/// @brief Gives an image file's size in pixels.
/// @return true if the file can be read.
static bool DrawImageSize(const char *path, f32 *retWidth, f32 *retHeight)
{
    SDL_Surface *image = DrawGetImage(path);

    *retWidth = image == NULL ? 0.0f : (f32)image->w;
    *retHeight = image == NULL ? 0.0f : (f32)image->h;
    return image != NULL;
}

/// @brief Limits the later drawing into a surface to a rectangle, in layout units, until DrawUnclip. A new clip replaces the old one.
static void DrawSetClip(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height)
{
    SDL_Rect rect = DrawPixels(surface->scale, x, y, width, height);

    for (usz i = 0; i < DRAW.clipCount; i++)
    {
        if (DRAW.clips[i].surface == surface)
        {
            DRAW.clips[i].rect = rect;
            return;
        }
    }

    DrawClip *clips = SDL_realloc(DRAW.clips, (DRAW.clipCount + 1) * sizeof(DrawClip));

    if (clips == NULL)
    {
        ECS_Log(DRAW.plugin, ECSLogLevel_Error, "Cannot clip a surface: out of memory.");
        return;
    }

    DRAW.clips = clips;
    DRAW.clips[DRAW.clipCount++] = (DrawClip){.surface = surface, .rect = rect};
}

/// @brief Lets drawing into a surface reach all of it again.
static void DrawUnclip(ECSSurface *surface)
{
    for (usz i = 0; i < DRAW.clipCount; i++)
    {
        if (DRAW.clips[i].surface == surface)
        {
            DRAW.clips[i] = DRAW.clips[--DRAW.clipCount];
            return;
        }
    }
}

/// @brief Frees the fonts and the text engine, and stops SDL_ttf.
static void DrawFree(void)
{
    for (usz i = 0; i < DRAW.fontCount; i++)
    {
        TTF_CloseFont(DRAW.fonts[i].font);
    }

    for (usz i = 0; i < DRAW.imageCount; i++)
    {
        SDL_free(DRAW.images[i].path);
        SDL_DestroySurface(DRAW.images[i].surface);
    }

    if (DRAW.engine != NULL)
    {
        TTF_DestroySurfaceTextEngine(DRAW.engine);
    }

    // SDL_ttf counts its starts, so the core's own use goes on
    if (DRAW.ttfStarted)
    {
        TTF_Quit();
    }

    SDL_free(DRAW.fonts);
    SDL_free(DRAW.images);
    SDL_free(DRAW.clips);
    SDL_free(DRAW.fontPath);
    SDL_zero(DRAW);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    DRAW.plugin = plugin;

    // the core's font; a relative path starts at the executable's folder
    const char *font = ECSValue_GetString(ECSSetting_Get("ecs.font"), "");

    if (SDL_asprintf(&DRAW.fontPath, "%s%s", font[0] == '/' ? "" : SDL_GetBasePath(), font) < 0)
    {
        DRAW.fontPath = NULL;
        DrawFree();
        return SHUResult_ErrAllocation;
    }

    DRAW.ttfStarted = TTF_Init();
    DRAW.engine = DRAW.ttfStarted ? TTF_CreateSurfaceTextEngine() : NULL;

    if (DRAW.engine == NULL)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "Cannot start SDL_ttf: %s", SDL_GetError());
        DrawFree();
        return SHUResult_ErrInternal;
    }

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {DRAW_NAME("fill"), (ECSFunction)DrawFill, "void(handle<ecs.surface> surface, float x, float y, float width, float height, int64 color)", "Fill a rectangle with a colour"},
        {DRAW_NAME("text"), (ECSFunction)DrawText, "float(handle<ecs.surface> surface, string text, float x, float y, float size, int64 color)", "Draw text and give its width"},
        {DRAW_NAME("measure"), (ECSFunction)DrawMeasure, "void(string text, float size, out float width, out float lineHeight)", "Give the width and line height of a text"},
        {DRAW_NAME("color"), (ECSFunction)DrawColor, "int64(string color)", "Read a colour, or a colour of the theme by name"},
        {DRAW_NAME("outline"), (ECSFunction)DrawOutline, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float thickness, int64 color)", "Draw the edge of a rectangle"},
        {DRAW_NAME("image"), (ECSFunction)DrawImage, "bool(handle<ecs.surface> surface, string path, float x, float y, float width, float height)", "Draw an image file stretched to a rectangle"},
        {DRAW_NAME("clip"), (ECSFunction)DrawSetClip, "void(handle<ecs.surface> surface, float x, float y, float width, float height)", "Limit later drawing into a surface to a rectangle"},
        {DRAW_NAME("unclip"), (ECSFunction)DrawUnclip, "void(handle<ecs.surface> surface)", "Let drawing reach the whole surface again"},
        {DRAW_NAME("imageSize"), (ECSFunction)DrawImageSize, "bool(string path, out float width, out float height)", "Give an image file's size in pixels"},
    };

    // a plugin whose Init fails gets no Shutdown, so it cleans up here
    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), DrawFree(););
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
    DrawFree();
}
