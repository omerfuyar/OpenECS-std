// The image standard plugin: reads image files into images of the draw plugin, which draws them, and writes those images to files (DESIGN 6).
// stb_image reads PNG, JPG, GIF, BMP, TGA, PSD, HDR and PNM files, and nanosvg reads SVG files, which are drawn sharp at the scale they are read at.
// stb_image_write writes PNG, JPG, BMP and TGA files.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define STBI_MALLOC SDL_malloc
#define STBI_REALLOC SDL_realloc
#define STBI_FREE SDL_free
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STBIW_MALLOC SDL_malloc
#define STBIW_REALLOC SDL_realloc
#define STBIW_FREE SDL_free
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#pragma region Source Only

/// @brief The resolution SVG files are read at: their units become pixels at 96 per inch.
#define IMAGE_SVG_DPI 96.0f

/// @brief The quality of the JPG files written, from 1 to 100.
#define IMAGE_JPG_QUALITY 90

/// @brief draw.newImage, which makes an image of the draw plugin from pixels.
typedef void *(*ImageNewFunction)(i32 width, i32 height, SHUSlice pixels);

/// @brief draw.imageSize, which gives an image's size in pixels.
typedef void (*ImageSizeFunction)(void *image, i32 *retWidth, i32 *retHeight);

/// @brief draw.imagePixels, which gives an image's pixels, ARGB in native byte order, row after row.
typedef SHUSlice (*ImagePixelsFunction)(void *image);

static struct
{
    ECSPlugin plugin;
    NSVGrasterizer *rasterizer;
    ImageNewFunction NewImage;
    ImageSizeFunction ImageSize;
    ImagePixelsFunction ImagePixels;
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

/// @brief Writes an image of draw to a file, in the format its extension names: .png, .jpg or .jpeg, .bmp or .tga.
/// @return Whether the file was written; the reason it was not is logged.
static bool ImageSave(void *image, const char *path)
{
    const struct
    {
        const char *extension;
        u8 format;
    } formats[] = {{".png", 0}, {".jpg", 1}, {".jpeg", 1}, {".bmp", 2}, {".tga", 3}};
    i32 format = -1;

    for (usz i = 0; i < (usz)SDL_arraysize(formats); i++)
    {
        format = format < 0 && ImageEndsWith(path, formats[i].extension) ? formats[i].format : format;
    }

    if (format < 0)
    {
        ECS_LogWarn(IMAGE.plugin, "An image is written to a .png, .jpg, .jpeg, .bmp or .tga file, not '%s'.", path);
        return false;
    }

    char *full = nullptr;
    i32 width = 0;
    i32 height = 0;
    IMAGE.ImageSize(image, &width, &height);
    SHUSlice argb = IMAGE.ImagePixels(image);
    // stb_image_write takes the bytes red, green, blue and alpha, in that order
    u8 *rgba = SDL_malloc(argb.size);
    bool written = false;

    if (rgba != nullptr && SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_ARGB8888, argb.data, width * 4, SDL_PIXELFORMAT_RGBA32, rgba, width * 4) &&
        SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) >= 0)
    {
        written = format == 0 ? stbi_write_png(full, width, height, 4, rgba, width * 4) != 0
                  : format == 1 ? stbi_write_jpg(full, width, height, 4, rgba, IMAGE_JPG_QUALITY) != 0
                  : format == 2 ? stbi_write_bmp(full, width, height, 4, rgba) != 0
                                : stbi_write_tga(full, width, height, 4, rgba) != 0;
    }

    if (!written)
    {
        ECS_LogError(IMAGE.plugin, "Cannot write the image '%s'", full != nullptr ? full : path);
    }

    SDL_free(full);
    SDL_free(rgba);
    return written;
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
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&IMAGE.ImageSize, "draw.imageSize", "void(handle<draw.image> image, out int width, out int height)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&IMAGE.ImagePixels, "draw.imagePixels", "buffer(handle<draw.image> image)"));
    IMAGE.rasterizer = nsvgCreateRasterizer();

    if (IMAGE.rasterizer == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = ImageFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), ImageFree(nullptr););

    SHU_ReturnResult(ECSService_RegisterFunction(plugin, "load", (ECSFunction)ImageLoad, "handle<draw.image>(string path, float scale)",
                                                 "Read an image file into an image of draw, at a scale of its own size; an SVG file is drawn sharp at that scale. Nothing if it cannot be read"));

    return ECSService_RegisterFunction(plugin, "save", (ECSFunction)ImageSave, "bool(handle<draw.image> image, string path)",
                                       "Write an image of draw to a file, in the format its extension names: .png, .jpg, .jpeg, .bmp or .tga");
}
