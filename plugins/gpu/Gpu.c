// The gpu standard plugin: draws into the GPU surface of another plugin's panel, on the GPU (DESIGN 15).
// A panel's Draw passes its surface to gpu's functions, as to draw's, and gpu sends what they drew to the GPU when the Draw returns. It draws shapes,
// lines, textures and text as draw does, with an offscreen SDL renderer of its own on OpenECS's GPU device, and vertex buffers with a plugin's own
// shaders, through pipelines, with SDL's GPU API. It compiles shaders from GLSL with glslang, to SPIR-V, which Vulkan takes, and turns SPIR-V into
// DXBC for Direct3D 12, through HLSL from SPIRV-Cross and Windows' own HLSL compiler.
// It holds no scene: the plugin that draws keeps its buffers and says what to draw each frame.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

#include "glslang/Include/glslang_c_interface.h"
#include "glslang/Public/resource_limits_c.h"
#include "spirv_cross_c.h"

#ifdef _WIN32
#include <d3dcompiler.h>
#endif

#pragma region Source Only

/// @brief How many depth textures are kept, one for each size of the surfaces drawn into lately.
#define GPU_DEPTHS 4

/// @brief The most vertex attributes a pipeline takes.
#define GPU_ATTRIBUTES 16

/// @brief The width and height of each texture that the text engine keeps glyphs in.
#define GPU_ATLAS_SIZE 1024

/// @brief Drawing into one GPU surface during its Draw, until the core says the Draw returned. Shapes go through the renderer; pipelines draw in a
/// render pass of their own.
typedef struct GpuFrame
{
    ECSSurface *surface;            // the surface it draws into
    struct GpuFrame *next;          // the frame of another surface whose Draw runs, such as a panel's while one of its popups draws
    SDL_Texture *target;            // the surface's texture, which the renderer draws into without owning it
    SDL_GPUTexture *color;          // the same texture, which render passes draw into
    i32 width;                      // the surface's size in pixels
    i32 height;
    f32 scale;                      // the surface's pixels per layout unit
    SDL_GPUCommandBuffer *commands; // the command buffer of the open render pass, or nullptr
    SDL_GPURenderPass *pass;        // the render pass that pipelines draw in, or nullptr
    bool depthCleared;              // the frame's first render pass cleared the depth texture
} GpuFrame;

/// @brief A buffer on the GPU of vertices or of indices, 32-bit numbers.
typedef struct GpuBuffer
{
    SDL_GPUBuffer *buffer;
    u32 size; // in bytes
    bool index;
} GpuBuffer;

/// @brief A font file opened at a size in pixels, for the text engine.
typedef struct GpuFont
{
    char *file;
    i32 pixels;
    TTF_Font *font;
} GpuFont;

/// @brief A texture of the text engine's glyphs, and the renderer's view of it.
typedef struct GpuAtlas
{
    SDL_GPUTexture *glyphs;
    SDL_Texture *texture;
} GpuAtlas;

/// @brief A graphics pipeline: shaders, how vertices are laid out, and how triangles are drawn.
typedef struct GpuPipeline
{
    SDL_GPUGraphicsPipeline *pipeline;
    u32 pitch; // bytes from one vertex to the next
} GpuPipeline;

static struct
{
    ECSPlugin plugin;
    SDL_GPUDevice *device;  // OpenECS's GPU device, or nullptr without a GPU renderer
    SDL_Renderer *renderer; // an offscreen GPU renderer on the device, or nullptr without a GPU renderer
    GpuFrame *frames;       // the surfaces being drawn into now
    bool rendered;          // the renderer has commands that are not sent yet, such as shapes or a texture's pixels
    SDL_GPUSampler *sampler;
    struct
    {
        i32 width;
        i32 height;
        SDL_GPUTexture *texture;
    } depths[GPU_DEPTHS]; // depth textures by size
    usz nextDepth;        // the one a new size replaces
    bool ttfStarted;
    TTF_TextEngine *textEngine;                // keeps the glyphs of text in textures on the device, or nullptr without a GPU renderer
    const char *(*FontFile)(void *font);       // draw.fontFile, which gives the file of a draw.font
    GpuFont *fonts;                            // every font file and size that text was drawn in
    usz fontCount;
    GpuAtlas *atlases;                         // the text engine's textures, as the renderer sees them
    usz atlasCount;
    bool glslangStarted;
    SDL_SharedObject *d3dcompiler; // Windows' HLSL compiler, d3dcompiler_47.dll, loaded the first time a shader needs DXBC
    void *compiled;                // the SPIR-V that gpu.compile gave last, kept until its next call
    char *compileError;            // why gpu.compile failed last, kept until its next call
} GPU = {0};

/// @brief Frees the renderer; the plugin's Destroy, after the handles of its frames and textures are gone.
static void GpuFree(void *data)
{
    (void)data;

    // the renderer's views of the glyph textures go before the text engine, which owns the textures
    for (usz i = 0; i < GPU.atlasCount; i++)
    {
        SDL_DestroyTexture(GPU.atlases[i].texture);
    }

    for (usz i = 0; i < GPU.fontCount; i++)
    {
        TTF_CloseFont(GPU.fonts[i].font);
        SDL_free(GPU.fonts[i].file);
    }

    if (GPU.textEngine != nullptr)
    {
        TTF_DestroyGPUTextEngine(GPU.textEngine);
    }

    if (GPU.ttfStarted)
    {
        TTF_Quit();
    }

    if (GPU.glslangStarted)
    {
        glslang_finalize_process();
    }

    if (GPU.d3dcompiler != nullptr)
    {
        SDL_UnloadObject(GPU.d3dcompiler);
    }

    SDL_free(GPU.compiled);
    SDL_free(GPU.compileError);
    GPU.compiled = nullptr;
    GPU.compileError = nullptr;
    GPU.glslangStarted = false;
    GPU.d3dcompiler = nullptr;

    SDL_free(GPU.atlases);
    SDL_free(GPU.fonts);
    GPU.atlases = nullptr;
    GPU.atlasCount = 0;
    GPU.fonts = nullptr;
    GPU.fontCount = 0;
    GPU.textEngine = nullptr;
    GPU.ttfStarted = false;

    for (usz i = 0; i < GPU_DEPTHS; i++)
    {
        if (GPU.depths[i].texture != nullptr)
        {
            SDL_ReleaseGPUTexture(GPU.device, GPU.depths[i].texture);
        }
    }

    if (GPU.sampler != nullptr)
    {
        SDL_ReleaseGPUSampler(GPU.device, GPU.sampler);
    }

    if (GPU.renderer != nullptr)
    {
        SDL_DestroyRenderer(GPU.renderer);
    }

    SDL_zero(GPU.depths);
    GPU.nextDepth = 0;
    GPU.sampler = nullptr;
    GPU.renderer = nullptr;
}

/// @brief Ends a frame's render pass and sends its commands, so shapes drawn after it come after it.
static void GpuEndPass(GpuFrame *frame)
{
    if (frame->pass != nullptr)
    {
        SDL_EndGPURenderPass(frame->pass);
        SDL_SubmitGPUCommandBuffer(frame->commands);
    }

    frame->pass = nullptr;
    frame->commands = nullptr;
}

/// @brief Sends what the renderer drew, and the pixels it uploaded, to the GPU, before later commands.
static void GpuSendRendered(void)
{
    if (GPU.rendered)
    {
        // presenting the offscreen renderer submits its commands
        SDL_SetRenderTarget(GPU.renderer, nullptr);
        SDL_RenderPresent(GPU.renderer);
    }

    GPU.rendered = false;
}

/// @brief Draws shapes into a frame's texture from now on, after what its pipelines drew, with a colour, and blends by its alpha.
static bool GpuTarget(GpuFrame *frame, ECSColor color)
{
    GpuEndPass(frame);
    GPU.rendered = true;
    SDL_SetRenderTarget(GPU.renderer, frame->target);
    return SDL_SetRenderDrawColor(GPU.renderer, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) & 0xFF);
}

static void GpuTextureDestroy(void *object)
{
    SDL_DestroyTexture(object);
}

/// @brief Ends drawing into a surface once its Draw returns: what it drew reaches the GPU before the core's renderer shows the texture.
static void GpuFinish(ECSSurface *surface, void *data)
{
    (void)surface;
    GpuFrame *frame = data;
    GpuEndPass(frame);
    GpuSendRendered();

    for (GpuFrame **link = &GPU.frames; *link != nullptr; link = &(*link)->next)
    {
        if (*link == frame)
        {
            *link = frame->next;
            break;
        }
    }

    SDL_DestroyTexture(frame->target);
    SDL_free(frame);
}

/// @brief Gives the frame of a GPU surface, which the first call of a Draw that draws into it starts; the core ends it when the Draw returns.
/// @return The frame, or nullptr if the surface is not a GPU surface or OpenECS has no GPU renderer, which is logged.
static GpuFrame *GpuFrameOf(ECSSurface *surface)
{
    for (GpuFrame *frame = GPU.frames; frame != nullptr; frame = frame->next)
    {
        if (frame->surface == surface)
        {
            return frame;
        }
    }

    if (GPU.renderer == nullptr || surface->type != ECSSurfaceType_Gpu || surface->texture == nullptr)
    {
        ECS_LogWarn(GPU.plugin, GPU.renderer == nullptr ? "gpu cannot draw: OpenECS has no GPU renderer." : "gpu draws only into GPU surfaces.");
        return nullptr;
    }

    // the renderer views the panel's texture, B8G8R8A8_UNORM, which is ARGB8888, as a target of its own
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ARGB8888);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, surface->width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, surface->height);
    SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, surface->texture);
    SDL_Texture *target = props == 0 ? nullptr : SDL_CreateTextureWithProperties(GPU.renderer, props);
    SDL_DestroyProperties(props);
    GpuFrame *frame = target == nullptr ? nullptr : SDL_calloc(1, sizeof(GpuFrame));

    if (frame == nullptr || ECSSurface_OnDrawn(surface, GpuFinish, frame))
    {
        ECS_LogError(GPU.plugin, "Cannot draw into a GPU surface: %s", SDL_GetError());
        SDL_free(frame);
        SDL_DestroyTexture(target);
        return nullptr;
    }

    frame->surface = surface;
    frame->target = target;
    frame->color = surface->texture;
    frame->width = surface->width;
    frame->height = surface->height;
    frame->scale = surface->scale;
    frame->next = GPU.frames;
    GPU.frames = frame;
    return frame;
}

static void GpuClear(ECSSurface *surface, ECSColor color)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    // clearing replaces the pixels, whatever the blend mode
    GpuTarget(frame, color);
    SDL_RenderClear(GPU.renderer);
}

static void GpuFill(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, ECSColor color)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    GpuTarget(frame, color);
    const SDL_FRect rect = {x * frame->scale, y * frame->scale, width * frame->scale, height * frame->scale};
    SDL_RenderFillRect(GPU.renderer, &rect);
}

/// @brief Fills triangles of one colour: vertices of x and y in pixels, and indices, three for each triangle, or none for the vertices in threes.
static void GpuRenderTriangles(const f32 *xy, int count, const int *indices, int indexCount, ECSColor color)
{
    const SDL_FColor fcolor = {(f32)((color >> 16) & 0xFF) / 255.0f, (f32)((color >> 8) & 0xFF) / 255.0f, (f32)(color & 0xFF) / 255.0f, (f32)((color >> 24) & 0xFF) / 255.0f};
    SDL_RenderGeometryRaw(GPU.renderer, nullptr, xy, 2 * sizeof(f32), &fcolor, 0, nullptr, 0, count, indices, indexCount, sizeof(int));
}

static void GpuLine(ECSSurface *surface, f32 x1, f32 y1, f32 x2, f32 y2, f32 thickness, ECSColor color)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    GpuTarget(frame, color);
    f32 s = frame->scale;
    f32 dx = (x2 - x1) * s;
    f32 dy = (y2 - y1) * s;
    f32 length = SDL_sqrtf(dx * dx + dy * dy);

    if (length <= 0.0f)
    {
        return;
    }

    // a line is a rectangle around it, of its thickness, in two triangles
    f32 nx = -dy / length * thickness * s / 2.0f;
    f32 ny = dx / length * thickness * s / 2.0f;
    const f32 xy[] = {x1 * s + nx, y1 * s + ny, x2 * s + nx, y2 * s + ny, x2 * s - nx, y2 * s - ny, x1 * s - nx, y1 * s - ny};
    const int indices[] = {0, 1, 2, 0, 2, 3};
    GpuRenderTriangles(xy, 4, indices, 6, color);
}

static void GpuTriangles(ECSSurface *surface, SHUSlice points, SHUSlice indices, ECSColor color)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    usz count = points.size / (2 * sizeof(f32));
    usz indexCount = indices.size / sizeof(u32);

    if (count == 0 || count > (usz)SDL_MAX_SINT32 || indexCount > (usz)SDL_MAX_SINT32)
    {
        return;
    }

    // the points are in layout units, and the renderer takes pixels; indices that name no point leave their triangle out
    f32 *xy = SDL_malloc(count * 2 * sizeof(f32));
    int *index = indexCount > 0 ? SDL_malloc((indexCount - indexCount % 3) * sizeof(int)) : nullptr;
    usz used = 0;

    for (usz i = 0; xy != nullptr && i < count * 2; i++)
    {
        xy[i] = ((const f32 *)points.data)[i] * frame->scale;
    }

    for (usz i = 0; index != nullptr && i + 2 < indexCount; i += 3)
    {
        const u32 *triangle = (const u32 *)indices.data + i;

        if (triangle[0] < count && triangle[1] < count && triangle[2] < count)
        {
            index[used++] = (int)triangle[0];
            index[used++] = (int)triangle[1];
            index[used++] = (int)triangle[2];
        }
    }

    if (xy != nullptr && (indexCount == 0 || index != nullptr))
    {
        GpuTarget(frame, color);
        GpuRenderTriangles(xy, (int)(count - (indexCount == 0 ? count % 3 : 0)), index, (int)used, color);
    }

    SDL_free(xy);
    SDL_free(index);
}

static void GpuMesh(ECSSurface *surface, SHUSlice points, SHUSlice colors, SHUSlice indices)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    usz count = points.size / (2 * sizeof(f32));
    usz indexCount = indices.size / sizeof(u32);

    if (count == 0 || colors.size / sizeof(u32) < count || count > (usz)SDL_MAX_SINT32 || indexCount > (usz)SDL_MAX_SINT32)
    {
        ECS_LogWarn(GPU.plugin, count == 0 ? "A mesh needs points." : "A mesh needs a colour for each of its points.");
        return;
    }

    // the points are in layout units, and the renderer takes pixels; each colour is ARGB, not premultiplied, as a colour text is, and blends across its triangles
    f32 *xy = SDL_malloc(count * 2 * sizeof(f32));
    SDL_FColor *fcolors = SDL_malloc(count * sizeof(SDL_FColor));
    int *index = indexCount > 0 ? SDL_malloc((indexCount - indexCount % 3) * sizeof(int)) : nullptr;
    usz used = 0;

    for (usz i = 0; xy != nullptr && fcolors != nullptr && i < count; i++)
    {
        u32 color = ((const u32 *)colors.data)[i];
        xy[2 * i] = ((const f32 *)points.data)[2 * i] * frame->scale;
        xy[2 * i + 1] = ((const f32 *)points.data)[2 * i + 1] * frame->scale;
        fcolors[i] = (SDL_FColor){(f32)((color >> 16) & 0xFF) / 255.0f, (f32)((color >> 8) & 0xFF) / 255.0f, (f32)(color & 0xFF) / 255.0f, (f32)((color >> 24) & 0xFF) / 255.0f};
    }

    // indices that name no point leave their triangle out
    for (usz i = 0; index != nullptr && i + 2 < indexCount; i += 3)
    {
        const u32 *triangle = (const u32 *)indices.data + i;

        if (triangle[0] < count && triangle[1] < count && triangle[2] < count)
        {
            index[used++] = (int)triangle[0];
            index[used++] = (int)triangle[1];
            index[used++] = (int)triangle[2];
        }
    }

    if (xy != nullptr && fcolors != nullptr && (indexCount == 0 || index != nullptr))
    {
        GpuTarget(frame, 0xFFFFFFFF);
        int drawn = (int)(count - (indexCount == 0 ? count % 3 : 0));
        SDL_RenderGeometryRaw(GPU.renderer, nullptr, xy, 2 * sizeof(f32), fcolors, sizeof(SDL_FColor), nullptr, 0, drawn, index, (int)used, sizeof(int));
    }

    SDL_free(xy);
    SDL_free(fcolors);
    SDL_free(index);
}

static SDL_Texture *GpuNewTexture(i32 width, i32 height, SHUSlice pixels)
{
    if (GPU.renderer == nullptr || width < 1 || height < 1 || pixels.size != (usz)width * (usz)height * 4)
    {
        ECS_LogWarn(GPU.plugin, GPU.renderer == nullptr ? "gpu cannot make a texture: OpenECS has no GPU renderer." : "A texture needs a size of at least 1 by 1, and 4 bytes of pixels for each pixel.");
        return nullptr;
    }

    // its pixels are ARGB with premultiplied alpha, as draw's images are, and blend so
    SDL_Texture *texture = SDL_CreateTexture(GPU.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, width, height);

    if (texture == nullptr || !SDL_UpdateTexture(texture, nullptr, pixels.data, width * 4))
    {
        ECS_LogError(GPU.plugin, "Cannot make a %dx%d texture: %s", width, height, SDL_GetError());
        SDL_DestroyTexture(texture);
        return nullptr;
    }

    // the upload is one of the renderer's commands, which goes before pipelines sample the texture
    GPU.rendered = true;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    return texture;
}

static bool GpuUpdateTexture(SDL_Texture *texture, SHUSlice pixels)
{
    if (pixels.size != (usz)texture->w * (usz)texture->h * 4)
    {
        ECS_LogWarn(GPU.plugin, "A %dx%d texture takes %d bytes of pixels.", texture->w, texture->h, texture->w * texture->h * 4);
        return false;
    }

    GPU.rendered = true;
    return SDL_UpdateTexture(texture, nullptr, pixels.data, texture->w * 4);
}

static void GpuTextureSize(SDL_Texture *texture, i32 *retWidth, i32 *retHeight)
{
    *retWidth = texture->w;
    *retHeight = texture->h;
}

static void GpuFreeTexture(SDL_Texture *texture)
{
    ECSHandle_Forget(texture);
    SDL_DestroyTexture(texture);
}

static void GpuImageTransformed(ECSSurface *surface, SDL_Texture *texture, f32 x, f32 y, f32 width, f32 height, f32 angle, ECSColor color)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    GpuTarget(frame, color);
    const SDL_FRect rect = {x * frame->scale, y * frame->scale, width * frame->scale, height * frame->scale};

    // the colour multiplies each pixel's, which are premultiplied, so its alpha fades them all
    f32 alpha = (f32)((color >> 24) & 0xFF) / 255.0f;
    SDL_SetTextureColorModFloat(texture, (f32)((color >> 16) & 0xFF) / 255.0f * alpha, (f32)((color >> 8) & 0xFF) / 255.0f * alpha, (f32)(color & 0xFF) / 255.0f * alpha);
    SDL_SetTextureAlphaModFloat(texture, alpha);
    SDL_RenderTextureRotated(GPU.renderer, texture, nullptr, &rect, angle, nullptr, SDL_FLIP_NONE);
}

static void GpuImage(ECSSurface *surface, SDL_Texture *texture, f32 x, f32 y, f32 width, f32 height)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    GpuImageTransformed(surface, texture, x, y, width, height, 0.0f, 0xFFFFFFFF);
}

#pragma region Text

/// @brief Gets a font file at a size in pixels, opening it the first time.
/// @return The font, or nullptr if it cannot be opened; the reason is logged.
static TTF_Font *GpuGetFont(const char *file, f32 pixels)
{
    i32 size = (i32)SDL_max(1.0f, SDL_roundf(pixels));

    for (usz i = 0; i < GPU.fontCount; i++)
    {
        if (GPU.fonts[i].pixels == size && SDL_strcmp(GPU.fonts[i].file, file) == 0)
        {
            return GPU.fonts[i].font;
        }
    }

    TTF_Font *opened = TTF_OpenFont(file, (float)size);
    char *copy = opened == nullptr ? nullptr : SDL_strdup(file);
    GpuFont *fonts = copy == nullptr ? nullptr : SDL_realloc(GPU.fonts, (GPU.fontCount + 1) * sizeof(GpuFont));

    if (fonts == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot open the font '%s' at %d pixels: %s", file, size, SDL_GetError());
        SDL_free(copy);
        TTF_CloseFont(opened);
        return nullptr;
    }

    GPU.fonts = fonts;
    GPU.fonts[GPU.fontCount++] = (GpuFont){.file = copy, .pixels = size, .font = opened};
    return opened;
}

/// @brief Gives the renderer's view of a texture of the text engine's glyphs, made the first time; nullptr for none, which draws in one colour.
static SDL_Texture *GpuGetAtlas(SDL_GPUTexture *glyphs)
{
    if (glyphs == nullptr)
    {
        return nullptr;
    }

    for (usz i = 0; i < GPU.atlasCount; i++)
    {
        if (GPU.atlases[i].glyphs == glyphs)
        {
            return GPU.atlases[i].texture;
        }
    }

    // the glyphs are white ARGB, B8G8R8A8_UNORM on the GPU, with straight alpha, so the text's colour multiplies them
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ARGB8888);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, GPU_ATLAS_SIZE);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, GPU_ATLAS_SIZE);
    SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, glyphs);
    SDL_Texture *texture = props == 0 ? nullptr : SDL_CreateTextureWithProperties(GPU.renderer, props);
    SDL_DestroyProperties(props);
    GpuAtlas *atlases = texture == nullptr ? nullptr : SDL_realloc(GPU.atlases, (GPU.atlasCount + 1) * sizeof(GpuAtlas));

    if (atlases == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot draw from the text's glyphs: %s", SDL_GetError());
        SDL_DestroyTexture(texture);
        return nullptr;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    GPU.atlases = atlases;
    GPU.atlases[GPU.atlasCount++] = (GpuAtlas){.glyphs = glyphs, .texture = texture};
    return texture;
}

/// @brief Draws text with SDL3_ttf's GPU text engine, which keeps its glyphs in textures on the device: the renderer draws their rectangles from
/// those textures, after the shapes drawn before, so text and shapes keep their order.
static f32 GpuText(ECSSurface *surface, void *font, const char *text, f32 x, f32 y, f32 size, ECSColor color)
{
    GpuFrame *frame = text[0] == '\0' || GPU.textEngine == nullptr ? nullptr : GpuFrameOf(surface);
    TTF_Font *opened = frame == nullptr ? nullptr : GpuGetFont(GPU.FontFile(font), size * frame->scale);
    TTF_Text *drawn = opened == nullptr ? nullptr : TTF_CreateText(GPU.textEngine, opened, text, 0);
    int width = 0;

    if (drawn != nullptr)
    {
        GpuTarget(frame, color);
        const SDL_FColor fcolor = {(f32)((color >> 16) & 0xFF) / 255.0f, (f32)((color >> 8) & 0xFF) / 255.0f, (f32)(color & 0xFF) / 255.0f, (f32)((color >> 24) & 0xFF) / 255.0f};
        f32 left = SDL_roundf(x * frame->scale);
        f32 top = SDL_roundf(y * frame->scale);

        // the engine's y goes up from the line's top, as the GPU's does, and the surface's goes down
        for (TTF_GPUAtlasDrawSequence *sequence = TTF_GetGPUTextDrawData(drawn); sequence != nullptr; sequence = sequence->next)
        {
            SDL_FPoint *points = SDL_malloc((usz)sequence->num_vertices * sizeof(SDL_FPoint));

            if (points == nullptr)
            {
                break;
            }

            for (int i = 0; i < sequence->num_vertices; i++)
            {
                points[i] = (SDL_FPoint){left + sequence->xy[i].x, top - sequence->xy[i].y};
            }

            SDL_RenderGeometryRaw(GPU.renderer, GpuGetAtlas(sequence->atlas_texture), &points->x, sizeof(SDL_FPoint), &fcolor, 0, &sequence->uv->x, sizeof(SDL_FPoint),
                                  sequence->num_vertices, sequence->indices, sequence->num_indices, sizeof(int));
            SDL_free(points);
        }

        TTF_GetTextSize(drawn, &width, nullptr);
    }

    TTF_DestroyText(drawn);
    return frame == nullptr ? 0.0f : (f32)width / frame->scale;
}

/// @brief Fills a part of a glyphs value: the number of a texture of the text engine, and the vertices and indices of the text's glyphs in it.
static SHUResult GpuFillGlyphPart(ECSValue *part, usz atlas, const TTF_GPUAtlasDrawSequence *sequence)
{
    // a vertex is x, y, u and v, four floats; the engine's y goes up from the line's top, and the vertices' y goes down, as the surface's does
    usz vertexBytes = (usz)sequence->num_vertices * 4 * sizeof(f32);
    usz indexBytes = (usz)sequence->num_indices * sizeof(u32);
    f32 *vertices = SDL_malloc(vertexBytes);
    u32 *indices = SDL_malloc(indexBytes);
    ECSValue *field = nullptr;
    SHUResult result = vertices == nullptr || indices == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;

    for (int i = 0; result == SHUResult_Ok && i < sequence->num_vertices; i++)
    {
        vertices[i * 4 + 0] = sequence->xy[i].x;
        vertices[i * 4 + 1] = -sequence->xy[i].y;
        vertices[i * 4 + 2] = sequence->uv[i].x;
        vertices[i * 4 + 3] = sequence->uv[i].y;
    }

    for (int i = 0; result == SHUResult_Ok && i < sequence->num_indices; i++)
    {
        indices[i] = (u32)sequence->indices[i];
    }

    if (result == SHUResult_Ok)
    {
        ECSValue_SetTable(part);
        result = ECSValue_TableSetField(part, "atlas", &field);
    }

    if (result == SHUResult_Ok)
    {
        ECSValue_SetInteger(field, (i64)atlas);
        result = ECSValue_TableSetField(part, "count", &field);
    }

    if (result == SHUResult_Ok)
    {
        ECSValue_SetInteger(field, sequence->num_indices);
        result = ECSValue_TableSetField(part, "vertices", &field);
    }

    result = result ? result : ECSValue_SetBuffer(field, (SHUSliceView){vertices, vertexBytes});
    result = result ? result : ECSValue_TableSetField(part, "indices", &field);
    result = result ? result : ECSValue_SetBuffer(field, (SHUSliceView){indices, indexBytes});
    SDL_free(vertices);
    SDL_free(indices);
    return result;
}

/// @brief Fills a glyphs value: the text's size, and a part for each texture of the text engine that holds some of its glyphs.
static SHUResult GpuFillGlyphs(TTF_Text *drawn, ECSValue *retGlyphs)
{
    int width = 0;
    int height = 0;
    ECSValue *field = nullptr;
    ECSValue *parts = nullptr;
    TTF_GetTextSize(drawn, &width, &height);
    ECSValue_SetTable(retGlyphs);
    SHU_ReturnResult(ECSValue_TableSetField(retGlyphs, "width", &field));
    ECSValue_SetNumber(field, width);
    SHU_ReturnResult(ECSValue_TableSetField(retGlyphs, "height", &field));
    ECSValue_SetNumber(field, height);
    SHU_ReturnResult(ECSValue_TableSetField(retGlyphs, "parts", &parts));
    ECSValue_SetTable(parts);

    for (TTF_GPUAtlasDrawSequence *sequence = TTF_GetGPUTextDrawData(drawn); sequence != nullptr; sequence = sequence->next)
    {
        // the engine's textures are numbered from 1 in the order gpu first meets them, and keep their numbers
        usz atlas = 0;
        GpuGetAtlas(sequence->atlas_texture);

        while (atlas < GPU.atlasCount && GPU.atlases[atlas].glyphs != sequence->atlas_texture)
        {
            atlas++;
        }

        ECSValue *part = nullptr;

        if (atlas == GPU.atlasCount)
        {
            return SHUResult_ErrInternal;
        }

        SHU_ReturnResult(ECSValue_ListAddItem(parts, &part));
        SHU_ReturnResult(GpuFillGlyphPart(part, atlas + 1, sequence));
    }

    return SHUResult_Ok;
}

/// @brief Gives the glyphs of a text for a plugin's own pipeline: the rectangles of its glyphs in the text engine's textures, which a draw samples by number.
static void GpuGlyphs(void *font, const char *text, f32 size, ECSValue *retGlyphs)
{
    TTF_Font *opened = text[0] == '\0' || GPU.textEngine == nullptr ? nullptr : GpuGetFont(GPU.FontFile(font), size);
    TTF_Text *drawn = opened == nullptr ? nullptr : TTF_CreateText(GPU.textEngine, opened, text, 0);

    if (drawn == nullptr || GpuFillGlyphs(drawn, retGlyphs) != SHUResult_Ok)
    {
        ECSValue_SetNil(retGlyphs);
    }

    TTF_DestroyText(drawn);
}

#pragma endregion Text

#pragma region Pipelines

/// @brief Copies bytes into a GPU buffer, with a transfer buffer and a copy pass of their own, which reach the GPU before later draws.
static bool GpuUpload(SDL_GPUBuffer *buffer, SHUSlice bytes)
{
    const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)bytes.size};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(GPU.device, &info);
    void *mapped = transfer == nullptr ? nullptr : SDL_MapGPUTransferBuffer(GPU.device, transfer, false);
    SDL_GPUCommandBuffer *commands = mapped == nullptr ? nullptr : SDL_AcquireGPUCommandBuffer(GPU.device);

    if (commands == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot upload %zu bytes to the GPU: %s", bytes.size, SDL_GetError());

        if (mapped != nullptr)
        {
            SDL_UnmapGPUTransferBuffer(GPU.device, transfer);
        }

        SDL_ReleaseGPUTransferBuffer(GPU.device, transfer);
        return false;
    }

    SDL_memcpy(mapped, bytes.data, bytes.size);
    SDL_UnmapGPUTransferBuffer(GPU.device, transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation source = {.transfer_buffer = transfer};
    const SDL_GPUBufferRegion destination = {.buffer = buffer, .size = (Uint32)bytes.size};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);

    // the device keeps the transfer buffer until the copy is done
    SDL_ReleaseGPUTransferBuffer(GPU.device, transfer);
    return true;
}

static void GpuBufferDestroy(void *object)
{
    GpuBuffer *buffer = object;
    SDL_ReleaseGPUBuffer(GPU.device, buffer->buffer);
    SDL_free(buffer);
}

/// @brief Gives a buffer new bytes; a buffer of another size is made again.
static bool GpuFillBuffer(GpuBuffer *buffer, SHUSlice bytes)
{
    if (bytes.size == 0 || bytes.size > SDL_MAX_UINT32 || (buffer->index && bytes.size % 4 != 0))
    {
        ECS_LogWarn(GPU.plugin, buffer->index ? "An index buffer needs 32-bit indices, 4 bytes each." : "A buffer needs bytes.");
        return false;
    }

    if (buffer->buffer == nullptr || buffer->size != bytes.size)
    {
        const SDL_GPUBufferCreateInfo info = {.usage = buffer->index ? SDL_GPU_BUFFERUSAGE_INDEX : SDL_GPU_BUFFERUSAGE_VERTEX, .size = (Uint32)bytes.size};
        SDL_GPUBuffer *made = SDL_CreateGPUBuffer(GPU.device, &info);

        if (made == nullptr)
        {
            ECS_LogError(GPU.plugin, "Cannot make a buffer of %zu bytes: %s", bytes.size, SDL_GetError());
            return false;
        }

        SDL_ReleaseGPUBuffer(GPU.device, buffer->buffer);
        buffer->buffer = made;
        buffer->size = (u32)bytes.size;
    }

    return GpuUpload(buffer->buffer, bytes);
}

static GpuBuffer *GpuNewBuffer(const char *usage, SHUSlice bytes)
{
    bool index = SDL_strcmp(usage, "index") == 0;

    if (GPU.device == nullptr || (!index && SDL_strcmp(usage, "vertex") != 0))
    {
        ECS_LogWarn(GPU.plugin, GPU.device == nullptr ? "gpu cannot make a buffer: OpenECS has no GPU renderer." : "A buffer is for \"vertex\" or \"index\" data, not '%s'.", usage);
        return nullptr;
    }

    GpuBuffer *buffer = SDL_calloc(1, sizeof(GpuBuffer));

    if (buffer != nullptr)
    {
        buffer->index = index;
    }

    // a buffer made on the GPU whose upload failed is released with it
    if (buffer == nullptr || !GpuFillBuffer(buffer, bytes))
    {
        if (buffer != nullptr && buffer->buffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(GPU.device, buffer->buffer);
        }

        SDL_free(buffer);
        return nullptr;
    }

    return buffer;
}

static bool GpuUpdateBuffer(GpuBuffer *buffer, SHUSlice bytes)
{
    return GpuFillBuffer(buffer, bytes);
}

static i32 GpuBufferSize(GpuBuffer *buffer)
{
    return (i32)buffer->size;
}

static void GpuFreeBuffer(GpuBuffer *buffer)
{
    ECSHandle_Forget(buffer);
    GpuBufferDestroy(buffer);
}

static void GpuShaderDestroy(void *object)
{
    SDL_ReleaseGPUShader(GPU.device, object);
}

/// @brief Compiles a vertex or fragment shader from GLSL to SPIR-V with glslang, for Vulkan 1.0. Gives the SPIR-V, which SDL_free frees, and its
/// size in bytes; or nullptr, and why, which SDL_free frees too.
static u32 *GpuCompileGlsl(const char *source, bool vertex, usz *retSize, char **retError)
{
    *retSize = 0;
    *retError = nullptr;

    if (!GPU.glslangStarted)
    {
        GPU.glslangStarted = glslang_initialize_process() != 0;
    }

    const glslang_input_t input = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = vertex ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = GLSLANG_TARGET_VULKAN_1_0,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_0,
        .code = source,
        .default_version = 450,
        .default_profile = GLSLANG_NO_PROFILE,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = glslang_default_resource(),
    };

    glslang_shader_t *shader = glslang_shader_create(&input);
    glslang_program_t *program = nullptr;
    u32 *spirv = nullptr;

    if (shader == nullptr || !glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input))
    {
        *retError = SDL_strdup(shader == nullptr ? "glslang cannot make a shader" : glslang_shader_get_info_log(shader));
    }
    else
    {
        program = glslang_program_create();
        glslang_program_add_shader(program, shader);

        if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
        {
            *retError = SDL_strdup(glslang_program_get_info_log(program));
        }
        else
        {
            glslang_program_SPIRV_generate(program, input.stage);
            usz words = glslang_program_SPIRV_get_size(program);
            spirv = SDL_malloc(words * sizeof(u32));

            if (spirv != nullptr)
            {
                glslang_program_SPIRV_get(program, spirv);
                *retSize = words * sizeof(u32);
            }
            else
            {
                *retError = SDL_strdup("there is not enough memory for the SPIR-V");
            }
        }
    }

    if (program != nullptr)
    {
        glslang_program_delete(program);
    }

    if (shader != nullptr)
    {
        glslang_shader_delete(shader);
    }

    return spirv;
}

/// @brief Counts the uniform blocks and the sampled textures of SPIR-V, as a shader's description may leave them out. Leaves them as they are if
/// SPIRV-Cross cannot read it.
static void GpuReflect(const u32 *spirv, usz size, u32 *uniforms, u32 *samplers)
{
    spvc_context context = nullptr;
    spvc_parsed_ir ir = nullptr;
    spvc_compiler compiler = nullptr;
    spvc_resources resources = nullptr;
    const spvc_reflected_resource *list = nullptr;
    usz count = 0;

    if (spvc_context_create(&context) != SPVC_SUCCESS)
    {
        return;
    }

    if (spvc_context_parse_spirv(context, spirv, size / sizeof(u32), &ir) == SPVC_SUCCESS &&
        spvc_context_create_compiler(context, SPVC_BACKEND_NONE, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) == SPVC_SUCCESS &&
        spvc_compiler_create_shader_resources(compiler, &resources) == SPVC_SUCCESS)
    {
        if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count) == SPVC_SUCCESS)
        {
            *uniforms = (u32)count;
        }

        if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &count) == SPVC_SUCCESS)
        {
            *samplers = (u32)count;
        }
    }

    spvc_context_destroy(context);
}

/// @brief Turns SPIR-V into HLSL of shader model 5.1 with SPIRV-Cross. Its uniform blocks and textures keep their sets as register spaces, as
/// SDL's GPU API sets them for Direct3D 12, and a vertex attribute of location n is TEXCOORDn. Gives the HLSL, which SDL_free frees, or nullptr.
static char *GpuHlsl(const u32 *spirv, usz size)
{
    spvc_context context = nullptr;
    spvc_parsed_ir ir = nullptr;
    spvc_compiler compiler = nullptr;
    spvc_compiler_options options = nullptr;
    const char *hlsl = nullptr;
    char *copy = nullptr;

    if (spvc_context_create(&context) != SPVC_SUCCESS)
    {
        return nullptr;
    }

    if (spvc_context_parse_spirv(context, spirv, size / sizeof(u32), &ir) == SPVC_SUCCESS &&
        spvc_context_create_compiler(context, SPVC_BACKEND_HLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) == SPVC_SUCCESS &&
        spvc_compiler_create_compiler_options(compiler, &options) == SPVC_SUCCESS)
    {
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, 51);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_NONWRITABLE_UAV_TEXTURE_AS_SRV, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_FLATTEN_MATRIX_VERTEX_INPUT_SEMANTICS, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_USE_ENTRY_POINT_NAME, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT, SPVC_TRUE);

        if (spvc_compiler_install_compiler_options(compiler, options) == SPVC_SUCCESS && spvc_compiler_compile(compiler, &hlsl) == SPVC_SUCCESS)
        {
            copy = SDL_strdup(hlsl);
        }
    }

    if (copy == nullptr)
    {
        ECS_LogError(GPU.plugin, "SPIRV-Cross cannot turn a shader into HLSL: %s", spvc_context_get_last_error_string(context));
    }

    spvc_context_destroy(context);
    return copy;
}

/// @brief Compiles HLSL of shader model 5.1 to DXBC with Windows' own compiler, d3dcompiler_47.dll, which Windows ships. Gives the DXBC, which
/// SDL_free frees, or nullptr; elsewhere than on Windows, always nullptr.
static void *GpuDxbc(const char *hlsl, const char *entry, bool vertex, usz *retSize)
{
    *retSize = 0;

#ifdef _WIN32
    if (GPU.d3dcompiler == nullptr)
    {
        GPU.d3dcompiler = SDL_LoadObject("d3dcompiler_47.dll");
    }

    pD3DCompile compile = GPU.d3dcompiler == nullptr ? nullptr : (pD3DCompile)(void *)SDL_LoadFunction(GPU.d3dcompiler, "D3DCompile");

    if (compile == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot load Windows' HLSL compiler, d3dcompiler_47.dll: %s", SDL_GetError());
        return nullptr;
    }

    ID3DBlob *code = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT result = compile(hlsl, SDL_strlen(hlsl), nullptr, nullptr, nullptr, entry, vertex ? "vs_5_1" : "ps_5_1", 0, 0, &code, &errors);
    void *dxbc = nullptr;

    if (result < 0 || code == nullptr)
    {
        ECS_LogError(GPU.plugin, "Windows' HLSL compiler cannot compile a shader: %s",
                     errors != nullptr ? (const char *)errors->lpVtbl->GetBufferPointer(errors) : "it does not tell why");
    }
    else
    {
        *retSize = code->lpVtbl->GetBufferSize(code);
        dxbc = SDL_malloc(*retSize);

        if (dxbc != nullptr)
        {
            SDL_memcpy(dxbc, code->lpVtbl->GetBufferPointer(code), *retSize);
        }
    }

    if (code != nullptr)
    {
        code->lpVtbl->Release(code);
    }

    if (errors != nullptr)
    {
        errors->lpVtbl->Release(errors);
    }

    return dxbc;
#else
    (void)hlsl;
    (void)entry;
    (void)vertex;
    return nullptr;
#endif
}

static SDL_GPUShader *GpuNewShader(const ECSValue *desc)
{
    if (GPU.device == nullptr)
    {
        ECS_LogWarn(GPU.plugin, "gpu cannot make a shader: OpenECS has no GPU renderer.");
        return nullptr;
    }

    const char *stage = ECSValue_GetString(ECSValue_GetTableField(desc, "stage"), "");
    bool vertex = SDL_strcmp(stage, "vertex") == 0;

    if (!vertex && SDL_strcmp(stage, "fragment") != 0)
    {
        ECS_LogWarn(GPU.plugin, "A shader's stage is \"vertex\" or \"fragment\".");
        return nullptr;
    }

    // code that the device's driver takes is used as it is: SPIR-V for Vulkan, DXIL for Direct3D 12, MSL for Metal; the first one given that it takes
    const struct
    {
        const char *field;
        SDL_GPUShaderFormat format;
    } kinds[] = {{"spirv", SDL_GPU_SHADERFORMAT_SPIRV}, {"dxil", SDL_GPU_SHADERFORMAT_DXIL}, {"msl", SDL_GPU_SHADERFORMAT_MSL}};
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(GPU.device);
    SHUSlice code = {0};
    SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;

    for (usz i = 0; i < SDL_arraysize(kinds) && format == SDL_GPU_SHADERFORMAT_INVALID; i++)
    {
        code = ECSValue_GetBuffer(ECSValue_GetTableField(desc, kinds[i].field), (SHUSlice){0});
        format = code.size > 0 && (formats & kinds[i].format) != 0 ? kinds[i].format : SDL_GPU_SHADERFORMAT_INVALID;
    }

    const char *entry = ECSValue_GetString(ECSValue_GetTableField(desc, "entry"), "main");
    const char *glsl = ECSValue_GetString(ECSValue_GetTableField(desc, "glsl"), nullptr);
    SHUSlice spirv = ECSValue_GetBuffer(ECSValue_GetTableField(desc, "spirv"), (SHUSlice){0});
    void *made = nullptr; // code that gpu made, from GLSL or from SPIR-V, which it frees
    u32 uniforms = 0, samplers = 0;

    // GLSL compiles to SPIR-V, which serves a driver that takes it as it is; it and SPIR-V that was given also tell the counts a description leaves out
    if (format == SDL_GPU_SHADERFORMAT_INVALID && glsl != nullptr)
    {
        usz size = 0;
        char *error = nullptr;
        made = GpuCompileGlsl(glsl, vertex, &size, &error);

        if (made == nullptr)
        {
            ECS_LogError(GPU.plugin, "Cannot compile a %s shader from GLSL: %s", stage, error != nullptr ? error : "glslang does not tell why");
            SDL_free(error);
            return nullptr;
        }

        spirv = cs(made, size);
        code = spirv;
        format = (formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0 ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_INVALID;
    }

    if (spirv.size > 0 && spirv.size % sizeof(u32) == 0)
    {
        u32 *words = SDL_malloc(spirv.size);

        if (words != nullptr)
        {
            SDL_memcpy(words, spirv.data, spirv.size);
            GpuReflect(words, spirv.size, &uniforms, &samplers);
        }

        // a driver that does not take SPIR-V, such as Direct3D 12's, gets DXBC made from it
        if (format == SDL_GPU_SHADERFORMAT_INVALID && words != nullptr && (formats & SDL_GPU_SHADERFORMAT_DXBC) != 0)
        {
            char *hlsl = GpuHlsl(words, spirv.size);
            usz size = 0;
            void *dxbc = hlsl == nullptr ? nullptr : GpuDxbc(hlsl, entry, vertex, &size);
            SDL_free(hlsl);

            if (dxbc != nullptr)
            {
                SDL_free(made);
                made = dxbc;
                code = cs(dxbc, size);
                format = SDL_GPU_SHADERFORMAT_DXBC;
            }
        }

        SDL_free(words);
    }

    if (format == SDL_GPU_SHADERFORMAT_INVALID)
    {
        ECS_LogWarn(GPU.plugin, "A shader needs code that the GPU's driver takes, or GLSL or SPIR-V that gpu turns into it: glsl, spirv, dxil or msl.");
        SDL_free(made);
        return nullptr;
    }

    const ECSValue *givenUniforms = ECSValue_GetTableField(desc, "uniforms");
    const ECSValue *givenSamplers = ECSValue_GetTableField(desc, "samplers");
    const SDL_GPUShaderCreateInfo info = {
        .code_size = code.size,
        .code = code.data,
        .entrypoint = entry,
        .format = format,
        .stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = givenSamplers == nullptr ? samplers : (Uint32)SDL_max(0, ECSValue_GetInteger(givenSamplers, 0)),
        .num_uniform_buffers = givenUniforms == nullptr ? uniforms : (Uint32)SDL_max(0, ECSValue_GetInteger(givenUniforms, 0)),
    };
    SDL_GPUShader *shader = SDL_CreateGPUShader(GPU.device, &info);

    if (shader == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot make a %s shader: %s", stage, SDL_GetError());
    }

    SDL_free(made);
    return shader;
}

/// @brief gpu.compile: compiles GLSL to SPIR-V, so a plugin finds a shader's errors, or keeps the SPIR-V. Both are kept until the next call.
static SHUSlice GpuCompile(const char *source, const char *stage, const char **retError)
{
    SDL_free(GPU.compiled);
    SDL_free(GPU.compileError);
    GPU.compiled = nullptr;
    GPU.compileError = nullptr;
    *retError = "";

    bool vertex = SDL_strcmp(stage, "vertex") == 0;

    if (!vertex && SDL_strcmp(stage, "fragment") != 0)
    {
        *retError = "A shader's stage is \"vertex\" or \"fragment\".";
        return cs(nullptr, 0);
    }

    usz size = 0;
    GPU.compiled = GpuCompileGlsl(source, vertex, &size, &GPU.compileError);

    // a failure always says why, even when there was no memory for the reason
    if (GPU.compileError != nullptr)
    {
        *retError = GPU.compileError;
    }
    else if (GPU.compiled == nullptr)
    {
        *retError = "gpu cannot compile the shader";
    }

    return cs(GPU.compiled, size);
}

static void GpuPipelineDestroy(void *object)
{
    GpuPipeline *pipeline = object;
    SDL_ReleaseGPUGraphicsPipeline(GPU.device, pipeline->pipeline);
    SDL_free(pipeline);
}

/// @brief Finds a word in a list of names, which are in the order of their values.
static i32 GpuChoose(const char *word, const char *const *names, usz count)
{
    for (usz i = 0; i < count; i++)
    {
        if (SDL_strcmp(word, names[i]) == 0)
        {
            return (i32)i;
        }
    }

    return -1;
}

static GpuPipeline *GpuNewPipeline(const ECSValue *desc)
{
    SDL_GPUShader *vertex = ECSValue_GetHandle(ECSValue_GetTableField(desc, "vertex"), "gpu.shader");
    SDL_GPUShader *fragment = ECSValue_GetHandle(ECSValue_GetTableField(desc, "fragment"), "gpu.shader");
    const ECSValue *attributes = ECSValue_GetTableField(desc, "attributes");
    usz count = ECSValue_GetListCount(attributes);

    if (GPU.device == nullptr || vertex == nullptr || fragment == nullptr || count == 0 || count > GPU_ATTRIBUTES)
    {
        ECS_LogWarn(GPU.plugin, GPU.device == nullptr ? "gpu cannot make a pipeline: OpenECS has no GPU renderer." : "A pipeline needs a vertex shader, a fragment shader, and from 1 to 16 attributes.");
        return nullptr;
    }

    // each attribute of a vertex, in order, at the location of its number from 0; the vertices are one after another in one buffer
    static const char *const formatNames[] = {"float", "float2", "float3", "float4", "color"};
    static const SDL_GPUVertexElementFormat formats[] = {SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM};
    static const u32 sizes[] = {4, 8, 12, 16, 4};
    SDL_GPUVertexAttribute vertexAttributes[GPU_ATTRIBUTES] = {0};
    u32 pitch = 0;

    for (usz i = 0; i < count; i++)
    {
        const char *name = ECSValue_GetString(ECSValue_GetListItem(attributes, i), "");
        i32 chosen = GpuChoose(name, formatNames, SDL_arraysize(formatNames));

        if (chosen < 0)
        {
            ECS_LogWarn(GPU.plugin, "A vertex attribute is float, float2, float3, float4 or color, not '%s'.", name);
            return nullptr;
        }

        vertexAttributes[i] = (SDL_GPUVertexAttribute){.location = (Uint32)i, .format = formats[chosen], .offset = pitch};
        pitch += sizes[chosen];
    }

    static const char *const primitiveNames[] = {"triangles", "triangleStrip", "lines", "lineStrip", "points"};
    static const SDL_GPUPrimitiveType primitives[] = {SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP, SDL_GPU_PRIMITIVETYPE_LINELIST, SDL_GPU_PRIMITIVETYPE_LINESTRIP, SDL_GPU_PRIMITIVETYPE_POINTLIST};
    static const char *const cullNames[] = {"none", "front", "back"};
    static const SDL_GPUCullMode culls[] = {SDL_GPU_CULLMODE_NONE, SDL_GPU_CULLMODE_FRONT, SDL_GPU_CULLMODE_BACK};
    static const char *const blendNames[] = {"none", "alpha", "add"};
    i32 primitive = GpuChoose(ECSValue_GetString(ECSValue_GetTableField(desc, "primitive"), "triangles"), primitiveNames, SDL_arraysize(primitiveNames));
    i32 cull = GpuChoose(ECSValue_GetString(ECSValue_GetTableField(desc, "cull"), "none"), cullNames, SDL_arraysize(cullNames));
    i32 blend = GpuChoose(ECSValue_GetString(ECSValue_GetTableField(desc, "blend"), "none"), blendNames, SDL_arraysize(blendNames));
    bool depth = ECSValue_GetBool(ECSValue_GetTableField(desc, "depth"), true);

    if (primitive < 0 || cull < 0 || blend < 0)
    {
        ECS_LogWarn(GPU.plugin, "A pipeline's primitive is triangles, triangleStrip, lines, lineStrip or points; its cull none, front or back; its blend none, alpha or add.");
        return nullptr;
    }

    // alpha blends by the fragment's alpha, which is not premultiplied; add adds the fragment's colour
    SDL_GPUColorTargetDescription target = {.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM};
    target.blend_state = (SDL_GPUColorTargetBlendState){
        .enable_blend = blend != 0,
        .src_color_blendfactor = blend == 1 ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE,
        .dst_color_blendfactor = blend == 1 ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE,
        .color_blend_op = SDL_GPU_BLENDOP_ADD,
        .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
        .dst_alpha_blendfactor = blend == 1 ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE,
        .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
    };

    const SDL_GPUVertexBufferDescription buffer = {.slot = 0, .pitch = pitch, .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    const SDL_GPUGraphicsPipelineCreateInfo info = {
        .vertex_shader = vertex,
        .fragment_shader = fragment,
        .vertex_input_state = {.vertex_buffer_descriptions = &buffer, .num_vertex_buffers = 1, .vertex_attributes = vertexAttributes, .num_vertex_attributes = (Uint32)count},
        .primitive_type = primitives[primitive],
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = culls[cull], .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE},
        // every render pass of a frame has a depth texture, so a pipeline without depth only leaves it alone
        .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_LESS, .enable_depth_test = depth, .enable_depth_write = depth},
        .target_info = {.color_target_descriptions = &target, .num_color_targets = 1, .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM, .has_depth_stencil_target = true},
    };
    GpuPipeline *pipeline = SDL_calloc(1, sizeof(GpuPipeline));

    if (pipeline != nullptr)
    {
        pipeline->pitch = pitch;
        pipeline->pipeline = SDL_CreateGPUGraphicsPipeline(GPU.device, &info);
    }

    if (pipeline == nullptr || pipeline->pipeline == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot make a pipeline: %s", SDL_GetError());
        SDL_free(pipeline);
        return nullptr;
    }

    return pipeline;
}

/// @brief Gives a depth texture of a size, the same one each time for a size, so a frame's passes share it; a new size takes the place of the oldest.
static SDL_GPUTexture *GpuDepth(i32 width, i32 height)
{
    for (usz i = 0; i < GPU_DEPTHS; i++)
    {
        if (GPU.depths[i].texture != nullptr && GPU.depths[i].width == width && GPU.depths[i].height == height)
        {
            return GPU.depths[i].texture;
        }
    }

    const SDL_GPUTextureCreateInfo info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_D16_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
        .width = (Uint32)width,
        .height = (Uint32)height,
        .layer_count_or_depth = 1,
        .num_levels = 1,
    };
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(GPU.device, &info);

    if (texture == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot make a %dx%d depth texture: %s", width, height, SDL_GetError());
        return nullptr;
    }

    usz slot = GPU.nextDepth;
    if (GPU.depths[slot].texture != nullptr)
    {
        SDL_ReleaseGPUTexture(GPU.device, GPU.depths[slot].texture);
    }

    GPU.depths[slot].texture = texture;
    GPU.depths[slot].width = width;
    GPU.depths[slot].height = height;
    GPU.nextDepth = (slot + 1) % GPU_DEPTHS;
    return texture;
}

/// @brief Opens a frame's render pass after what the renderer drew; the frame's first pass clears the depth texture, and later ones keep it.
static SDL_GPURenderPass *GpuBeginPass(GpuFrame *frame)
{
    if (frame->pass != nullptr)
    {
        return frame->pass;
    }

    GpuSendRendered();
    SDL_GPUTexture *depth = GpuDepth(frame->width, frame->height);
    frame->commands = depth == nullptr ? nullptr : SDL_AcquireGPUCommandBuffer(GPU.device);

    if (frame->commands == nullptr)
    {
        return nullptr;
    }

    const SDL_GPUColorTargetInfo color = {.texture = frame->color, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE};
    const SDL_GPUDepthStencilTargetInfo depthTarget = {
        .texture = depth,
        .clear_depth = 1.0f,
        .load_op = frame->depthCleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
        .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
    };
    frame->pass = SDL_BeginGPURenderPass(frame->commands, &color, 1, &depthTarget);
    frame->depthCleared = true;

    if (frame->pass == nullptr)
    {
        SDL_SubmitGPUCommandBuffer(frame->commands);
        frame->commands = nullptr;
    }

    return frame->pass;
}

static void GpuDraw(ECSSurface *surface, GpuPipeline *pipeline, const ECSValue *draw)
{
    GpuFrame *frame = GpuFrameOf(surface);

    if (frame == nullptr)
    {
        return;
    }

    GpuBuffer *vertices = ECSValue_GetHandle(ECSValue_GetTableField(draw, "vertices"), "gpu.buffer");
    GpuBuffer *indices = ECSValue_GetHandle(ECSValue_GetTableField(draw, "indices"), "gpu.buffer");
    SDL_Texture *texture = ECSValue_GetHandle(ECSValue_GetTableField(draw, "texture"), "gpu.texture");
    i64 atlas = ECSValue_GetInteger(ECSValue_GetTableField(draw, "atlas"), 0);

    // a texture of the text engine's glyphs, by the number that gpu.glyphs gave, in place of a texture
    if (texture == nullptr && atlas > 0 && atlas <= (i64)GPU.atlasCount)
    {
        texture = GPU.atlases[atlas - 1].texture;
    }

    if (vertices == nullptr || vertices->index || (indices != nullptr && !indices->index))
    {
        ECS_LogWarn(GPU.plugin, "A draw needs a vertex buffer as its vertices, and an index buffer as its indices, if it has any.");
        return;
    }

    // without a count, a draw takes every index, or every vertex
    i64 all = indices != nullptr ? indices->size / 4 : vertices->size / pipeline->pitch;
    i64 count = ECSValue_GetInteger(ECSValue_GetTableField(draw, "count"), all);
    SHUSlice vertexUniforms = ECSValue_GetBuffer(ECSValue_GetTableField(draw, "vertexUniforms"), (SHUSlice){0});
    SHUSlice fragmentUniforms = ECSValue_GetBuffer(ECSValue_GetTableField(draw, "fragmentUniforms"), (SHUSlice){0});

    // a texture whose pixels changed since the pass began gets them first
    if (texture != nullptr && GPU.rendered)
    {
        GpuEndPass(frame);
    }

    SDL_GPURenderPass *pass = count <= 0 ? nullptr : GpuBeginPass(frame);

    if (pass == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, pipeline->pipeline);
    const SDL_GPUBufferBinding vertexBinding = {.buffer = vertices->buffer};
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

    // uniforms are each stage's first uniform block, laid out as std140 lays it out
    if (vertexUniforms.size > 0)
    {
        SDL_PushGPUVertexUniformData(frame->commands, 0, vertexUniforms.data, (Uint32)vertexUniforms.size);
    }

    if (fragmentUniforms.size > 0)
    {
        SDL_PushGPUFragmentUniformData(frame->commands, 0, fragmentUniforms.data, (Uint32)fragmentUniforms.size);
    }

    // a texture is the fragment shader's first sampler
    if (texture != nullptr)
    {
        const SDL_GPUTextureSamplerBinding binding = {
            .texture = SDL_GetPointerProperty(SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr),
            .sampler = GPU.sampler,
        };
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    }

    if (indices != nullptr)
    {
        const SDL_GPUBufferBinding indexBinding = {.buffer = indices->buffer};
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, (Uint32)SDL_min(count, all), 1, 0, 0, 0);
    }
    else
    {
        SDL_DrawGPUPrimitives(pass, (Uint32)SDL_min(count, all), 1, 0, 0);
    }
}

#pragma endregion Pipelines

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    GPU.plugin = plugin;

    // without OpenECS's GPU device the plugin still loads, so its users load too, and its functions say why they cannot draw
    SDL_GPUDevice *device = ECSGpu_GetDevice();
    GPU.renderer = device == nullptr ? nullptr : SDL_CreateGPURenderer(device, nullptr);

    if (device != nullptr && GPU.renderer == nullptr)
    {
        ECS_LogWarn(plugin, "Cannot make a renderer on the GPU device: %s", SDL_GetError());
    }

    // pipelines draw on the same device; a texture is sampled linearly, and its edges are clamped
    if (GPU.renderer != nullptr)
    {
        SDL_SetRenderDrawBlendMode(GPU.renderer, SDL_BLENDMODE_BLEND);
        GPU.device = device;
        const SDL_GPUSamplerCreateInfo sampler = {
            .min_filter = SDL_GPU_FILTER_LINEAR,
            .mag_filter = SDL_GPU_FILTER_LINEAR,
            .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
            .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        };
        GPU.sampler = SDL_CreateGPUSampler(device, &sampler);

        // text keeps its glyphs in textures on the same device
        GPU.ttfStarted = TTF_Init();
        SDL_PropertiesID props = GPU.ttfStarted ? SDL_CreateProperties() : 0;
        SDL_SetPointerProperty(props, TTF_PROP_GPU_TEXT_ENGINE_DEVICE_POINTER, device);
        SDL_SetNumberProperty(props, TTF_PROP_GPU_TEXT_ENGINE_ATLAS_TEXTURE_SIZE_NUMBER, GPU_ATLAS_SIZE);
        GPU.textEngine = props == 0 ? nullptr : TTF_CreateGPUTextEngineWithProperties(props);
        SDL_DestroyProperties(props);

        if (GPU.textEngine == nullptr)
        {
            ECS_LogWarn(plugin, "Cannot draw text on the GPU device: %s", SDL_GetError());
        }
    }

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = GpuFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), GpuFree(nullptr););
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&GPU.FontFile, "draw.fontFile", "string(handle<draw.font> font)"), GpuFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "texture", GpuTextureDestroy), GpuFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "buffer", GpuBufferDestroy), GpuFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "shader", GpuShaderDestroy), GpuFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "pipeline", GpuPipelineDestroy), GpuFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"clear", (ECSFunction)GpuClear, "void(handle<ecs.surface> surface, color color)",
         "Fill the whole surface with a colour, replacing what it holds\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param color The colour, alpha included, that every pixel takes"},
        {"fill", (ECSFunction)GpuFill, "void(handle<ecs.surface> surface, float x, float y, float width, float height, color color)",
         "Fill a rectangle\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units\n"
         "@param color The colour, blended by its alpha"},
        {"line", (ECSFunction)GpuLine, "void(handle<ecs.surface> surface, float x1, float y1, float x2, float y2, float thickness, color color)",
         "Draw a line\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param x1 The start's x, in layout units\n"
         "@param y1 The start's y, in layout units\n"
         "@param x2 The end's x, in layout units\n"
         "@param y2 The end's y, in layout units\n"
         "@param thickness The line's thickness, in layout units, half on each side; a line whose ends meet draws nothing\n"
         "@param color The colour, blended by its alpha"},
        {"triangles", (ECSFunction)GpuTriangles, "void(handle<ecs.surface> surface, buffer points, buffer indices, color color)",
         "Fill triangles: points are x and y floats, and indices name three points for each triangle, or are empty for the points in threes\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param points The points, x and y 32-bit floats each, in layout units\n"
         "@param indices The 32-bit indices of the points, from 0, three for each triangle; a triangle with an index past the points is left out\n"
         "@param color The colour of every triangle, blended by its alpha"},
        {"mesh", (ECSFunction)GpuMesh, "void(handle<ecs.surface> surface, buffer points, buffer colors, buffer indices)",
         "Fill triangles whose points each have a colour, blended across each triangle: points are x and y floats, colors are one ARGB 32-bit number for each point, and indices name three points for each triangle, or are empty for the points in threes; later triangles cover earlier ones\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param points The points, x and y 32-bit floats each, in layout units\n"
         "@param colors One colour for each point, ARGB without premultiplied alpha; fewer colours than points draw nothing\n"
         "@param indices The 32-bit indices of the points, from 0, three for each triangle; a triangle with an index past the points is left out"},
        {"texture", (ECSFunction)GpuNewTexture, "handle<gpu.texture>(int width, int height, buffer pixels)",
         "Make a texture on the GPU from pixels, ARGB with premultiplied alpha in native byte order, row after row, as draw's images have\n"
         "@param width The texture's width in pixels, at least 1\n"
         "@param height The texture's height in pixels, at least 1\n"
         "@param pixels Exactly 4 bytes for each pixel, which are copied\n"
         "@return The texture, or nothing for a wrong size or without a GPU renderer; the reason is logged"},
        {"updateTexture", (ECSFunction)GpuUpdateTexture, "bool(handle<gpu.texture> texture, buffer pixels)",
         "Replace a texture's pixels with as many new ones\n"
         "@param texture The texture\n"
         "@param pixels Exactly 4 bytes for each of the texture's pixels, laid out as texture takes them, which are copied\n"
         "@return Whether the pixels were replaced; false for another number of bytes"},
        {"textureSize", (ECSFunction)GpuTextureSize, "void(handle<gpu.texture> texture, out int width, out int height)",
         "Give a texture's size in pixels\n"
         "@param texture The texture\n"
         "@param width Set to the texture's width in pixels\n"
         "@param height Set to the texture's height in pixels"},
        {"freeTexture", (ECSFunction)GpuFreeTexture, "void(handle<gpu.texture> texture)",
         "Free a texture now, instead of when no one uses its handle\n"
         "@param texture The texture, which is not used again after this"},
        {"buffer", (ECSFunction)GpuNewBuffer, "handle<gpu.buffer>(string usage, buffer bytes)",
         "Make a buffer on the GPU of \"vertex\" data, vertices one after another as a pipeline's attributes lay them out, or of \"index\" data, 32-bit indices\n"
         "@param usage What it holds: \"vertex\" or \"index\"\n"
         "@param bytes The buffer's bytes, at least one, which are copied; 4 bytes for each index\n"
         "@return The buffer, or nothing for another usage, no bytes or without a GPU renderer; the reason is logged"},
        {"updateBuffer", (ECSFunction)GpuUpdateBuffer, "bool(handle<gpu.buffer> buffer, buffer bytes)",
         "Replace a buffer's bytes; bytes of another size make the buffer again\n"
         "@param buffer The buffer\n"
         "@param bytes The new bytes, at least one, which are copied; 4 bytes for each index\n"
         "@return Whether the bytes were replaced; the reason they were not is logged"},
        {"bufferSize", (ECSFunction)GpuBufferSize, "int(handle<gpu.buffer> buffer)",
         "Give a buffer's size in bytes\n"
         "@param buffer The buffer\n"
         "@return The number of bytes it holds"},
        {"freeBuffer", (ECSFunction)GpuFreeBuffer, "void(handle<gpu.buffer> buffer)",
         "Free a buffer now, instead of when no one uses its handle\n"
         "@param buffer The buffer, which is not used again after this"},
        {"shader", (ECSFunction)GpuNewShader, "handle<gpu.shader>(value<gpu.ShaderDesc> desc)",
         "Make a vertex or fragment shader from GLSL, or from compiled code: SPIR-V, DXIL or MSL; gpu turns GLSL and SPIR-V into the code the GPU's driver takes\n"
         "@param desc The shader's stage, its code, its entry, and its numbers of uniforms and samplers, as gpu.ShaderDesc's fields say\n"
         "@return The shader, or nothing if it cannot be made, such as for code that does not compile; the reason is logged"},
        {"compile", (ECSFunction)GpuCompile, "buffer(string source, string stage, out string error)",
         "Compile GLSL to SPIR-V for a vertex or fragment stage; an empty buffer and why if it cannot\n"
         "@param source The GLSL source, version 450 for Vulkan\n"
         "@param stage The shader's stage: \"vertex\" or \"fragment\"\n"
         "@param error Set to why it does not compile, or to an empty string if it does\n"
         "@return The SPIR-V, kept until the next compile, or an empty buffer if it does not compile"},
        {"pipeline", (ECSFunction)GpuNewPipeline, "handle<gpu.pipeline>(value<gpu.PipelineDesc> desc)",
         "Make a pipeline: a vertex and a fragment shader, the attributes of a vertex, and how its primitives are drawn\n"
         "@param desc The shaders, the attributes, from 1 to 16, the primitive, depth, cull and blend, as gpu.PipelineDesc's fields say\n"
         "@return The pipeline, or nothing if it cannot be made, such as for a missing shader or an unknown word; the reason is logged"},
        {"draw", (ECSFunction)GpuDraw, "void(handle<ecs.surface> surface, handle<gpu.pipeline> pipeline, value<gpu.Draw> draw)",
         "Draw vertices with a pipeline into a frame, after what it drew before, with a depth texture that the frame's draws share\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param pipeline The pipeline that draws them\n"
         "@param draw The vertex and index buffers, the count, the uniforms and the texture or atlas, as gpu.Draw's fields say; a count above what the buffers hold draws all"},
        {"image", (ECSFunction)GpuImage, "void(handle<ecs.surface> surface, handle<gpu.texture> texture, float x, float y, float width, float height)",
         "Draw a texture stretched to a rectangle\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param texture The texture, blended by its premultiplied alpha\n"
         "@param x The rectangle's left, in layout units\n"
         "@param y The rectangle's top, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units"},
        {"imageTransformed", (ECSFunction)GpuImageTransformed, "void(handle<ecs.surface> surface, handle<gpu.texture> texture, float x, float y, float width, float height, float angle, color color)",
         "Draw a texture stretched to a rectangle, turned clockwise by an angle in degrees around its centre, with each pixel's colour multiplied by a colour\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param texture The texture, blended by its premultiplied alpha\n"
         "@param x The rectangle's left before it is turned, in layout units\n"
         "@param y The rectangle's top before it is turned, in layout units\n"
         "@param width The rectangle's width, in layout units\n"
         "@param height The rectangle's height, in layout units\n"
         "@param angle The turn in degrees, clockwise; 0 leaves it upright\n"
         "@param color The colour each pixel's colour is multiplied by; its alpha fades the whole texture, and white leaves it as it is"},
        {"text", (ECSFunction)GpuText, "float(handle<ecs.surface> surface, handle<draw.font> font, string text, float x, float y, float size, color color)",
         "Draw text in one of draw's fonts with its line's top left at a position, and give its width\n"
         "@param surface The GPU panel's surface, as its draw gives it\n"
         "@param font The font, such as draw.font gives\n"
         "@param text The text, in UTF-8\n"
         "@param x The left of the text's line, in layout units\n"
         "@param y The top of the text's line, in layout units\n"
         "@param size The font's size, in layout units\n"
         "@param color The text's colour, blended by its alpha\n"
         "@return The text's width in layout units; 0 for empty text, or when it cannot draw"},
        {"glyphs", (ECSFunction)GpuGlyphs, "void(handle<draw.font> font, string text, float size, out value<gpu.Glyphs?> glyphs)",
         "Give the glyphs of a text in one of draw's fonts at a size in pixels, for a pipeline of a plugin's own: for each texture of the glyphs, its number, which a draw's atlas samples, and the glyphs' rectangles as vertices and indices; nothing without a GPU renderer\n"
         "@param font The font, such as draw.font gives\n"
         "@param text The text, in UTF-8\n"
         "@param size The font's size in pixels, rounded to a whole pixel\n"
         "@param glyphs Set to the text's width, its line's height and its parts, as gpu.Glyphs's fields say; nothing for empty text, without a GPU renderer, or if the font cannot be opened"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), GpuFree(nullptr););
    }

    // Lua calls these on a handle, such as buffer:update(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"buffer", "update", "updateBuffer"},
        {"buffer", "size", "bufferSize"},
        {"buffer", "free", "freeBuffer"},
        {"texture", "update", "updateTexture"},
        {"texture", "size", "textureSize"},
        {"texture", "free", "freeTexture"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function), GpuFree(nullptr););
    }

    return SHUResult_Ok;
}
