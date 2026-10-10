// The image standard plugin: reads image files and draws them into the pixels surface of another plugin's panel (DESIGN 5).
// stb_image reads PNG, JPG, GIF, BMP, TGA, PSD, HDR and PNM files, and nanosvg reads SVG files, which are drawn sharp at any size.

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

/// @brief A name of the plugin: "image." and a local name.
#define IMAGE_NAME(localName) "image." localName

/// @brief The resolution SVG files are read at: their units become pixels at 96 per inch.
#define IMAGE_SVG_DPI 96.0f

/// @brief An image file, read once.
typedef struct ImagePicture
{
    char *path;
    i32 width; // in pixels; an SVG file's own size
    i32 height;
    SDL_Surface *surface; // ARGB; for an SVG file, its last drawing, at the size it was last drawn
    NSVGimage *svg;       // NULL for other files
} ImagePicture;

static struct
{
    ECSPlugin plugin;
    ImagePicture **pictures; // every file read so far, kept until the plugin shuts down
    usz pictureCount;
    NSVGrasterizer *rasterizer;
} IMAGE = {0};

static bool ImageEndsWith(const char *text, const char *suffix)
{
    usz length = SDL_strlen(text);
    usz suffixLength = SDL_strlen(suffix);
    return length >= suffixLength && SDL_strcasecmp(text + length - suffixLength, suffix) == 0;
}

/// @brief Draws an SVG picture at a size in pixels, if its last drawing has another size.
static bool ImageRasterize(ImagePicture *picture, i32 width, i32 height)
{
    if (picture->surface != NULL && picture->surface->w == width && picture->surface->h == height)
    {
        return true;
    }

    SDL_Surface *surface = width > 0 && height > 0 ? SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ABGR8888) : NULL;

    if (surface == NULL)
    {
        return false;
    }

    // nanosvg draws at one scale for both axes, so the picture is drawn at the scale that fits, and stretched when it is drawn
    f32 scale = SDL_min((f32)width / (f32)picture->width, (f32)height / (f32)picture->height);
    SDL_memset(surface->pixels, 0, (usz)surface->pitch * (usz)height);
    nsvgRasterize(IMAGE.rasterizer, picture->svg, 0.0f, 0.0f, scale, surface->pixels, width, height, surface->pitch);

    SDL_Surface *argb = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(surface);

    if (argb == NULL)
    {
        return false;
    }

    SDL_DestroySurface(picture->surface);
    picture->surface = argb;
    return true;
}

/// @brief Reads an image file.
/// @return The picture, or NULL if the file cannot be read; the reason is logged.
static ImagePicture *ImageRead(const char *path)
{
    char *full = NULL;
    ImagePicture *picture = SDL_calloc(1, sizeof(ImagePicture));

    // a relative path starts at the executable's folder
    if (picture == NULL || SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
    {
        SDL_free(picture);
        return NULL;
    }

    if (ImageEndsWith(full, ".svg"))
    {
        picture->svg = nsvgParseFromFile(full, "px", IMAGE_SVG_DPI);

        if (picture->svg != NULL && picture->svg->width > 0.0f && picture->svg->height > 0.0f)
        {
            picture->width = (i32)SDL_ceilf(picture->svg->width);
            picture->height = (i32)SDL_ceilf(picture->svg->height);
        }
    }
    else
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels = stbi_load(full, &width, &height, &channels, 4);
        SDL_Surface *rgba = pixels == NULL ? NULL : SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ABGR8888, pixels, width * 4);
        picture->surface = rgba == NULL ? NULL : SDL_ConvertSurface(rgba, SDL_PIXELFORMAT_ARGB8888);
        picture->width = width;
        picture->height = height;
        SDL_DestroySurface(rgba);
        stbi_image_free(pixels);
    }

    if (picture->surface == NULL && picture->svg == NULL)
    {
        ECS_Log(IMAGE.plugin, ECSLogLevel_Error, "Cannot read the image '%s': %s", full, stbi_failure_reason() != NULL ? stbi_failure_reason() : "not an image");
        SDL_free(full);
        SDL_free(picture);
        return NULL;
    }

    SDL_free(full);
    picture->path = SDL_strdup(path);
    return picture;
}

/// @brief Gets an image file, reading it the first time; a file that cannot be read is tried again next time.
static ImagePicture *ImageLoad(const char *path)
{
    for (usz i = 0; i < IMAGE.pictureCount; i++)
    {
        if (SDL_strcmp(IMAGE.pictures[i]->path, path) == 0)
        {
            return IMAGE.pictures[i];
        }
    }

    ImagePicture *picture = ImageRead(path);
    ImagePicture **pictures = picture == NULL || picture->path == NULL ? NULL : SDL_realloc(IMAGE.pictures, (IMAGE.pictureCount + 1) * sizeof(ImagePicture *));

    if (pictures == NULL)
    {
        if (picture != NULL)
        {
            nsvgDelete(picture->svg);
            SDL_DestroySurface(picture->surface);
            SDL_free(picture->path);
            SDL_free(picture);
        }

        return NULL;
    }

    IMAGE.pictures = pictures;
    IMAGE.pictures[IMAGE.pictureCount++] = picture;
    return picture;
}

static void ImageSize(ImagePicture *picture, i32 *retWidth, i32 *retHeight)
{
    *retWidth = picture->width;
    *retHeight = picture->height;
}

static void ImageDraw(ECSSurface *surface, ImagePicture *picture, f32 x, f32 y, f32 width, f32 height)
{
    SDL_Rect rect = {(int)SDL_roundf(x * surface->scale), (int)SDL_roundf(y * surface->scale), (int)SDL_roundf(width * surface->scale), (int)SDL_roundf(height * surface->scale)};

    if (surface->type != ECSSurfaceType_Pixels || (picture->svg != NULL && !ImageRasterize(picture, rect.w, rect.h)))
    {
        return;
    }

    SDL_Surface *target = SDL_CreateSurfaceFrom(surface->width, surface->height, SDL_PIXELFORMAT_ARGB8888, surface->pixels.data, surface->pitch);

    if (target != NULL)
    {
        SDL_BlitSurfaceScaled(picture->surface, NULL, target, &rect, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(target);
    }
}

static i64 ImagePixel(ImagePicture *picture, i32 x, i32 y)
{
    if (picture->surface == NULL || x < 0 || y < 0 || x >= picture->surface->w || y >= picture->surface->h)
    {
        return 0;
    }

    return *(u32 *)((u8 *)picture->surface->pixels + (usz)y * (usz)picture->surface->pitch + (usz)x * 4);
}

/// @brief A picture's handle stays valid until the plugin shuts down, which frees the pictures; the handle has nothing to free.
static void ImageForget(void *object)
{
    (void)object;
}

static void ImageFree(void)
{
    for (usz i = 0; i < IMAGE.pictureCount; i++)
    {
        nsvgDelete(IMAGE.pictures[i]->svg);
        SDL_DestroySurface(IMAGE.pictures[i]->surface);
        SDL_free(IMAGE.pictures[i]->path);
        SDL_free(IMAGE.pictures[i]);
    }

    nsvgDeleteRasterizer(IMAGE.rasterizer);
    SDL_free(IMAGE.pictures);
    SDL_zero(IMAGE);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    IMAGE.plugin = plugin;
    IMAGE.rasterizer = nsvgCreateRasterizer();

    if (IMAGE.rasterizer == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    SHU_ReturnResult(ECSHandle_RegisterType(plugin, IMAGE_NAME("picture"), ImageForget), ImageFree(););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {IMAGE_NAME("load"), (ECSFunction)ImageLoad, "handle<image.picture>(string path)", "Read an image file, once; nothing if it cannot be read"},
        {IMAGE_NAME("size"), (ECSFunction)ImageSize, "void(handle<image.picture> picture, out int width, out int height)", "Give a picture's size in pixels"},
        {IMAGE_NAME("draw"), (ECSFunction)ImageDraw, "void(handle<ecs.surface> surface, handle<image.picture> picture, float x, float y, float width, float height)", "Draw a picture stretched to a rectangle"},
        {IMAGE_NAME("pixel"), (ECSFunction)ImagePixel, "int64(handle<image.picture> picture, int x, int y)", "Give a pixel's colour as ARGB"},
    };

    // a plugin whose Init fails gets no Shutdown, so it cleans up here
    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), ImageFree(););
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
    ImageFree();
}
