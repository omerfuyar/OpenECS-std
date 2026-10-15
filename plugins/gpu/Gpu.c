// The gpu standard plugin: draws shapes, lines and textures into the GPU surface of another plugin's panel, on the GPU (DESIGN 15).
// Its functions are those of draw, for a frame instead of a surface: a panel's Draw starts a frame of its surface, draws, and submits it.
// It draws with an offscreen SDL renderer of its own on OpenECS's GPU device, which draws into the panel's texture, so it needs no shaders.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief Drawing into one GPU surface: the renderer's view of the surface's texture, until the frame is submitted.
typedef struct GpuFrame
{
    SDL_Texture *target; // the surface's texture, which the renderer draws into without owning it
    f32 scale;           // the surface's pixels per layout unit
} GpuFrame;

static struct
{
    ECSPlugin plugin;
    SDL_Renderer *renderer; // an offscreen GPU renderer on OpenECS's device, or nullptr without a GPU renderer
} GPU = {0};

/// @brief Frees the renderer; the plugin's Destroy, after the handles of its frames and textures are gone.
static void GpuFree(void *data)
{
    (void)data;

    if (GPU.renderer != nullptr)
    {
        SDL_DestroyRenderer(GPU.renderer);
    }

    GPU.renderer = nullptr;
}

/// @brief Draws into a frame's texture from now on, with a colour, and blends by its alpha.
static bool GpuTarget(GpuFrame *frame, ECSColor color)
{
    SDL_SetRenderTarget(GPU.renderer, frame->target);
    return SDL_SetRenderDrawColor(GPU.renderer, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) & 0xFF);
}

/// @brief Frees a frame that was not submitted; what it drew is still sent with the renderer's next frame.
static void GpuFrameDestroy(void *object)
{
    GpuFrame *frame = object;
    SDL_DestroyTexture(frame->target);
    SDL_free(frame);
}

static void GpuTextureDestroy(void *object)
{
    SDL_DestroyTexture(object);
}

static GpuFrame *GpuStartFrame(ECSSurface *surface)
{
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

    if (frame == nullptr)
    {
        ECS_LogError(GPU.plugin, "Cannot draw into a GPU surface: %s", SDL_GetError());
        SDL_DestroyTexture(target);
        return nullptr;
    }

    frame->target = target;
    frame->scale = surface->scale;
    return frame;
}

static void GpuSubmit(GpuFrame *frame)
{
    // presenting the offscreen renderer sends what it drew to the GPU, before the core's renderer shows the texture
    SDL_SetRenderTarget(GPU.renderer, nullptr);
    SDL_RenderPresent(GPU.renderer);
    ECSHandle_Forget(frame);
    GpuFrameDestroy(frame);
}

static void GpuClear(GpuFrame *frame, ECSColor color)
{
    // clearing replaces the pixels, whatever the blend mode
    GpuTarget(frame, color);
    SDL_RenderClear(GPU.renderer);
}

static void GpuFill(GpuFrame *frame, f32 x, f32 y, f32 width, f32 height, ECSColor color)
{
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

static void GpuLine(GpuFrame *frame, f32 x1, f32 y1, f32 x2, f32 y2, f32 thickness, ECSColor color)
{
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

static void GpuTriangles(GpuFrame *frame, SHUSlice points, SHUSlice indices, ECSColor color)
{
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

static void GpuImageTransformed(GpuFrame *frame, SDL_Texture *texture, f32 x, f32 y, f32 width, f32 height, f32 angle, ECSColor color)
{
    GpuTarget(frame, color);
    const SDL_FRect rect = {x * frame->scale, y * frame->scale, width * frame->scale, height * frame->scale};

    // the colour multiplies each pixel's, which are premultiplied, so its alpha fades them all
    f32 alpha = (f32)((color >> 24) & 0xFF) / 255.0f;
    SDL_SetTextureColorModFloat(texture, (f32)((color >> 16) & 0xFF) / 255.0f * alpha, (f32)((color >> 8) & 0xFF) / 255.0f * alpha, (f32)(color & 0xFF) / 255.0f * alpha);
    SDL_SetTextureAlphaModFloat(texture, alpha);
    SDL_RenderTextureRotated(GPU.renderer, texture, nullptr, &rect, angle, nullptr, SDL_FLIP_NONE);
}

static void GpuImage(GpuFrame *frame, SDL_Texture *texture, f32 x, f32 y, f32 width, f32 height)
{
    GpuImageTransformed(frame, texture, x, y, width, height, 0.0f, 0xFFFFFFFF);
}

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

    if (GPU.renderer != nullptr)
    {
        SDL_SetRenderDrawBlendMode(GPU.renderer, SDL_BLENDMODE_BLEND);
    }

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = GpuFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), GpuFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "frame", GpuFrameDestroy), GpuFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "texture", GpuTextureDestroy), GpuFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"frame", (ECSFunction)GpuStartFrame, "handle<gpu.frame>(handle<ecs.surface> surface)", "Start drawing into a GPU surface; submit the frame before the panel's draw returns"},
        {"submit", (ECSFunction)GpuSubmit, "void(handle<gpu.frame> frame)", "Send what a frame drew to the GPU, so the panel shows it; the frame cannot draw after"},
        {"clear", (ECSFunction)GpuClear, "void(handle<gpu.frame> frame, color color)", "Fill the whole surface with a colour, replacing what it holds"},
        {"fill", (ECSFunction)GpuFill, "void(handle<gpu.frame> frame, float x, float y, float width, float height, color color)", "Fill a rectangle"},
        {"line", (ECSFunction)GpuLine, "void(handle<gpu.frame> frame, float x1, float y1, float x2, float y2, float thickness, color color)", "Draw a line"},
        {"triangles", (ECSFunction)GpuTriangles, "void(handle<gpu.frame> frame, buffer points, buffer indices, color color)", "Fill triangles: points are x and y floats, and indices name three points for each triangle, or are empty for the points in threes"},
        {"texture", (ECSFunction)GpuNewTexture, "handle<gpu.texture>(int width, int height, buffer pixels)", "Make a texture on the GPU from pixels, ARGB with premultiplied alpha in native byte order, row after row, as draw's images have"},
        {"updateTexture", (ECSFunction)GpuUpdateTexture, "bool(handle<gpu.texture> texture, buffer pixels)", "Replace a texture's pixels with as many new ones"},
        {"textureSize", (ECSFunction)GpuTextureSize, "void(handle<gpu.texture> texture, out int width, out int height)", "Give a texture's size in pixels"},
        {"freeTexture", (ECSFunction)GpuFreeTexture, "void(handle<gpu.texture> texture)", "Free a texture now, instead of when no one uses its handle"},
        {"image", (ECSFunction)GpuImage, "void(handle<gpu.frame> frame, handle<gpu.texture> texture, float x, float y, float width, float height)", "Draw a texture stretched to a rectangle"},
        {"imageTransformed", (ECSFunction)GpuImageTransformed, "void(handle<gpu.frame> frame, handle<gpu.texture> texture, float x, float y, float width, float height, float angle, color color)", "Draw a texture stretched to a rectangle, turned clockwise by an angle in degrees around its centre, with each pixel's colour multiplied by a colour"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), GpuFree(nullptr););
    }

    // Lua calls these on a handle, such as frame:fill(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"frame", "submit", "submit"},
        {"frame", "clear", "clear"},
        {"frame", "fill", "fill"},
        {"frame", "line", "line"},
        {"frame", "triangles", "triangles"},
        {"frame", "image", "image"},
        {"frame", "imageTransformed", "imageTransformed"},
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
