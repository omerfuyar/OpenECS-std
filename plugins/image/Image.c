// The image standard plugin: reads image files into images of the draw plugin, which draws them (DESIGN 6).
// stb_image reads PNG, JPG, GIF, BMP, TGA, PSD, HDR and PNM files, and nanosvg reads SVG files, which are drawn sharp at the scale they are read at.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define STBI_MALLOC SDL_malloc
#define STBI_REALLOC SDL_realloc
#define STBI_FREE SDL_free
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#pragma region Source Only

/// @brief The resolution SVG files are read at: their units become pixels at 96 per inch.
#define IMAGE_SVG_DPI 96.0f

/// @brief draw.newImage, which makes an image of the draw plugin from pixels.
typedef void *(*ImageNewFunction)(i32 width, i32 height, SHUSlice pixels);

static struct
{
    ECSPlugin plugin;
    NSVGrasterizer *rasterizer;
    ImageNewFunction NewImage;
} IMAGE = {0};

static bool ImageEndsWith(const char *text, const char *suffix)
{
    usz length = SDL_strlen(text);
    usz suffixLength = SDL_strlen(suffix);
    return length >= suffixLength && SDL_strcasecmp(text + length - suffixLength, suffix) == 0;
}

/// @brief Reads an SVG file and draws it at a scale of its own size.
/// @return The pixels, ARGB, to free with SDL_DestroySurface, or nullptr if the file cannot be read.
static SDL_Surface *ImageReadSvg(const char *path, f32 scale)
{
    NSVGimage *svg = nsvgParseFromFile(path, "px", IMAGE_SVG_DPI);
    i32 width = svg == nullptr ? 0 : (i32)SDL_ceilf(svg->width * scale);
    i32 height = svg == nullptr ? 0 : (i32)SDL_ceilf(svg->height * scale);
    SDL_Surface *rgba = width > 0 && height > 0 ? SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ABGR8888) : nullptr;

    if (rgba != nullptr)
    {
        SDL_memset(rgba->pixels, 0, (usz)rgba->pitch * (usz)height);
        nsvgRasterize(IMAGE.rasterizer, svg, 0.0f, 0.0f, scale, rgba->pixels, width, height, rgba->pitch);
    }

    SDL_Surface *argb = rgba == nullptr ? nullptr : SDL_ConvertSurface(rgba, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(rgba);
    nsvgDelete(svg);
    return argb;
}

/// @brief Reads a raster image file, stretched by a scale.
/// @return The pixels, ARGB, to free with SDL_DestroySurface, or nullptr if the file cannot be read.
static SDL_Surface *ImageReadRaster(const char *path, f32 scale)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(path, &width, &height, &channels, 4);
    SDL_Surface *rgba = pixels == nullptr ? nullptr : SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ABGR8888, pixels, width * 4);
    SDL_Surface *argb = rgba == nullptr ? nullptr : SDL_ConvertSurface(rgba, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(rgba);
    stbi_image_free(pixels);

    if (argb == nullptr || scale == 1.0f)
    {
        return argb;
    }

    SDL_Surface *scaled = SDL_ScaleSurface(argb, SDL_max(1, (int)SDL_roundf((f32)width * scale)), SDL_max(1, (int)SDL_roundf((f32)height * scale)), SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(argb);
    return scaled;
}

/// @brief Reads an image file into a new image of the draw plugin.
/// @return The image, or nullptr if the file cannot be read; the reason is logged.
static void *ImageLoad(const char *path, f32 scale)
{
    char *full = nullptr;

    // a relative path starts at the executable's folder
    if (scale <= 0.0f || SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
    {
        ECS_LogWarn(IMAGE.plugin, "An image is read at a scale above 0, not %g.", (double)scale);
        return nullptr;
    }

    SDL_Surface *surface = ImageEndsWith(full, ".svg") ? ImageReadSvg(full, scale) : ImageReadRaster(full, scale);
    void *image = nullptr;

    if (surface == nullptr)
    {
        ECS_LogError(IMAGE.plugin, "Cannot read the image '%s': %s", full, stbi_failure_reason() != nullptr ? stbi_failure_reason() : "not an image");
    }
    else
    {
        // draw takes the pixels row after row, without the surface's padding
        SDL_Surface *packed = surface->pitch == surface->w * 4 ? surface : SDL_DuplicateSurface(surface);
        image = packed == nullptr ? nullptr : IMAGE.NewImage(surface->w, surface->h, cs(packed->pixels, (usz)surface->w * (usz)surface->h * 4));

        if (packed != surface)
        {
            SDL_DestroySurface(packed);
        }
    }

    SDL_DestroySurface(surface);
    SDL_free(full);
    return image;
}

/// @brief Frees what the plugin holds; the Destroy function of the plugin.
static void ImageFree(void *data)
{
    (void)data;
    nsvgDeleteRasterizer(IMAGE.rasterizer);
    SDL_zero(IMAGE);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    IMAGE.plugin = plugin;
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&IMAGE.NewImage, "draw.newImage", "handle<draw.image>(int width, int height, buffer pixels)"));
    IMAGE.rasterizer = nsvgCreateRasterizer();

    if (IMAGE.rasterizer == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = ImageFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), ImageFree(nullptr););

    return ECSService_RegisterFunction(plugin, "load", (ECSFunction)ImageLoad, "handle<draw.image>(string path, float scale)",
                                       "Read an image file into an image of draw, at a scale of its own size; an SVG file is drawn sharp at that scale. Nothing if it cannot be read");
}
