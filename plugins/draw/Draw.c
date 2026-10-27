// The draw standard plugin: draws shapes, images and text into the pixels surface of another plugin's panel (DESIGN 2).
// It knows the primitives of 2D drawing; loaders, such as image, turn files into them. It calls SDL3 and SDL3_ttf itself, using the executable's copy.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

#include <float.h>

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

/// @brief The monospace font draw ships with, in its folder.
#define DRAW_MONO_FONT "fonts/RobotoMono-Regular.ttf"

/// @brief A font file opened at one size, in pixels.
typedef struct DrawFontSize
{
    i32 pixels;
    TTF_Font *font;
} DrawFontSize;

/// @brief A font: a TrueType file, opened at each size asked for. Its handle, draw.font, lives as long as the plugin.
typedef struct DrawFont
{
    char *name;          // the name it was asked by: "ui", "mono" or a path
    char *path;          // the file
    DrawFontSize *sizes; // stb_ds array of the sizes opened so far
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
    DrawFont **fonts;       // stb_ds array of every font asked for so far
    DrawClip *clips;        // stb_ds array of the surfaces that have a clip rectangle
    TTF_TextEngine *engine; // draws text into surfaces, and keeps the glyphs it drew
    bool ttfStarted;
    ECSSubscription settingChanged; // hears ecs.font change, the core's setting that the "ui" font follows
} DRAW = {0};

#pragma region Pixels

/// @brief Gets the rectangle of a surface that drawing reaches: the whole surface, or its clip rectangle.
static SDL_Rect DrawBounds(const ECSSurface *surface)
{
    SDL_Rect bounds = {0, 0, surface->width, surface->height};

    for (usz i = 0; i < arrlenu(DRAW.clips); i++)
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
    f32 centerX;
    f32 centerY;
    f32 halfWidth;
    f32 halfHeight;
    f32 radius;    // of the corners
    f32 thickness; // of a ring, or 0 for the whole shape
} DrawRounded;

static f32 DrawRoundedDistance(const void *shape, f32 x, f32 y)
{
    const DrawRounded *rounded = shape;

    // how far the point is past the straight part of each side, toward the corners
    f32 pastX = SDL_fabsf(x - rounded->centerX) - (rounded->halfWidth - rounded->radius);
    f32 pastY = SDL_fabsf(y - rounded->centerY) - (rounded->halfHeight - rounded->radius);
    f32 outside = SDL_sqrtf(SDL_max(pastX, 0.0f) * SDL_max(pastX, 0.0f) + SDL_max(pastY, 0.0f) * SDL_max(pastY, 0.0f));
    f32 distance = outside + SDL_min(SDL_max(pastX, pastY), 0.0f) - rounded->radius;

    // a ring is the band from the edge inwards by its thickness
    return rounded->thickness > 0.0f ? SDL_fabsf(distance + rounded->thickness / 2.0f) - rounded->thickness / 2.0f : distance;
}

/// @brief A line of a thickness between two points, with flat ends, in pixels.
typedef struct DrawStroke
{
    DrawPoint start;
    DrawPoint end;
    f32 halfThickness;
} DrawStroke;

static f32 DrawStrokeDistance(const void *shape, f32 x, f32 y)
{
    const DrawStroke *stroke = shape;
    f32 directionX = stroke->end.x - stroke->start.x;
    f32 directionY = stroke->end.y - stroke->start.y;
    f32 length = SDL_sqrtf(directionX * directionX + directionY * directionY);
    f32 along = ((x - stroke->start.x) * directionX + (y - stroke->start.y) * directionY) / length;
    f32 across = SDL_fabsf((x - stroke->start.x) * directionY - (y - stroke->start.y) * directionX) / length;

    // the distance to the band across the line, and to the band along it, whichever is farther
    return SDL_max(across - stroke->halfThickness, SDL_fabsf(along - length / 2.0f) - length / 2.0f);
}

/// @brief Blends a colour over a row of pixels, from one column up to another, without it, inside the bounds.
static void DrawSpan(const ECSSurface *surface, SDL_Rect bounds, i32 row, i32 start, i32 end, ECSColor color)
{
    start = SDL_max(start, bounds.x);
    end = SDL_min(end, bounds.x + bounds.w);

    if (row < bounds.y || row >= bounds.y + bounds.h || start >= end || (color >> 24) == 0)
    {
        return;
    }

    u32 *pixel = (u32 *)((u8 *)surface->pixels.data + (usz)row * (usz)surface->pitch);

    for (i32 column = start; column < end; column++)
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
static void DrawTriangle(const ECSSurface *surface, SDL_Rect bounds, DrawPoint pointA, DrawPoint pointB, DrawPoint pointC, ECSColor color)
{
    f32 top = SDL_min(pointA.y, SDL_min(pointB.y, pointC.y));
    f32 bottom = SDL_max(pointA.y, SDL_max(pointB.y, pointC.y));
    i32 first = SDL_max(bounds.y, (i32)SDL_ceilf(top - 0.5f));
    i32 last = SDL_min(bounds.y + bounds.h, (i32)SDL_ceilf(bottom - 0.5f));
    const DrawPoint edges[3][2] = {{pointA, pointB}, {pointB, pointC}, {pointC, pointA}};

    // each row is crossed by two edges; the span between them is filled
    for (i32 row = first; row < last; row++)
    {
        f32 y = (f32)row + 0.5f;
        f32 left = FLT_MAX;
        f32 right = -FLT_MAX;

        for (usz i = 0; i < 3; i++)
        {
            DrawPoint start = edges[i][0];
            DrawPoint end = edges[i][1];

            if ((start.y <= y && end.y > y) || (end.y <= y && start.y > y))
            {
                f32 x = start.x + (y - start.y) * (end.x - start.x) / (end.y - start.y);
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
static void DrawSegment(const ECSSurface *surface, SDL_Rect bounds, DrawPoint start, DrawPoint end, f32 thickness, ECSColor color)
{
    if (start.x == end.x && start.y == end.y)
    {
        return;
    }

    // the line is at least a pixel thick, so a thin line never disappears
    DrawStroke stroke = {start, end, SDL_max(thickness, 1.0f) / 2.0f};
    f32 reach = stroke.halfThickness + 1.0f;
    SDL_Rect area = {(i32)SDL_floorf(SDL_min(start.x, end.x) - reach), (i32)SDL_floorf(SDL_min(start.y, end.y) - reach), 0, 0};
    area.w = (i32)SDL_ceilf(SDL_max(start.x, end.x) + reach) - area.x;
    area.h = (i32)SDL_ceilf(SDL_max(start.y, end.y) + reach) - area.y;
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
static SDL_Rect DrawToPixels(f32 scale, f32 x, f32 y, f32 width, f32 height)
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
        DrawRect(surface, DrawBounds(surface), DrawToPixels(surface->scale, x, y, width, height), color);
    }
}

static void DrawOutline(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, f32 thickness, ECSColor color)
{
    if (!DrawCanDraw(surface))
    {
        return;
    }

    SDL_Rect bounds = DrawBounds(surface);
    SDL_Rect rect = DrawToPixels(surface->scale, x, y, width, height);
    int edge = SDL_max(1, (int)SDL_roundf(thickness * surface->scale));
    edge = SDL_min(edge, SDL_min(rect.w, rect.h) / 2 + 1);

    DrawRect(surface, bounds, (SDL_Rect){rect.x, rect.y, rect.w, edge}, color);
    DrawRect(surface, bounds, (SDL_Rect){rect.x, rect.y + rect.h - edge, rect.w, edge}, color);
    DrawRect(surface, bounds, (SDL_Rect){rect.x, rect.y + edge, edge, rect.h - 2 * edge}, color);
    DrawRect(surface, bounds, (SDL_Rect){rect.x + rect.w - edge, rect.y + edge, edge, rect.h - 2 * edge}, color);
}

static void DrawLine(ECSSurface *surface, f32 startX, f32 startY, f32 endX, f32 endY, f32 thickness, ECSColor color)
{
    if (DrawCanDraw(surface))
    {
        f32 scale = surface->scale;
        DrawSegment(surface, DrawBounds(surface), (DrawPoint){startX * scale, startY * scale}, (DrawPoint){endX * scale, endY * scale}, thickness * scale, color);
    }
}

// a circle is a square whose corners are rounded by half its size
static void DrawCircle(ECSSurface *surface, f32 x, f32 y, f32 radius, f32 thickness, ECSColor color)
{
    if (DrawCanDraw(surface))
    {
        f32 scale = surface->scale;
        f32 pixelRadius = radius * scale;
        DrawRoundedPixels(surface, DrawBounds(surface), x * scale - pixelRadius, y * scale - pixelRadius, 2.0f * pixelRadius, 2.0f * pixelRadius, pixelRadius, thickness > 0.0f ? SDL_max(thickness * scale, 1.0f) : 0.0f, color);
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
        usz first = indexCount > 0 ? index[3 * i] : 3 * i;
        usz second = indexCount > 0 ? index[3 * i + 1] : 3 * i + 1;
        usz third = indexCount > 0 ? index[3 * i + 2] : 3 * i + 2;

        if (first < pointCount && second < pointCount && third < pointCount)
        {
            DrawTriangle(surface, bounds, DrawPointAt(points, first, surface->scale), DrawPointAt(points, second, surface->scale), DrawPointAt(points, third, surface->scale), color);
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
        usz start = indexCount > 0 ? index[2 * i] : i;
        usz end = indexCount > 0 ? index[2 * i + 1] : i + 1;

        if (start < pointCount && end < pointCount)
        {
            DrawSegment(surface, bounds, DrawPointAt(points, start, surface->scale), DrawPointAt(points, end, surface->scale), thickness * surface->scale, color);
        }
    }
}

#pragma endregion Shapes

#pragma region Images

/// @brief The colour of an image between its pixels, from the four nearest, each counted by its alpha so that transparent pixels do not darken the edges.
/// @param imageX, imageY A point in the image's pixels, whose centres are at whole numbers; outside the image, the nearest edge pixel counts.
static ECSColor DrawSample(const SDL_Surface *image, f32 imageX, f32 imageY)
{
    i32 left = (i32)SDL_floorf(imageX);
    i32 top = (i32)SDL_floorf(imageY);
    f32 across = imageX - (f32)left;
    f32 down = imageY - (f32)top;
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
static ECSColor DrawMultiply(ECSColor color, ECSColor factor)
{
    ECSColor product = 0;

    for (u32 shift = 0; shift < 32; shift += 8)
    {
        product |= (((color >> shift) & 0xFF) * ((factor >> shift) & 0xFF) / 255) << shift;
    }

    return product;
}

static void DrawImageTransformed(ECSSurface *surface, void *imageHandle, f32 x, f32 y, f32 width, f32 height, f32 angle, ECSColor color)
{
    DrawImage *image = imageHandle;

    if (!DrawCanDraw(surface) || width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    // the rectangle in pixels, turned around its centre; a positive angle turns it clockwise on the screen
    f32 scale = surface->scale;
    f32 pixelWidth = width * scale;
    f32 pixelHeight = height * scale;
    f32 centreX = (x + width / 2.0f) * scale;
    f32 centreY = (y + height / 2.0f) * scale;
    f32 cosine = SDL_cosf(angle * SDL_PI_F / 180.0f);
    f32 sine = SDL_sinf(angle * SDL_PI_F / 180.0f);
    f32 reachX = (SDL_fabsf(pixelWidth * cosine) + SDL_fabsf(pixelHeight * sine)) / 2.0f;
    f32 reachY = (SDL_fabsf(pixelWidth * sine) + SDL_fabsf(pixelHeight * cosine)) / 2.0f;
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
            f32 fromCentreX = (f32)column + 0.5f - centreX;
            f32 fromCentreY = (f32)row + 0.5f - centreY;
            f32 inX = fromCentreX * cosine + fromCentreY * sine + pixelWidth / 2.0f;
            f32 inY = -fromCentreX * sine + fromCentreY * cosine + pixelHeight / 2.0f;

            // a pixel at the rectangle's edge is covered in part, so the edges are smooth
            f32 coverage = SDL_min(SDL_min(inX, pixelWidth - inX), SDL_min(inY, pixelHeight - inY)) + 0.5f;

            if (coverage > 0.0f)
            {
                ECSColor sample = DrawSample(image->surface, inX / pixelWidth * (f32)image->surface->w - 0.5f, inY / pixelHeight * (f32)image->surface->h - 0.5f);
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

static void *DrawNewImage(i32 width, i32 height, SHUSlice pixels)
{
    if (width <= 0 || height <= 0 || (pixels.size != 0 && pixels.size != (usz)width * (usz)height * sizeof(u32)))
    {
        ECS_LogWarn(DRAW.plugin, "An image of %d by %d pixels needs %lld bytes of pixels, or none.", width, height, (long long)width * (long long)height * 4);
        return nullptr;
    }

    DrawImage *image = SDL_calloc(1, sizeof(DrawImage));

    if (image == nullptr)
    {
        return nullptr;
    }

    image->surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);

    if (image->surface == nullptr)
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

static void DrawImageSize(void *imageHandle, i32 *retWidth, i32 *retHeight)
{
    DrawImage *image = imageHandle;

    *retWidth = image->surface->w;
    *retHeight = image->surface->h;
}

static ECSColor DrawImagePixel(void *imageHandle, i32 x, i32 y)
{
    DrawImage *image = imageHandle;

    if (x < 0 || y < 0 || x >= image->surface->w || y >= image->surface->h)
    {
        return 0;
    }

    return ((const u32 *)((const u8 *)image->surface->pixels + (usz)y * (usz)image->surface->pitch))[x];
}

static SHUSlice DrawImagePixels(void *imageHandle)
{
    DrawImage *image = imageHandle;

    // a surface of four bytes a pixel has no padding after its rows, so its pixels are row after row
    SDL_assert(image->surface->pitch == image->surface->w * (i32)sizeof(u32));
    return cs(image->surface->pixels, (usz)image->surface->pitch * (usz)image->surface->h);
}

static void DrawFreeImage(void *image)
{
    ECSHandle_Free(DRAW.plugin, "image", image);
}

/// @brief Wraps a surface's pixels in an SDL surface, clipped to what drawing reaches, for SDL and SDL_ttf to draw into. Free it with SDL_DestroySurface.
/// @return The SDL surface, or nullptr if draw cannot draw into the surface.
static SDL_Surface *DrawWrapSurface(const ECSSurface *surface)
{
    SDL_Surface *target = DrawCanDraw(surface) ? SDL_CreateSurfaceFrom(surface->width, surface->height, SDL_PIXELFORMAT_ARGB8888, surface->pixels.data, surface->pitch) : nullptr;

    if (target != nullptr)
    {
        SDL_Rect bounds = DrawBounds(surface);
        SDL_SetSurfaceClipRect(target, &bounds);
    }

    return target;
}

static void DrawDrawImage(ECSSurface *surface, void *imageHandle, f32 x, f32 y, f32 width, f32 height)
{
    DrawImage *image = imageHandle;

    SDL_Surface *target = DrawWrapSurface(surface);

    if (target == nullptr)
    {
        return;
    }

    SDL_Rect rect = DrawToPixels(surface->scale, x, y, width, height);
    SDL_BlitSurfaceScaled(image->surface, nullptr, target, &rect, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(target);
}

#pragma endregion Images

#pragma region Text

/// @brief Finds the file of a font name: "ui" is the core's font, "mono" the setting draw.monoFont, or the font draw ships with when it is empty, and anything else a path. A relative path starts at the executable's folder.
/// @return The path, to free with SDL_free, or nullptr if out of memory.
static char *DrawFontPath(const char *name)
{
    const char *file = SDL_strcmp(name, "ui") == 0 ? ECSValue_GetString(ECSSetting_Get("ecs.font"), "") : SDL_strcmp(name, "mono") == 0 ? ECSValue_GetString(ECSSetting_Get("draw.monoFont"), "")
                                                                                                                                        : name;
    bool shipped = SDL_strcmp(name, "mono") == 0 && file[0] == '\0';
    char *path = nullptr;

    if (shipped)
    {
        return SDL_asprintf(&path, "%s%s", ECSPlugin_GetFolder(DRAW.plugin), DRAW_MONO_FONT) < 0 ? nullptr : path;
    }

    return ECSPath_GetFull(&path, file) ? nullptr : path;
}

/// @brief Closes the sizes a font has open, such as when its file changes.
static void DrawCloseSizes(DrawFont *font)
{
    for (usz i = 0; i < arrlenu(font->sizes); i++)
    {
        TTF_CloseFont(font->sizes[i].font);
    }

    arrfree(font->sizes);
}

/// @brief Finds the font of a name, and adds it the first time it is asked for.
/// @return The font, or nullptr if out of memory.
static void *DrawFindOrAddFont(const char *name)
{
    for (usz i = 0; i < arrlenu(DRAW.fonts); i++)
    {
        if (SDL_strcmp(DRAW.fonts[i]->name, name) == 0)
        {
            return DRAW.fonts[i];
        }
    }

    DrawFont *font = SDL_calloc(1, sizeof(DrawFont));

    if (font == nullptr)
    {
        return nullptr;
    }

    font->name = SDL_strdup(name);
    font->path = DrawFontPath(name);

    if (font->name == nullptr || font->path == nullptr)
    {
        SDL_free(font->name);
        SDL_free(font->path);
        SDL_free(font);
        return nullptr;
    }

    arrput(DRAW.fonts, font);
    return font;
}

/// @brief Finds a font at a size in pixels, and opens it the first time.
/// @return The font, or nullptr if it cannot be opened; the reason is logged.
static TTF_Font *DrawFindOrOpenSize(DrawFont *font, f32 pixels)
{
    i32 size = (i32)SDL_max(1.0f, SDL_roundf(pixels));

    for (usz i = 0; i < arrlenu(font->sizes); i++)
    {
        if (font->sizes[i].pixels == size)
        {
            return font->sizes[i].font;
        }
    }

    TTF_Font *opened = TTF_OpenFont(font->path, (float)size);

    if (opened == nullptr)
    {
        ECS_LogError(DRAW.plugin, "Cannot open the font '%s' at %d pixels: %s", font->path, size, SDL_GetError());
        return nullptr;
    }

    arrput(font->sizes, ((DrawFontSize){.pixels = size, .font = opened}));
    return opened;
}

/// @brief Opens the fonts again when a setting that names a font file changes.
static void DrawFontsChanged(void *data)
{
    (void)data;

    for (usz i = 0; i < arrlenu(DRAW.fonts); i++)
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

/// @brief Opens the fonts again when the core's font, which "ui" is, changes.
static void DrawSettingChanged(void *data, const char *name, const ECSValue *value)
{
    (void)name;

    if (SDL_strcmp(ECSValue_GetString(ECSValue_GetTableField(value, "name"), ""), "ecs.font") == 0)
    {
        DrawFontsChanged(data);
    }
}

/// @brief Gives the file a font is read from, for plugins that draw text in other ways, such as gpu. A setting that names another file changes it.
static const char *DrawFontFile(void *fontHandle)
{
    DrawFont *font = fontHandle;

    return font->path;
}

static f32 DrawText(ECSSurface *surface, void *font, const char *text, f32 x, f32 y, f32 size, ECSColor color)
{
    if (text[0] == '\0' || surface->scale <= 0.0f)
    {
        return 0.0f;
    }

    TTF_Font *opened = DrawFindOrOpenSize(font, size * surface->scale);
    SDL_Surface *target = opened == nullptr ? nullptr : DrawWrapSurface(surface);

    if (target == nullptr)
    {
        return 0.0f;
    }

    int width = 0;
    TTF_Text *drawn = TTF_CreateText(DRAW.engine, opened, text, 0);

    if (drawn != nullptr)
    {
        TTF_SetTextColor(drawn, (Uint8)(color >> 16), (Uint8)(color >> 8), (Uint8)color, (Uint8)(color >> 24));
        TTF_DrawSurfaceText(drawn, (int)SDL_roundf(x * surface->scale), (int)SDL_roundf(y * surface->scale), target);
        TTF_GetTextSize(drawn, &width, nullptr);
        TTF_DestroyText(drawn);
    }

    SDL_DestroySurface(target);
    return (f32)width / surface->scale;
}

static void DrawMeasure(void *font, const char *text, f32 size, f32 *retWidth, f32 *retLineHeight)
{
    TTF_Font *opened = DrawFindOrOpenSize(font, size);
    int width = 0;

    if (opened != nullptr)
    {
        TTF_GetStringSize(opened, text, 0, &width, nullptr);
    }

    *retWidth = (f32)width;
    *retLineHeight = opened == nullptr ? 0.0f : (f32)TTF_GetFontHeight(opened);
}

#pragma endregion Text

#pragma region Clipping

/// @brief Finds the clip of a surface, or nullptr.
static DrawClip *DrawFindClip(const ECSSurface *surface)
{
    for (usz i = 0; i < arrlenu(DRAW.clips); i++)
    {
        if (DRAW.clips[i].surface == surface)
        {
            return &DRAW.clips[i];
        }
    }

    return nullptr;
}

/// @brief Draws only inside a rectangle while a function runs, and inside the clip around it, if there is one. The clip before comes back when the function returns, also when it fails.
static void DrawClipTo(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, void (*body)(void))
{
    if (body == nullptr)
    {
        return;
    }

    DrawClip *outer = DrawFindClip(surface);
    bool hadOuter = outer != nullptr;
    SDL_Rect before = hadOuter ? outer->rect : (SDL_Rect){0, 0, surface->width, surface->height};
    SDL_Rect rect = DrawToPixels(surface->scale, x, y, width, height);

    // an empty overlap leaves nothing to draw into
    if (!SDL_GetRectIntersection(&rect, &before, &rect))
    {
        rect = (SDL_Rect){0};
    }

    if (hadOuter)
    {
        outer->rect = rect;
    }
    else
    {
        arrput(DRAW.clips, ((DrawClip){.surface = surface, .rect = rect}));
    }

    // an error in the function is reported, and the clip still ends here
    body();

    // the function may have clipped the surface itself, which moved the clips, so the clip is found again
    DrawClip *clip = DrawFindClip(surface);

    if (clip != nullptr && hadOuter)
    {
        clip->rect = before;
    }
    else if (clip != nullptr)
    {
        arrdelswap(DRAW.clips, (usz)(clip - DRAW.clips));
    }
}

#pragma endregion Clipping

/// @brief Frees the fonts and the text engine, and stops SDL_ttf. The Destroy function of the plugin.
static void DrawFree(void *data)
{
    (void)data;

    for (usz i = 0; i < arrlenu(DRAW.fonts); i++)
    {
        DrawCloseSizes(DRAW.fonts[i]);
        SDL_free(DRAW.fonts[i]->name);
        SDL_free(DRAW.fonts[i]->path);
        SDL_free(DRAW.fonts[i]);
    }

    ECSEvent_Unsubscribe(&DRAW.settingChanged);

    if (DRAW.engine != nullptr)
    {
        TTF_DestroySurfaceTextEngine(DRAW.engine);
    }

    // SDL_ttf counts its starts, so the core's own use goes on
    if (DRAW.ttfStarted)
    {
        TTF_Quit();
    }

    arrfree(DRAW.fonts);
    arrfree(DRAW.clips);
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
    SHU_ReturnResult(ECSEvent_Subscribe(plugin, &DRAW.settingChanged, "ecs.settingChanged", DrawSettingChanged, nullptr), DrawFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "font", nullptr), DrawFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "image", DrawImageDestroy), DrawFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"fill", (ECSFunction)DrawFill, "void(handle<ecs.surface> surface, float x, float y, float width, float height, color color)",
         "Fill a rectangle\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units\n"
         "@param color The colour, blended by its alpha"},
        {"outline", (ECSFunction)DrawOutline, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float thickness, color color)",
         "Draw the edge of a rectangle\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units\n"
         "@param thickness The edge's thickness inward, in layout units; at least a pixel\n"
         "@param color The colour, blended by its alpha"},
        {"line", (ECSFunction)DrawLine, "void(handle<ecs.surface> surface, float x1, float y1, float x2, float y2, float thickness, color color)",
         "Draw a line\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param x1 The start's x, in layout units\n"
         "@param y1 The start's y, in layout units\n"
         "@param x2 The end's x, in layout units; an end at the start draws nothing\n"
         "@param y2 The end's y, in layout units\n"
         "@param thickness The line's thickness, in layout units; at least a pixel\n"
         "@param color The colour, blended by its alpha"},
        {"circle", (ECSFunction)DrawCircle, "void(handle<ecs.surface> surface, float x, float y, float radius, float thickness, color color)",
         "Draw a circle: a ring of a thickness, or filled with a thickness of 0\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param x The centre's x, in layout units\n"
         "@param y The centre's y, in layout units\n"
         "@param radius The radius, in layout units\n"
         "@param thickness The ring's thickness inward from the edge, in layout units, and at least a pixel; 0 or less fills the circle\n"
         "@param color The colour, blended by its alpha"},
        {"rounded", (ECSFunction)DrawRoundedFill, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float radius, color color)",
         "Fill a rectangle with corners rounded by a radius\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units\n"
         "@param radius The corners' radius, in layout units; at most half the shorter side\n"
         "@param color The colour, blended by its alpha"},
        {"roundedOutline", (ECSFunction)DrawRoundedOutline, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float radius, float thickness, color color)",
         "Draw the edge of a rectangle with corners rounded by a radius\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units\n"
         "@param radius The corners' radius, in layout units; at most half the shorter side\n"
         "@param thickness The edge's thickness inward, in layout units; at least a pixel\n"
         "@param color The colour, blended by its alpha"},
        {"triangles", (ECSFunction)DrawTriangles, "void(handle<ecs.surface> surface, buffer points, buffer indices, color color)",
         "Fill triangles: points are x and y floats, and indices name three points for each triangle, or are empty for the points in threes\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param points The points, an x and a y float each, in layout units\n"
         "@param indices Three 32-bit point numbers for each triangle, counted from 0; a triangle that names a missing point is skipped\n"
         "@param color The colour, blended by its alpha"},
        {"lines", (ECSFunction)DrawLines, "void(handle<ecs.surface> surface, buffer points, buffer indices, float thickness, color color)",
         "Draw lines: points are x and y floats, and indices name two points for each line, or are empty for one line through the points\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param points The points, an x and a y float each, in layout units\n"
         "@param indices Two 32-bit point numbers for each line, counted from 0; a line that names a missing point is skipped\n"
         "@param thickness The lines' thickness, in layout units; at least a pixel\n"
         "@param color The colour, blended by its alpha"},
        {"newImage", (ECSFunction)DrawNewImage, "handle<draw.image>(int width, int height, buffer pixels)",
         "Make an image from pixels, ARGB in native byte order, row after row; empty pixels make it transparent\n"
         "@param width The image's width, in pixels; more than 0\n"
         "@param height The image's height, in pixels; more than 0\n"
         "@param pixels Four bytes for each pixel, not premultiplied, or none; they are copied\n"
         "@return The image, or nothing if the pixels do not match the size or memory runs out"},
        {"imageSize", (ECSFunction)DrawImageSize, "void(handle<draw.image> image, out int width, out int height)",
         "Give an image's size in pixels\n"
         "@param image The image\n"
         "@param width Gets the image's width\n"
         "@param height Gets the image's height"},
        {"imagePixel", (ECSFunction)DrawImagePixel, "color(handle<draw.image> image, int x, int y)",
         "Give the colour of a pixel of an image\n"
         "@param image The image\n"
         "@param x The pixel's column, from 0 at the left\n"
         "@param y The pixel's row, from 0 at the top\n"
         "@return The pixel's colour, not premultiplied, or 0, transparent, outside the image"},
        {"imagePixels", (ECSFunction)DrawImagePixels, "buffer(handle<draw.image> image)",
         "Give an image's pixels, ARGB in native byte order, row after row; they stay the image's, so C reads them only while the image lives\n"
         "@param image The image\n"
         "@return The pixels, four bytes each, not premultiplied"},
        {"freeImage", (ECSFunction)DrawFreeImage, "void(handle<draw.image> image)",
         "Free an image now, instead of when no one uses its handle\n"
         "@param image The image; its handle is not used again"},
        {"image", (ECSFunction)DrawDrawImage, "void(handle<ecs.surface> surface, handle<draw.image> image, float x, float y, float width, float height)",
         "Draw an image stretched to a rectangle\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param image The image, blended by its alpha\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units"},
        {"imageTransformed", (ECSFunction)DrawImageTransformed, "void(handle<ecs.surface> surface, handle<draw.image> image, float x, float y, float width, float height, float angle, color color)",
         "Draw an image stretched to a rectangle, turned clockwise by an angle in degrees around its centre, with each pixel's colour multiplied by a colour\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param image The image, blended by its alpha\n"
         "@param x The rectangle's left before it turns, in layout units\n"
         "@param y The rectangle's top before it turns, in layout units\n"
         "@param width The rectangle's width, in layout units; 0 or less draws nothing\n"
         "@param height The rectangle's height, in layout units; 0 or less draws nothing\n"
         "@param angle The turn, in degrees; a negative angle turns it anticlockwise\n"
         "@param color Multiplies each pixel's colour: white leaves it as it is, and a lower alpha fades it"},
        {"font", (ECSFunction)DrawFindOrAddFont, "handle<draw.font>(string name)",
         "Give a font: \"ui\", the interface's; \"mono\", a monospace one; or the path of a TrueType file\n"
         "@param name \"ui\", \"mono\" or a path; a relative path starts at the executable's folder\n"
         "@return The font, the same handle for the same name, or nothing if memory runs out; a file that cannot be opened is reported when text is drawn"},
        {"fontFile", (ECSFunction)DrawFontFile, "string(handle<draw.font> font)",
         "Give the file a font is read from; a setting that names another file changes it\n"
         "@param font The font\n"
         "@return The file's path; it stays the font's, so C reads it only until a font setting changes"},
        {"text", (ECSFunction)DrawText, "float(handle<ecs.surface> surface, handle<draw.font> font, string text, float x, float y, float size, color color)",
         "Draw text with its line's top left at a position, and give its width\n"
         "@param surface The panel's surface, as its Draw gives it; draw draws only into pixels surfaces\n"
         "@param font The font\n"
         "@param text The text, in UTF-8\n"
         "@param x The line's left, in layout units\n"
         "@param y The line's top, in layout units\n"
         "@param size The font size, in layout units\n"
         "@param color The colour, blended by its alpha\n"
         "@return The text's width, in layout units, or 0 if nothing is drawn"},
        {"measure", (ECSFunction)DrawMeasure, "void(handle<draw.font> font, string text, float size, out float width, out float lineHeight)",
         "Give the width and the line height of a text\n"
         "@param font The font\n"
         "@param text The text, in UTF-8\n"
         "@param size The font size; the width and the line height are in the same units\n"
         "@param width Gets the text's width, or 0 if the font cannot be opened\n"
         "@param lineHeight Gets the font's line height, or 0 if the font cannot be opened"},
        {"clip", (ECSFunction)DrawClipTo, "void(handle<ecs.surface> surface, float x, float y, float width, float height, fn<void()> body)",
         "Draw only inside a rectangle while a function runs\n"
         "@param surface The panel's surface, as its Draw gives it\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units\n"
         "@param body Draws inside the rectangle, and inside a clip around this one; the clip ends when it returns, also when it fails, whose error is reported"},
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
