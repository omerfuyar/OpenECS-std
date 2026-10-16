// The draw standard plugin: draws shapes, images and text into the pixels surface of another plugin's panel (DESIGN 2).
// It knows the primitives of 2D drawing; loaders, such as image, turn files into them. It calls SDL3 and SDL3_ttf itself, using the executable's copy.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

#include <float.h>

#pragma region Source Only

/// @brief The monospace font draw ships with, in its folder.
#define DRAW_MONO_FONT "fonts/RobotoMono-Regular.ttf"

/// @brief A font file at one size, in pixels.
typedef struct DrawSize
{
    i32 pixels;
    TTF_Font *font;
} DrawSize;

/// @brief A font: a TrueType file, opened at each size asked for. Its handle, draw.font, lives as long as the plugin.
typedef struct DrawFont
{
    char *name; // the name it was asked by: "ui", "mono" or a path
    char *path; // the file
    DrawSize *sizes;
    usz sizeCount;
} DrawFont;

/// @brief A picture that draw can draw: pixels in ARGB8888, not premultiplied. Its handle is draw.image.
typedef struct DrawImage
{
    SDL_Surface *surface;
} DrawImage;

/// @brief The clip rectangle of a surface, in pixels.
typedef struct DrawClip
{
    const ECSSurface *surface;
    SDL_Rect rect;
} DrawClip;

/// @brief A point in pixels.
typedef struct DrawPoint
{
    f32 x;
    f32 y;
} DrawPoint;

static struct
{
    ECSPlugin plugin;
    DrawFont **fonts; // every font asked for so far
    usz fontCount;
    DrawClip *clips; // the surfaces that have a clip rectangle
    usz clipCount;
    TTF_TextEngine *engine; // draws text into surfaces, and keeps the glyphs it drew
    bool ttfStarted;
} DRAW = {0};

#pragma region Pixels

/// @brief Gets the rectangle of a surface that drawing reaches: the whole surface, or its clip rectangle.
static SDL_Rect DrawBounds(const ECSSurface *surface)
{
    SDL_Rect bounds = {0, 0, surface->width, surface->height};

    for (usz i = 0; i < DRAW.clipCount; i++)
    {
        if (DRAW.clips[i].surface == surface && !SDL_GetRectIntersection(&bounds, &DRAW.clips[i].rect, &bounds))
        {
            bounds = (SDL_Rect){0};
        }
    }

    return bounds;
}

/// @brief Blends a colour over a pixel; surfaces hold premultiplied colours.
static void DrawBlend(u32 *pixel, ECSColor color)
{
    u32 alpha = color >> 24;

    if (alpha == 0xFF)
    {
        *pixel = color;
        return;
    }

    u32 blended = 0;

    for (u32 shift = 0; shift < 32; shift += 8)
    {
        u32 source = shift == 24 ? alpha : ((color >> shift) & 0xFF) * alpha / 255;
        u32 destination = (*pixel >> shift) & 0xFF;
        blended |= (source + destination * (255 - alpha) / 255) << shift;
    }

    *pixel = blended;
}

/// @brief Blends a colour over a pixel inside the bounds, with its alpha multiplied by how much of the pixel a shape covers, from 0 to 1.
static void DrawCover(const ECSSurface *surface, SDL_Rect bounds, i32 column, i32 row, ECSColor color, f32 coverage)
{
    if (coverage <= 0.0f || column < bounds.x || column >= bounds.x + bounds.w || row < bounds.y || row >= bounds.y + bounds.h)
    {
        return;
    }

    u32 alpha = (u32)SDL_roundf((f32)(color >> 24) * SDL_min(coverage, 1.0f));
    u32 *pixel = (u32 *)((u8 *)surface->pixels.data + (usz)row * (usz)surface->pitch) + column;
    DrawBlend(pixel, (color & 0x00FFFFFF) | (alpha << 24));
}

/// @brief A distance from the edge of a shape to a pixel's centre, in pixels, negative inside; it gives the shape's coverage of the pixel.
typedef f32 (*DrawDistanceFunction)(const void *shape, f32 x, f32 y);

/// @brief Draws a shape with smooth edges: each pixel of a rectangle that holds it gets the part of it the shape covers, from its distance to the edge.
static void DrawSmooth(const ECSSurface *surface, SDL_Rect bounds, SDL_Rect area, DrawDistanceFunction Distance, const void *shape, ECSColor color)
{
    i32 left = SDL_max(area.x, bounds.x);
    i32 right = SDL_min(area.x + area.w, bounds.x + bounds.w);
    i32 top = SDL_max(area.y, bounds.y);
    i32 bottom = SDL_min(area.y + area.h, bounds.y + bounds.h);

    for (i32 row = top; row < bottom; row++)
    {
        for (i32 column = left; column < right; column++)
        {
            DrawCover(surface, bounds, column, row, color, 0.5f - Distance(shape, (f32)column + 0.5f, (f32)row + 0.5f));
        }
    }
}

/// @brief A rectangle with rounded corners, or a ring of one, in pixels.
typedef struct DrawRounded
{
    f32 cx, cy;         // the centre
    f32 halfW, halfH;   // half the size
    f32 radius;         // of the corners
    f32 thickness;      // of a ring, or 0 for the whole shape
} DrawRounded;

static f32 DrawRoundedDistance(const void *shape, f32 x, f32 y)
{
    const DrawRounded *r = shape;
    f32 qx = SDL_fabsf(x - r->cx) - (r->halfW - r->radius);
    f32 qy = SDL_fabsf(y - r->cy) - (r->halfH - r->radius);
    f32 outside = SDL_sqrtf(SDL_max(qx, 0.0f) * SDL_max(qx, 0.0f) + SDL_max(qy, 0.0f) * SDL_max(qy, 0.0f));
    f32 distance = outside + SDL_min(SDL_max(qx, qy), 0.0f) - r->radius;

    // a ring is the band from the edge inwards by its thickness
    return r->thickness > 0.0f ? SDL_fabsf(distance + r->thickness / 2.0f) - r->thickness / 2.0f : distance;
}

/// @brief A line of a thickness between two points, with flat ends, in pixels.
typedef struct DrawStroke
{
    DrawPoint a;
    DrawPoint b;
    f32 half; // half the thickness
} DrawStroke;

static f32 DrawStrokeDistance(const void *shape, f32 x, f32 y)
{
    const DrawStroke *s = shape;
    f32 dx = s->b.x - s->a.x;
    f32 dy = s->b.y - s->a.y;
    f32 length = SDL_sqrtf(dx * dx + dy * dy);
    f32 along = ((x - s->a.x) * dx + (y - s->a.y) * dy) / length;
    f32 across = SDL_fabsf((x - s->a.x) * dy - (y - s->a.y) * dx) / length;

    // the distance to the band across the line, and to the band along it, whichever is farther
    return SDL_max(across - s->half, SDL_fabsf(along - length / 2.0f) - length / 2.0f);
}

/// @brief Blends a colour over a row of pixels, from one column up to another, without it, inside the bounds.
static void DrawSpan(const ECSSurface *surface, SDL_Rect bounds, i32 row, i32 from, i32 to, ECSColor color)
{
    from = SDL_max(from, bounds.x);
    to = SDL_min(to, bounds.x + bounds.w);

    if (row < bounds.y || row >= bounds.y + bounds.h || from >= to || (color >> 24) == 0)
    {
        return;
    }

    u32 *pixel = (u32 *)((u8 *)surface->pixels.data + (usz)row * (usz)surface->pitch);

    for (i32 column = from; column < to; column++)
    {
        DrawBlend(&pixel[column], color);
    }
}

/// @brief Fills a rectangle in pixels.
static void DrawRect(const ECSSurface *surface, SDL_Rect bounds, SDL_Rect rect, ECSColor color)
{
    for (i32 row = rect.y; row < rect.y + rect.h; row++)
    {
        DrawSpan(surface, bounds, row, rect.x, rect.x + rect.w, color);
    }
}

/// @brief Fills a triangle in pixels: each pixel whose centre is inside it.
static void DrawTriangle(const ECSSurface *surface, SDL_Rect bounds, DrawPoint a, DrawPoint b, DrawPoint c, ECSColor color)
{
    f32 top = SDL_min(a.y, SDL_min(b.y, c.y));
    f32 bottom = SDL_max(a.y, SDL_max(b.y, c.y));
    i32 first = SDL_max(bounds.y, (i32)SDL_ceilf(top - 0.5f));
    i32 last = SDL_min(bounds.y + bounds.h, (i32)SDL_ceilf(bottom - 0.5f));
    const DrawPoint edges[3][2] = {{a, b}, {b, c}, {c, a}};

    // each row is crossed by two edges; the span between them is filled
    for (i32 row = first; row < last; row++)
    {
        f32 y = (f32)row + 0.5f;
        f32 left = FLT_MAX;
        f32 right = -FLT_MAX;

        for (usz i = 0; i < 3; i++)
        {
            DrawPoint p = edges[i][0];
            DrawPoint q = edges[i][1];

            if ((p.y <= y && q.y > y) || (q.y <= y && p.y > y))
            {
                f32 x = p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y);
                left = SDL_min(left, x);
                right = SDL_max(right, x);
            }
        }

        if (left <= right)
        {
            DrawSpan(surface, bounds, row, (i32)SDL_ceilf(left - 0.5f), (i32)SDL_ceilf(right - 0.5f), color);
        }
    }
}

/// @brief Draws a line in pixels, with smooth edges.
static void DrawSegment(const ECSSurface *surface, SDL_Rect bounds, DrawPoint a, DrawPoint b, f32 thickness, ECSColor color)
{
    if (a.x == b.x && a.y == b.y)
    {
        return;
    }

    // the line is at least a pixel thick, so a thin line never disappears
    DrawStroke stroke = {a, b, SDL_max(thickness, 1.0f) / 2.0f};
    f32 reach = stroke.half + 1.0f;
    SDL_Rect area = {(i32)SDL_floorf(SDL_min(a.x, b.x) - reach), (i32)SDL_floorf(SDL_min(a.y, b.y) - reach), 0, 0};
    area.w = (i32)SDL_ceilf(SDL_max(a.x, b.x) + reach) - area.x;
    area.h = (i32)SDL_ceilf(SDL_max(a.y, b.y) + reach) - area.y;
    DrawSmooth(surface, bounds, area, DrawStrokeDistance, &stroke, color);
}

/// @brief Draws a rectangle with rounded corners in pixels, or a ring of one, with smooth edges.
static void DrawRoundedPixels(const ECSSurface *surface, SDL_Rect bounds, f32 x, f32 y, f32 width, f32 height, f32 radius, f32 thickness, ECSColor color)
{
    if (width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    DrawRounded shape = {x + width / 2.0f, y + height / 2.0f, width / 2.0f, height / 2.0f, SDL_clamp(radius, 0.0f, SDL_min(width, height) / 2.0f), thickness};
    SDL_Rect area = {(i32)SDL_floorf(x) - 1, (i32)SDL_floorf(y) - 1, 0, 0};
    area.w = (i32)SDL_ceilf(x + width) + 1 - area.x;
    area.h = (i32)SDL_ceilf(y + height) + 1 - area.y;
    DrawSmooth(surface, bounds, area, DrawRoundedDistance, &shape, color);
}

/// @brief Turns a rectangle in layout units into pixels.
static SDL_Rect DrawPixels(f32 scale, f32 x, f32 y, f32 width, f32 height)
{
    return (SDL_Rect){(int)SDL_roundf(x * scale), (int)SDL_roundf(y * scale), (int)SDL_roundf(width * scale), (int)SDL_roundf(height * scale)};
}

/// @brief Checks that draw can draw into a surface.
static bool DrawCanDraw(const ECSSurface *surface)
{
    if (surface->type != ECSSurfaceType_Pixels)
    {
        ECS_LogWarn(DRAW.plugin, "draw draws only into pixels surfaces.");
        return false;
    }

    return true;
}

#pragma endregion Pixels

#pragma region Shapes

static void DrawFill(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, ECSColor color)
{
    if (DrawCanDraw(surface))
    {
        DrawRect(surface, DrawBounds(surface), DrawPixels(surface->scale, x, y, width, height), color);
    }
}

static void DrawOutline(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, f32 thickness, ECSColor color)
{
    if (!DrawCanDraw(surface))
    {
        return;
    }

    SDL_Rect bounds = DrawBounds(surface);
    SDL_Rect rect = DrawPixels(surface->scale, x, y, width, height);
    int edge = SDL_max(1, (int)SDL_roundf(thickness * surface->scale));
    edge = SDL_min(edge, SDL_min(rect.w, rect.h) / 2 + 1);

    DrawRect(surface, bounds, (SDL_Rect){rect.x, rect.y, rect.w, edge}, color);
    DrawRect(surface, bounds, (SDL_Rect){rect.x, rect.y + rect.h - edge, rect.w, edge}, color);
    DrawRect(surface, bounds, (SDL_Rect){rect.x, rect.y + edge, edge, rect.h - 2 * edge}, color);
    DrawRect(surface, bounds, (SDL_Rect){rect.x + rect.w - edge, rect.y + edge, edge, rect.h - 2 * edge}, color);
}

static void DrawLine(ECSSurface *surface, f32 x1, f32 y1, f32 x2, f32 y2, f32 thickness, ECSColor color)
{
    if (DrawCanDraw(surface))
    {
        f32 scale = surface->scale;
        DrawSegment(surface, DrawBounds(surface), (DrawPoint){x1 * scale, y1 * scale}, (DrawPoint){x2 * scale, y2 * scale}, thickness * scale, color);
    }
}

// a circle is a square whose corners are rounded by half its size
static void DrawCircle(ECSSurface *surface, f32 x, f32 y, f32 radius, f32 thickness, ECSColor color)
{
    if (DrawCanDraw(surface))
    {
        f32 scale = surface->scale;
        f32 r = radius * scale;
        DrawRoundedPixels(surface, DrawBounds(surface), x * scale - r, y * scale - r, 2.0f * r, 2.0f * r, r, SDL_max(thickness, 0.0f) * scale, color);
    }
}

static void DrawRoundedFill(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, f32 radius, ECSColor color)
{
    if (DrawCanDraw(surface))
    {
        f32 scale = surface->scale;
        DrawRoundedPixels(surface, DrawBounds(surface), x * scale, y * scale, width * scale, height * scale, radius * scale, 0.0f, color);
    }
}

static void DrawRoundedOutline(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, f32 radius, f32 thickness, ECSColor color)
{
    if (DrawCanDraw(surface))
    {
        f32 scale = surface->scale;
        DrawRoundedPixels(surface, DrawBounds(surface), x * scale, y * scale, width * scale, height * scale, radius * scale, SDL_max(thickness * scale, 1.0f), color);
    }
}

/// @brief Reads a point of a buffer of floats, x and y pairs in layout units, as pixels.
static DrawPoint DrawPointAt(SHUSlice points, usz index, f32 scale)
{
    const f32 *values = points.data;
    return (DrawPoint){values[2 * index] * scale, values[2 * index + 1] * scale};
}

static void DrawTriangles(ECSSurface *surface, SHUSlice points, SHUSlice indices, ECSColor color)
{
    if (!DrawCanDraw(surface))
    {
        return;
    }

    SDL_Rect bounds = DrawBounds(surface);
    usz pointCount = points.size / (2 * sizeof(f32));
    usz indexCount = indices.size / sizeof(u32);
    const u32 *index = indices.data;

    // without indices, the points go in threes
    usz count = indexCount > 0 ? indexCount / 3 : pointCount / 3;

    for (usz i = 0; i < count; i++)
    {
        usz a = indexCount > 0 ? index[3 * i] : 3 * i;
        usz b = indexCount > 0 ? index[3 * i + 1] : 3 * i + 1;
        usz c = indexCount > 0 ? index[3 * i + 2] : 3 * i + 2;

        if (a < pointCount && b < pointCount && c < pointCount)
        {
            DrawTriangle(surface, bounds, DrawPointAt(points, a, surface->scale), DrawPointAt(points, b, surface->scale), DrawPointAt(points, c, surface->scale), color);
        }
    }
}

static void DrawLines(ECSSurface *surface, SHUSlice points, SHUSlice indices, f32 thickness, ECSColor color)
{
    if (!DrawCanDraw(surface))
    {
        return;
    }

    SDL_Rect bounds = DrawBounds(surface);
    usz pointCount = points.size / (2 * sizeof(f32));
    usz indexCount = indices.size / sizeof(u32);
    const u32 *index = indices.data;

    // without indices, one line goes through the points in order
    usz count = indexCount > 0 ? indexCount / 2 : (pointCount > 0 ? pointCount - 1 : 0);

    for (usz i = 0; i < count; i++)
    {
        usz a = indexCount > 0 ? index[2 * i] : i;
        usz b = indexCount > 0 ? index[2 * i + 1] : i + 1;

        if (a < pointCount && b < pointCount)
        {
            DrawSegment(surface, bounds, DrawPointAt(points, a, surface->scale), DrawPointAt(points, b, surface->scale), thickness * surface->scale, color);
        }
    }
}

#pragma endregion Shapes

#pragma region Images

/// @brief The colour of an image between its pixels, from the four nearest, each counted by its alpha so that transparent pixels do not darken the edges.
/// @param u, v A point in the image's pixels, whose centres are at whole numbers; outside the image, the nearest edge pixel counts.
static ECSColor DrawSample(const SDL_Surface *image, f32 u, f32 v)
{
    i32 left = (i32)SDL_floorf(u);
    i32 top = (i32)SDL_floorf(v);
    f32 across = u - (f32)left;
    f32 down = v - (f32)top;
    f32 sums[4] = {0}; // blue, green and red, each multiplied by alpha, then alpha

    for (i32 corner = 0; corner < 4; corner++)
    {
        i32 column = SDL_clamp(left + (corner & 1), 0, image->w - 1);
        i32 row = SDL_clamp(top + (corner >> 1), 0, image->h - 1);
        f32 weight = ((corner & 1) ? across : 1.0f - across) * ((corner >> 1) ? down : 1.0f - down);
        u32 pixel = ((const u32 *)((const u8 *)image->pixels + (usz)row * (usz)image->pitch))[column];
        f32 alpha = (f32)(pixel >> 24) * weight;

        for (u32 channel = 0; channel < 3; channel++)
        {
            sums[channel] += (f32)((pixel >> (channel * 8)) & 0xFF) * alpha;
        }

        sums[3] += alpha;
    }

    ECSColor color = (u32)SDL_roundf(sums[3]) << 24;

    for (u32 channel = 0; sums[3] > 0.0f && channel < 3; channel++)
    {
        color |= (u32)SDL_roundf(sums[channel] / sums[3]) << (channel * 8);
    }

    return color;
}

/// @brief Multiplies each channel of a colour by the same channel of another.
static ECSColor DrawMultiply(ECSColor color, ECSColor by)
{
    ECSColor product = 0;

    for (u32 shift = 0; shift < 32; shift += 8)
    {
        product |= (((color >> shift) & 0xFF) * ((by >> shift) & 0xFF) / 255) << shift;
    }

    return product;
}

static void DrawImageTransformed(ECSSurface *surface, DrawImage *image, f32 x, f32 y, f32 width, f32 height, f32 angle, ECSColor color)
{
    if (!DrawCanDraw(surface) || width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    // the rectangle in pixels, turned around its centre; a positive angle turns it clockwise on the screen
    f32 scale = surface->scale;
    f32 w = width * scale;
    f32 h = height * scale;
    f32 centreX = (x + width / 2.0f) * scale;
    f32 centreY = (y + height / 2.0f) * scale;
    f32 cosine = SDL_cosf(angle * SDL_PI_F / 180.0f);
    f32 sine = SDL_sinf(angle * SDL_PI_F / 180.0f);
    f32 reachX = (SDL_fabsf(w * cosine) + SDL_fabsf(h * sine)) / 2.0f;
    f32 reachY = (SDL_fabsf(w * sine) + SDL_fabsf(h * cosine)) / 2.0f;
    SDL_Rect area = {(i32)SDL_floorf(centreX - reachX), (i32)SDL_floorf(centreY - reachY), 0, 0};
    area.w = (i32)SDL_ceilf(centreX + reachX) - area.x;
    area.h = (i32)SDL_ceilf(centreY + reachY) - area.y;
    SDL_Rect bounds = DrawBounds(surface);

    if (!SDL_GetRectIntersection(&area, &bounds, &area))
    {
        return;
    }

    for (i32 row = area.y; row < area.y + area.h; row++)
    {
        for (i32 column = area.x; column < area.x + area.w; column++)
        {
            // the pixel's centre, turned back into the rectangle, from its top left
            f32 dx = (f32)column + 0.5f - centreX;
            f32 dy = (f32)row + 0.5f - centreY;
            f32 inX = dx * cosine + dy * sine + w / 2.0f;
            f32 inY = -dx * sine + dy * cosine + h / 2.0f;

            // a pixel at the rectangle's edge is covered in part, so the edges are smooth
            f32 coverage = SDL_min(SDL_min(inX, w - inX), SDL_min(inY, h - inY)) + 0.5f;

            if (coverage > 0.0f)
            {
                ECSColor sample = DrawSample(image->surface, inX / w * (f32)image->surface->w - 0.5f, inY / h * (f32)image->surface->h - 0.5f);
                DrawCover(surface, bounds, column, row, DrawMultiply(sample, color), coverage);
            }
        }
    }
}

/// @brief Frees an image; the Destroy function of the handle type draw.image.
static void DrawImageDestroy(void *object)
{
    DrawImage *image = object;
    SDL_DestroySurface(image->surface);
    SDL_free(image);
}

static DrawImage *DrawNewImage(i32 width, i32 height, SHUSlice pixels)
{
    if (width <= 0 || height <= 0 || (pixels.size != 0 && pixels.size != (usz)width * (usz)height * sizeof(u32)))
    {
        ECS_LogWarn(DRAW.plugin, "An image of %d by %d pixels needs %d bytes of pixels, or none.", width, height, width * height * 4);
        return nullptr;
    }

    DrawImage *image = SDL_calloc(1, sizeof(DrawImage));
    image->surface = image == nullptr ? nullptr : SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);

    if (image == nullptr || image->surface == nullptr)
    {
        SDL_free(image);
        return nullptr;
    }

    // a missing buffer gives a transparent image
    for (i32 row = 0; row < height && pixels.size != 0; row++)
    {
        SDL_memcpy((u8 *)image->surface->pixels + (usz)row * (usz)image->surface->pitch, (const u8 *)pixels.data + (usz)row * (usz)width * sizeof(u32), (usz)width * sizeof(u32));
    }

    SDL_SetSurfaceBlendMode(image->surface, SDL_BLENDMODE_BLEND);
    return image;
}

static void DrawImageSize(DrawImage *image, i32 *retWidth, i32 *retHeight)
{
    *retWidth = image->surface->w;
    *retHeight = image->surface->h;
}

static ECSColor DrawImagePixel(DrawImage *image, i32 x, i32 y)
{
    if (x < 0 || y < 0 || x >= image->surface->w || y >= image->surface->h)
    {
        return 0;
    }

    return ((const u32 *)((const u8 *)image->surface->pixels + (usz)y * (usz)image->surface->pitch))[x];
}

static SHUSlice DrawImagePixels(DrawImage *image)
{
    // a surface of four bytes a pixel has no padding after its rows, so its pixels are row after row
    SDL_assert(image->surface->pitch == image->surface->w * (i32)sizeof(u32));
    return cs(image->surface->pixels, (usz)image->surface->pitch * (usz)image->surface->h);
}

static void DrawFreeImage(DrawImage *image)
{
    ECSHandle_Forget(image);
    DrawImageDestroy(image);
}

static void DrawDrawImage(ECSSurface *surface, DrawImage *image, f32 x, f32 y, f32 width, f32 height)
{
    SDL_Surface *target = DrawCanDraw(surface) ? SDL_CreateSurfaceFrom(surface->width, surface->height, SDL_PIXELFORMAT_ARGB8888, surface->pixels.data, surface->pitch) : nullptr;

    if (target == nullptr)
    {
        return;
    }

    SDL_Rect bounds = DrawBounds(surface);
    SDL_Rect rect = DrawPixels(surface->scale, x, y, width, height);
    SDL_SetSurfaceClipRect(target, &bounds);
    SDL_BlitSurfaceScaled(image->surface, nullptr, target, &rect, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(target);
}

#pragma endregion Images

#pragma region Text

/// @brief Finds the file of a font name: "ui" is the core's font, "mono" the setting draw.monoFont, or the font draw ships with when it is empty, and anything else a path. A relative path starts at the executable's folder.
/// @return The path, to free with SDL_free, or nullptr if out of memory.
static char *DrawFontPath(const char *name)
{
    const char *file = SDL_strcmp(name, "ui") == 0 ? ECSValue_GetString(ECSSetting_Get("ecs.font"), "") : SDL_strcmp(name, "mono") == 0 ? ECSValue_GetString(ECSSetting_Get("draw.monoFont"), "") : name;
    bool shipped = SDL_strcmp(name, "mono") == 0 && file[0] == '\0';
    char *path = nullptr;

    if (SDL_asprintf(&path, "%s%s", shipped ? ECSPlugin_GetFolder(DRAW.plugin) : ECSPath_IsAbsolute(file) ? "" : SDL_GetBasePath(), shipped ? DRAW_MONO_FONT : file) < 0)
    {
        return nullptr;
    }

    return path;
}

/// @brief Closes the sizes a font has open, such as when its file changes.
static void DrawCloseSizes(DrawFont *font)
{
    for (usz i = 0; i < font->sizeCount; i++)
    {
        TTF_CloseFont(font->sizes[i].font);
    }

    SDL_free(font->sizes);
    font->sizes = nullptr;
    font->sizeCount = 0;
}

static DrawFont *DrawGetFontByName(const char *name)
{
    for (usz i = 0; i < DRAW.fontCount; i++)
    {
        if (SDL_strcmp(DRAW.fonts[i]->name, name) == 0)
        {
            return DRAW.fonts[i];
        }
    }

    DrawFont *font = SDL_calloc(1, sizeof(DrawFont));
    DrawFont **fonts = font == nullptr ? nullptr : SDL_realloc(DRAW.fonts, (DRAW.fontCount + 1) * sizeof(DrawFont *));

    if (fonts == nullptr || (font->name = SDL_strdup(name)) == nullptr || (font->path = DrawFontPath(name)) == nullptr)
    {
        if (font != nullptr)
        {
            SDL_free(font->name);
        }

        SDL_free(font);
        DRAW.fonts = fonts != nullptr ? fonts : DRAW.fonts;
        return nullptr;
    }

    DRAW.fonts = fonts;
    DRAW.fonts[DRAW.fontCount++] = font;
    return font;
}

/// @brief Gets a font at a size in pixels, opening it the first time.
/// @return The font, or nullptr if it cannot be opened; the reason is logged.
static TTF_Font *DrawGetSize(DrawFont *font, f32 pixels)
{
    i32 size = (i32)SDL_max(1.0f, SDL_roundf(pixels));

    for (usz i = 0; i < font->sizeCount; i++)
    {
        if (font->sizes[i].pixels == size)
        {
            return font->sizes[i].font;
        }
    }

    TTF_Font *opened = TTF_OpenFont(font->path, (float)size);
    DrawSize *sizes = opened == nullptr ? nullptr : SDL_realloc(font->sizes, (font->sizeCount + 1) * sizeof(DrawSize));

    if (sizes == nullptr)
    {
        ECS_LogError(DRAW.plugin, "Cannot open the font '%s' at %d pixels: %s", font->path, size, SDL_GetError());
        TTF_CloseFont(opened);
        return nullptr;
    }

    font->sizes = sizes;
    font->sizes[font->sizeCount++] = (DrawSize){.pixels = size, .font = opened};
    return opened;
}

/// @brief Opens the fonts again when a setting that names a font file changes.
static void DrawFontsChanged(void *data)
{
    (void)data;

    for (usz i = 0; i < DRAW.fontCount; i++)
    {
        char *path = DrawFontPath(DRAW.fonts[i]->name);

        if (path != nullptr)
        {
            DrawCloseSizes(DRAW.fonts[i]);
            SDL_free(DRAW.fonts[i]->path);
            DRAW.fonts[i]->path = path;
        }
    }
}

/// @brief Gives the file a font is read from, for plugins that draw text in other ways, such as gpu. A setting that names another file changes it.
static const char *DrawFontFile(DrawFont *font)
{
    return font->path;
}

static f32 DrawText(ECSSurface *surface, DrawFont *font, const char *text, f32 x, f32 y, f32 size, ECSColor color)
{
    TTF_Font *opened = DrawCanDraw(surface) && surface->scale > 0.0f ? DrawGetSize(font, size * surface->scale) : nullptr;
    TTF_Text *drawn = opened == nullptr || text[0] == '\0' ? nullptr : TTF_CreateText(DRAW.engine, opened, text, 0);
    SDL_Surface *target = drawn == nullptr ? nullptr : SDL_CreateSurfaceFrom(surface->width, surface->height, SDL_PIXELFORMAT_ARGB8888, surface->pixels.data, surface->pitch);
    int width = 0;

    if (target != nullptr)
    {
        SDL_Rect bounds = DrawBounds(surface);
        SDL_SetSurfaceClipRect(target, &bounds);
        TTF_SetTextColor(drawn, (Uint8)(color >> 16), (Uint8)(color >> 8), (Uint8)color, (Uint8)(color >> 24));
        TTF_DrawSurfaceText(drawn, (int)SDL_roundf(x * surface->scale), (int)SDL_roundf(y * surface->scale), target);
        TTF_GetTextSize(drawn, &width, nullptr);
        SDL_DestroySurface(target);
    }

    TTF_DestroyText(drawn);
    return surface->scale > 0.0f ? (f32)width / surface->scale : 0.0f;
}

static void DrawMeasure(DrawFont *font, const char *text, f32 size, f32 *retWidth, f32 *retHeight)
{
    TTF_Font *opened = DrawGetSize(font, size);
    int width = 0;

    if (opened != nullptr)
    {
        TTF_GetStringSize(opened, text, 0, &width, nullptr);
    }

    *retWidth = (f32)width;
    *retHeight = opened == nullptr ? 0.0f : (f32)TTF_GetFontHeight(opened);
}

#pragma endregion Text

#pragma region Clipping

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

    if (clips == nullptr)
    {
        ECS_LogError(DRAW.plugin, "Cannot clip a surface: out of memory.");
        return;
    }

    DRAW.clips = clips;
    DRAW.clips[DRAW.clipCount++] = (DrawClip){.surface = surface, .rect = rect};
}

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

#pragma endregion Clipping

/// @brief Frees the fonts and the text engine, and stops SDL_ttf. The Destroy function of the plugin.
static void DrawFree(void *data)
{
    (void)data;

    for (usz i = 0; i < DRAW.fontCount; i++)
    {
        DrawCloseSizes(DRAW.fonts[i]);
        SDL_free(DRAW.fonts[i]->name);
        SDL_free(DRAW.fonts[i]->path);
        SDL_free(DRAW.fonts[i]);
    }

    if (DRAW.engine != nullptr)
    {
        TTF_DestroySurfaceTextEngine(DRAW.engine);
    }

    // SDL_ttf counts its starts, so the core's own use goes on
    if (DRAW.ttfStarted)
    {
        TTF_Quit();
    }

    SDL_free(DRAW.fonts);
    SDL_free(DRAW.clips);
    SDL_zero(DRAW);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    DRAW.plugin = plugin;
    DRAW.ttfStarted = TTF_Init();
    DRAW.engine = DRAW.ttfStarted ? TTF_CreateSurfaceTextEngine() : nullptr;

    if (DRAW.engine == nullptr)
    {
        ECS_LogError(plugin, "Cannot start SDL_ttf: %s", SDL_GetError());
        DrawFree(nullptr);
        return SHUResult_ErrInternal;
    }

    const ECSSettingDesc monoFont = {
        .name = "monoFont",
        .type = ECSSettingType_String,
        .description = "Monospace TrueType font, draw.font(\"mono\"); empty for the one draw ships with",
        .defaultString = "",
        .Changed = DrawFontsChanged,
    };

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = DrawFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), DrawFree(nullptr););
    SHU_ReturnResult(ECSSetting_Declare(plugin, &monoFont), DrawFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "font", nullptr), DrawFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "image", DrawImageDestroy), DrawFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"fill", (ECSFunction)DrawFill, "void(handle<ecs.surface> surface, float x, float y, float width, float height, color color)", "Fill a rectangle"},
        {"outline", (ECSFunction)DrawOutline, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float thickness, color color)", "Draw the edge of a rectangle"},
        {"line", (ECSFunction)DrawLine, "void(handle<ecs.surface> surface, float x1, float y1, float x2, float y2, float thickness, color color)", "Draw a line"},
        {"circle", (ECSFunction)DrawCircle, "void(handle<ecs.surface> surface, float x, float y, float radius, float thickness, color color)", "Draw a circle: a ring of a thickness, or filled with a thickness of 0"},
        {"rounded", (ECSFunction)DrawRoundedFill, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float radius, color color)", "Fill a rectangle with corners rounded by a radius"},
        {"roundedOutline", (ECSFunction)DrawRoundedOutline, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float radius, float thickness, color color)", "Draw the edge of a rectangle with corners rounded by a radius"},
        {"triangles", (ECSFunction)DrawTriangles, "void(handle<ecs.surface> surface, buffer points, buffer indices, color color)", "Fill triangles: points are x and y floats, and indices name three points for each triangle, or are empty for the points in threes"},
        {"lines", (ECSFunction)DrawLines, "void(handle<ecs.surface> surface, buffer points, buffer indices, float thickness, color color)", "Draw lines: points are x and y floats, and indices name two points for each line, or are empty for one line through the points"},
        {"newImage", (ECSFunction)DrawNewImage, "handle<draw.image>(int width, int height, buffer pixels)", "Make an image from pixels, ARGB in native byte order, row after row; empty pixels make it transparent"},
        {"imageSize", (ECSFunction)DrawImageSize, "void(handle<draw.image> image, out int width, out int height)", "Give an image's size in pixels"},
        {"imagePixel", (ECSFunction)DrawImagePixel, "color(handle<draw.image> image, int x, int y)", "Give the colour of a pixel of an image"},
        {"imagePixels", (ECSFunction)DrawImagePixels, "buffer(handle<draw.image> image)", "Give an image's pixels, ARGB in native byte order, row after row; they stay the image's, so C reads them only while the image lives"},
        {"freeImage", (ECSFunction)DrawFreeImage, "void(handle<draw.image> image)", "Free an image now, instead of when no one uses its handle"},
        {"image", (ECSFunction)DrawDrawImage, "void(handle<ecs.surface> surface, handle<draw.image> image, float x, float y, float width, float height)", "Draw an image stretched to a rectangle"},
        {"imageTransformed", (ECSFunction)DrawImageTransformed, "void(handle<ecs.surface> surface, handle<draw.image> image, float x, float y, float width, float height, float angle, color color)", "Draw an image stretched to a rectangle, turned clockwise by an angle in degrees around its centre, with each pixel's colour multiplied by a colour"},
        {"font", (ECSFunction)DrawGetFontByName, "handle<draw.font>(string name)", "Give a font: \"ui\", the interface's; \"mono\", a monospace one; or the path of a TrueType file"},
        {"fontFile", (ECSFunction)DrawFontFile, "string(handle<draw.font> font)", "Give the file a font is read from; a setting that names another file changes it"},
        {"text", (ECSFunction)DrawText, "float(handle<ecs.surface> surface, handle<draw.font> font, string text, float x, float y, float size, color color)", "Draw text with its line's top left at a position, and give its width"},
        {"measure", (ECSFunction)DrawMeasure, "void(handle<draw.font> font, string text, float size, out float width, out float lineHeight)", "Give the width and the line height of a text"},
        {"clip", (ECSFunction)DrawSetClip, "void(handle<ecs.surface> surface, float x, float y, float width, float height)", "Limit later drawing into a surface to a rectangle"},
        {"unclip", (ECSFunction)DrawUnclip, "void(handle<ecs.surface> surface)", "Let drawing reach the whole surface again"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), DrawFree(nullptr););
    }

    // Lua calls these on a handle, such as handle:measure(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"font", "measure", "measure"},
        {"image", "size", "imageSize"},
        {"image", "pixel", "imagePixel"},
        {"image", "pixels", "imagePixels"},
        {"image", "free", "freeImage"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function), DrawFree(nullptr););
    }

    return SHUResult_Ok;
}
