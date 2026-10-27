// The image standard plugin: reads image files into images of the draw plugin, which draws them, and writes those images to files (DESIGN 6).
// stb_image reads PNG, JPG, GIF, BMP, TGA, PSD, HDR and PNM files, and the frames of animated GIF files; nanosvg reads SVG files, which are drawn sharp at the scale they are read at.
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

/// @brief Draws a parsed SVG image at a scale of its own size, and frees it.
/// @return The pixels, ARGB, to free with SDL_DestroySurface, or nullptr if there is no image.
static SDL_Surface *ImageDrawSvg(NSVGimage *svg, f32 scale)
{
    if (svg == nullptr)
    {
        return nullptr;
    }

    i32 width = (i32)SDL_ceilf(svg->width * scale);
    i32 height = (i32)SDL_ceilf(svg->height * scale);
    SDL_Surface *rgba = width > 0 && height > 0 ? SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32) : nullptr;
    SDL_Surface *argb = nullptr;

    if (rgba != nullptr)
    {
        SDL_memset(rgba->pixels, 0, (usz)rgba->pitch * (usz)height);
        nsvgRasterize(IMAGE.rasterizer, svg, 0.0f, 0.0f, scale, rgba->pixels, width, height, rgba->pitch);
        argb = SDL_ConvertSurface(rgba, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(rgba);
    }

    nsvgDelete(svg);
    return argb;
}

/// @brief Converts pixels of red, green, blue and alpha bytes, stretched by a scale.
/// @return The pixels, ARGB, to free with SDL_DestroySurface, or nullptr if out of memory.
static SDL_Surface *ImageFromRgbaPixels(void *pixels, i32 width, i32 height, f32 scale)
{
    SDL_Surface *rgba = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    SDL_Surface *argb = rgba == nullptr ? nullptr : SDL_ConvertSurface(rgba, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(rgba);

    if (argb == nullptr || scale == 1.0f)
    {
        return argb;
    }

    SDL_Surface *scaled = SDL_ScaleSurface(argb, SDL_max(1, (int)SDL_roundf((f32)width * scale)), SDL_max(1, (int)SDL_roundf((f32)height * scale)), SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(argb);
    return scaled;
}

/// @brief Reads the bytes of an image file at a scale of its own size; bytes that start with < after any spaces are an SVG file, which is text.
/// @return The pixels, ARGB, to free with SDL_DestroySurface, or nullptr if the bytes are not an image.
static SDL_Surface *ImageReadBytes(SHUSlice bytes, f32 scale)
{
    const u8 *data = bytes.data;
    usz first = 0;

    while (first < bytes.size && SDL_isspace(data[first]))
    {
        first++;
    }

    if (first < bytes.size && data[first] == '<')
    {
        // nanosvg changes the text it parses, so it gets a copy that ends with a zero
        char *text = SDL_malloc(bytes.size + 1);

        if (text == nullptr)
        {
            return nullptr;
        }

        SDL_memcpy(text, bytes.data, bytes.size);
        text[bytes.size] = '\0';
        SDL_Surface *surface = ImageDrawSvg(nsvgParse(text, "px", IMAGE_SVG_DPI), scale);
        SDL_free(text);
        return surface;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(bytes.data, (int)bytes.size, &width, &height, &channels, 4);
    SDL_Surface *surface = pixels == nullptr ? nullptr : ImageFromRgbaPixels(pixels, width, height, scale);
    stbi_image_free(pixels);
    return surface;
}

/// @brief Makes an image of draw from a surface's pixels, and destroys the surface.
static void *ImageMake(SDL_Surface *surface)
{
    if (surface == nullptr)
    {
        return nullptr;
    }

    // draw takes the pixels row after row, without the surface's padding
    SDL_Surface *packed = surface->pitch == surface->w * 4 ? surface : SDL_DuplicateSurface(surface);
    void *image = packed == nullptr ? nullptr : IMAGE.NewImage(surface->w, surface->h, cs(packed->pixels, (usz)surface->w * (usz)surface->h * 4));

    if (packed != surface)
    {
        SDL_DestroySurface(packed);
    }

    SDL_DestroySurface(surface);
    return image;
}

/// @brief Tells why an image cannot be read: stb_image's reason, or that it is not an image.
static const char *ImageFailure(void)
{
    return stbi_failure_reason() != nullptr ? stbi_failure_reason() : "not an image";
}

/// @brief Reads a whole file, from a path whose relative form starts at the executable's folder. Free it with SDL_free.
/// @return The bytes, or empty if the file cannot be read, or is too large for stb_image; the reason is logged.
static SHUSlice ImageReadFile(const char *path)
{
    char *fullPath = nullptr;
    usz size = 0;
    void *file = ECSPath_GetFull(&fullPath, path) ? nullptr : SDL_LoadFile(fullPath, &size);

    if (file == nullptr || size > (usz)SDL_MAX_SINT32)
    {
        ECS_LogError(IMAGE.plugin, "Cannot read the image '%s': %s", fullPath != nullptr ? fullPath : path, file == nullptr ? SDL_GetError() : "the file is too large");
        SDL_free(file);
        SDL_free(fullPath);
        return cs(nullptr, 0);
    }

    SDL_free(fullPath);
    return cs(file, size);
}

/// @brief Checks that an image is read at a scale above 0; the reason it is not is logged.
static bool ImageCheckScale(f32 scale)
{
    if (scale <= 0.0f)
    {
        ECS_LogWarn(IMAGE.plugin, "An image is read at a scale above 0, not %g.", (double)scale);
        return false;
    }

    return true;
}

/// @brief Reads an image file into a new image of the draw plugin.
/// @return The image, or nullptr if the file cannot be read; the reason is logged.
static void *ImageLoad(const char *path, f32 scale)
{
    SHUSlice file = ImageCheckScale(scale) ? ImageReadFile(path) : cs(nullptr, 0);

    if (file.data == nullptr)
    {
        return nullptr;
    }

    SDL_Surface *surface = ImageReadBytes(file, scale);
    SDL_free(file.data);

    if (surface == nullptr)
    {
        ECS_LogError(IMAGE.plugin, "Cannot read the image '%s': %s", path, ImageFailure());
    }

    return ImageMake(surface);
}

/// @brief Reads an image from bytes in memory, such as a PNG file's, into a new image of the draw plugin; bytes that start with < are read as SVG.
/// @return The image, or nullptr if the bytes are not an image; the reason is logged.
static void *ImageDecode(SHUSlice bytes, f32 scale)
{
    if (scale <= 0.0f || bytes.size == 0 || bytes.size > (usz)SDL_MAX_SINT32)
    {
        ECS_LogWarn(IMAGE.plugin, "An image is read from bytes that are there, at a scale above 0.");
        return nullptr;
    }

    SDL_Surface *surface = ImageReadBytes(bytes, scale);

    if (surface == nullptr)
    {
        ECS_LogError(IMAGE.plugin, "Cannot read an image from bytes: %s", ImageFailure());
    }

    return ImageMake(surface);
}

/// @brief Adds a frame to a list of frames: a table of its image and the seconds it shows for.
static SHUResult ImageAddFrame(ECSValue *frames, void *image, f64 seconds)
{
    ECSValue *frame = nullptr;
    ECSValue *field = nullptr;
    SHU_ReturnResult(ECSValue_AddListItem(frames, &frame));
    ECSValue_SetTable(frame);
    SHU_ReturnResult(ECSValue_SetTableField(frame, "image", &field));
    SHU_ReturnResult(ECSValue_SetHandle(field, "draw.image", image));
    SHU_ReturnResult(ECSValue_SetTableField(frame, "seconds", &field));
    ECSValue_SetNumber(field, seconds);
    return SHUResult_Ok;
}

/// @brief Skips a GIF's sub-blocks: each starts with its size, and an empty one ends them.
/// @return Where the sub-blocks end, or size if the data ends first.
static usz ImageGifSkipBlocks(const u8 *data, usz size, usz offset)
{
    while (offset < size && data[offset] != 0)
    {
        offset += 1 + (usz)data[offset];
    }

    return offset < size ? offset + 1 : size;
}

/// @brief Makes every frame of a GIF that restores the frame before it restore the background instead, in place.
/// stb_image reads the frame two back from before its buffer for that disposal, so a file could make it read memory it does not own;
/// for stb, restoring the background means restoring what showed before the frame, which is the same.
static void ImageGifAvoidPrevious(u8 *data, usz size)
{
    // the header, then the logical screen, with the global colour table if its flag is set
    if (size < 13 || SDL_memcmp(data, "GIF", 3) != 0)
    {
        return;
    }

    usz offset = 13 + ((data[10] & 0x80) != 0 ? 3 * ((usz)2 << (data[10] & 0x07)) : 0);

    while (offset < size)
    {
        if (data[offset] == 0x21 && offset + 1 < size)
        {
            // a graphic control extension: its first sub-block has 4 bytes, and its first byte holds the disposal in bits 2 to 4
            if (data[offset + 1] == 0xF9 && offset + 3 < size && data[offset + 2] == 4 && ((data[offset + 3] >> 2) & 0x07) == 3)
            {
                data[offset + 3] = (u8)((data[offset + 3] & ~0x1C) | (2 << 2));
            }

            offset = ImageGifSkipBlocks(data, size, offset + 2);
        }
        else if (data[offset] == 0x2C && offset + 10 < size)
        {
            // an image: its descriptor, its local colour table if its flag is set, the size of its codes, then its data
            u8 flags = data[offset + 9];
            offset += 10 + ((flags & 0x80) != 0 ? 3 * ((usz)2 << (flags & 0x07)) : 0) + 1;
            offset = ImageGifSkipBlocks(data, size, offset);
        }
        else
        {
            // the trailer, or data that stb stops at too
            return;
        }
    }
}

/// @brief Reads the frames of an animated GIF file into new images of draw, each with the seconds it shows for; any other image file gives one frame of 0 seconds.
/// The frames are a list; a file that cannot be read gives nil, and the reason is logged.
static void ImageLoadFrames(const char *path, f32 scale, ECSValue *retFrames)
{
    ECSValue_SetNil(retFrames);

    if (!ImageEndsWith(path, ".gif"))
    {
        void *image = ImageLoad(path, scale);
        ECSValue_SetTable(retFrames);

        if (image == nullptr || ImageAddFrame(retFrames, image, 0.0) != SHUResult_Ok)
        {
            ECSValue_SetNil(retFrames);
        }

        return;
    }

    SHUSlice file = ImageCheckScale(scale) ? ImageReadFile(path) : cs(nullptr, 0);

    if (file.data == nullptr)
    {
        return;
    }

    int *delays = nullptr;
    int width = 0;
    int height = 0;
    int count = 0;
    int channels = 0;
    ImageGifAvoidPrevious(file.data, file.size);
    stbi_uc *pixels = stbi_load_gif_from_memory(file.data, (int)file.size, &delays, &width, &height, &count, &channels, 4);
    SDL_free(file.data);

    if (pixels == nullptr)
    {
        ECS_LogError(IMAGE.plugin, "Cannot read the image '%s': %s", path, ImageFailure());
        return;
    }

    // the frames lie one after another, each with its delay in milliseconds
    ECSValue_SetTable(retFrames);

    for (int i = 0; i < count; i++)
    {
        void *image = ImageMake(ImageFromRgbaPixels(pixels + (usz)i * (usz)width * (usz)height * 4, width, height, scale));

        if (image == nullptr || ImageAddFrame(retFrames, image, delays == nullptr ? 0.0 : (f64)delays[i] / 1000.0) != SHUResult_Ok)
        {
            ECS_LogError(IMAGE.plugin, "Cannot make frame %d of the image '%s': out of memory", i + 1, path);
            break;
        }
    }

    stbi_image_free(delays);
    stbi_image_free(pixels);
}

/// @brief The file formats image writes.
typedef enum ImageFormat
{
    ImageFormat_Png,
    ImageFormat_Jpg,
    ImageFormat_Bmp,
    ImageFormat_Tga,
} ImageFormat;

/// @brief Writes pixels of red, green, blue and alpha bytes to a file in a format.
/// @return Whether the file was written.
static bool ImageWrite(const char *fullPath, ImageFormat format, i32 width, i32 height, const u8 *rgba)
{
    switch (format)
    {
    case ImageFormat_Png:
        return stbi_write_png(fullPath, width, height, 4, rgba, width * 4) != 0;
    case ImageFormat_Jpg:
        return stbi_write_jpg(fullPath, width, height, 4, rgba, IMAGE_JPG_QUALITY) != 0;
    case ImageFormat_Bmp:
        return stbi_write_bmp(fullPath, width, height, 4, rgba) != 0;
    case ImageFormat_Tga:
        return stbi_write_tga(fullPath, width, height, 4, rgba) != 0;
    }

    return false;
}

/// @brief Writes an image of draw to a file, in the format its extension names: .png, .jpg or .jpeg, .bmp or .tga.
/// @return Whether the file was written; the reason it was not is logged.
static bool ImageSave(void *image, const char *path)
{
    const struct
    {
        const char *extension;
        ImageFormat format;
    } formats[] = {{".png", ImageFormat_Png}, {".jpg", ImageFormat_Jpg}, {".jpeg", ImageFormat_Jpg}, {".bmp", ImageFormat_Bmp}, {".tga", ImageFormat_Tga}};
    usz chosen = 0;

    while (chosen < SDL_arraysize(formats) && !ImageEndsWith(path, formats[chosen].extension))
    {
        chosen++;
    }

    if (chosen == SDL_arraysize(formats))
    {
        ECS_LogWarn(IMAGE.plugin, "An image is written to a .png, .jpg, .jpeg, .bmp or .tga file, not '%s'.", path);
        return false;
    }

    i32 width = 0;
    i32 height = 0;
    IMAGE.ImageSize(image, &width, &height);
    SHUSlice argb = IMAGE.ImagePixels(image);

    // stb_image_write takes the bytes red, green, blue and alpha, in that order
    u8 *rgba = SDL_malloc(argb.size);
    char *fullPath = nullptr;
    bool written = rgba != nullptr && ECSPath_GetFull(&fullPath, path) == SHUResult_Ok &&
                   SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_ARGB8888, argb.data, width * 4, SDL_PIXELFORMAT_RGBA32, rgba, width * 4) &&
                   ImageWrite(fullPath, formats[chosen].format, width, height, rgba);

    if (!written)
    {
        ECS_LogError(IMAGE.plugin, "Cannot write the image '%s'", fullPath != nullptr ? fullPath : path);
    }

    SDL_free(fullPath);
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
                                                 "Read an image file into an image of draw, at a scale of its own size; an SVG file is drawn sharp at that scale. Nothing if it cannot be read\n"
                                                 "@param path The file's path, PNG, JPG, GIF, BMP, TGA, PSD, HDR, PNM or SVG; a relative path starts at the executable's folder\n"
                                                 "@param scale The scale of the file's own size, above 0; 1 keeps its size, and an SVG file's size is its own at 96 dots per inch\n"
                                                 "@return A new image, or nothing if the file cannot be read; the reason is logged"));

    SHU_ReturnResult(ECSService_RegisterFunction(plugin, "decode", (ECSFunction)ImageDecode, "handle<draw.image>(buffer bytes, float scale)",
                                                 "Read an image from the bytes of its file, such as a PNG file's, into an image of draw, at a scale as load does. Nothing if they are not an image\n"
                                                 "@param bytes The whole file's bytes; bytes that start with < after any spaces are read as SVG\n"
                                                 "@param scale The scale of the image's own size, above 0; 1 keeps its size\n"
                                                 "@return A new image, or nothing if the bytes are not an image; the reason is logged"));

    SHU_ReturnResult(ECSService_RegisterFunction(plugin, "loadFrames", (ECSFunction)ImageLoadFrames, "void(string path, float scale, out value<image.Frame[]?> frames)",
                                                 "Read the frames of an animated GIF file into images of draw, each a table of its image and the seconds it shows for; another image file gives one frame\n"
                                                 "@param path The file's path, as load takes it\n"
                                                 "@param scale The scale of each frame's own size, above 0, as load takes it\n"
                                                 "@param frames Set to the list of frames in order, each with its new image and the seconds it shows for, 0 for a file that is not a GIF; nothing if the file cannot be read"));

    return ECSService_RegisterFunction(plugin, "save", (ECSFunction)ImageSave, "bool(handle<draw.image> image, string path)",
                                       "Write an image of draw to a file, in the format its extension names: .png, .jpg, .jpeg, .bmp or .tga\n"
                                       "@param image The image to write, which is left as it is\n"
                                       "@param path The file's path, replaced if it is there; a relative path starts at the executable's folder, and a JPG file is written at a quality of 90 in 100\n"
                                       "@return Whether the file was written; the reason it was not is logged");
}
