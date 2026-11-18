// The compress standard plugin: packs and unpacks data with gzip, zlib, deflate and zstd, and reads and writes zip archives (DESIGN 18).
// zlib packs gzip, zlib and deflate, zstd packs zstd, and zlib's minizip reads and writes zip archives; all are compiled into this plugin (DESIGN 1).
// minizip reads and writes files through SDL, so paths are UTF-8 on every system. A function that fails gives nothing and why, which is kept until the plugin's next call.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "unzip.h"
#include "zip.h"
#include "zlib.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

/// @brief How much an unpacked buffer grows at least, when it is full.
#define COMPRESS_GROWTH 65536

/// @brief The most bytes that minizip reads or writes in one call.
#define COMPRESS_ZIP_CHUNK 0x40000000u

typedef enum CompressFormat
{
    CompressFormat_Gzip,
    CompressFormat_Zlib,
    CompressFormat_Deflate,
    CompressFormat_Zstd,
    CompressFormat_Detect, // gzip, zlib or zstd, told by the data's first bytes
} CompressFormat;

static struct
{
    ECSPlugin plugin;
} COMPRESS = {0};

/// @brief Fails a call, as ECS_Fail does, and gives why.
static OPENECS_PRINTF(1, 2) const char *CompressFail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    char *message = nullptr;
    bool formatted = SDL_vasprintf(&message, format, arguments) >= 0;
    va_end(arguments);

    const char *error = nullptr;
    ECS_Fail(COMPRESS.plugin, &error, "%s", formatted ? message : "out of memory");
    SDL_free(message);
    return error;
}

/// @brief Makes a value a buffer of the bytes of an stb_ds array, or an empty string if it has none.
static bool CompressSetBytes(ECSValue *retValue, const u8 *bytes, const char **retError)
{
    SHUResult result = arrlenu(bytes) == 0 ? ECSValue_SetString(retValue, "") : ECSValue_SetBuffer(retValue, (SHUSliceView){bytes, arrlenu(bytes)});

    if (result != SHUResult_Ok)
    {
        *retError = CompressFail("out of memory");
        ECSValue_SetNil(retValue);
    }

    return result == SHUResult_Ok;
}

static bool CompressReadFormat(const char *name, bool detect, CompressFormat *retFormat)
{
    const char *const names[] = {"gzip", "zlib", "deflate", "zstd", ""};

    for (usz i = 0; i < SDL_arraysize(names); i++)
    {
        if (SDL_strcmp(name, names[i]) == 0 && (detect || i != CompressFormat_Detect))
        {
            *retFormat = (CompressFormat)i;
            return true;
        }
    }

    (void)ECS_Check(COMPRESS.plugin, false, "'%s' is not a format: \"gzip\", \"zlib\", \"deflate\" or \"zstd\"%s", name, detect ? ", or an empty text to tell it from the data" : "");
    return false;
}

/// @brief Tells the format of packed data by its first bytes: gzip's, zstd's or zlib's.
static bool CompressDetect(SHUSlice data, CompressFormat *retFormat, const char **retError)
{
    const u8 *bytes = data.data;

    if (data.size >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B)
    {
        *retFormat = CompressFormat_Gzip;
    }
    else if (data.size >= 4 && bytes[0] == 0x28 && bytes[1] == 0xB5 && bytes[2] == 0x2F && bytes[3] == 0xFD)
    {
        *retFormat = CompressFormat_Zstd;
    }
    else if (data.size >= 2 && ((u32)bytes[0] & 0x0Fu) == 8 && (((u32)bytes[0] << 8u) | bytes[1]) % 31 == 0)
    {
        *retFormat = CompressFormat_Zlib;
    }
    else
    {
        *retError = CompressFail("the data's format is not gzip, zlib or zstd; deflate's must be named");
        return false;
    }

    return true;
}

#pragma endregion Source Only

#pragma region zlib

static voidpf CompressZlibAlloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    return SDL_calloc(items, size);
}

static void CompressZlibFree(voidpf opaque, voidpf address)
{
    (void)opaque;
    SDL_free(address);
}

/// @brief zlib's window bits for a format: they choose gzip's or zlib's header, or none for deflate.
static int CompressZlibBits(CompressFormat format)
{
    return format == CompressFormat_Gzip ? 15 + 16 : format == CompressFormat_Zlib ? 15
                                                                                   : -15;
}

static bool CompressPackZlib(SHUSlice data, CompressFormat format, int level, u8 **retPacked, const char **retError)
{
    if (!ECS_Check(COMPRESS.plugin, level >= 0 && level <= 9, "%s's levels are 1 to 9, and 0 for its default", format == CompressFormat_Gzip ? "gzip" : format == CompressFormat_Zlib ? "zlib"
                                                                                                                                                                                      : "deflate"))
    {
        return false;
    }

    z_stream stream = {.zalloc = CompressZlibAlloc, .zfree = CompressZlibFree};

    if (data.size > UINT_MAX || deflateInit2(&stream, level == 0 ? Z_DEFAULT_COMPRESSION : level, Z_DEFLATED, CompressZlibBits(format), 8, Z_DEFAULT_STRATEGY) != Z_OK)
    {
        *retError = CompressFail(data.size > UINT_MAX ? "the data is too large to pack at once" : "out of memory");
        return false;
    }

    uLong bound = deflateBound(&stream, (uLong)data.size);
    arrsetlen(*retPacked, bound);
    stream.next_in = data.data;
    stream.avail_in = (uInt)data.size;
    stream.next_out = *retPacked;
    stream.avail_out = (uInt)bound;
    int status = deflate(&stream, Z_FINISH);
    arrsetlen(*retPacked, (usz)(stream.next_out - *retPacked));
    deflateEnd(&stream);

    if (status != Z_STREAM_END)
    {
        *retError = CompressFail("zlib cannot pack the data: %s", stream.msg != nullptr ? stream.msg : "out of memory");
        return false;
    }

    return true;
}

static bool CompressUnpackZlib(SHUSlice data, CompressFormat format, u8 **retData, const char **retError)
{
    z_stream stream = {.zalloc = CompressZlibAlloc, .zfree = CompressZlibFree};

    if (data.size > UINT_MAX || inflateInit2(&stream, CompressZlibBits(format)) != Z_OK)
    {
        *retError = CompressFail(data.size > UINT_MAX ? "the data is too large to unpack at once" : "out of memory");
        return false;
    }

    stream.next_in = data.data;
    stream.avail_in = (uInt)data.size;
    usz used = 0;
    int status = Z_OK;

    // the unpacked size is not known, so the buffer grows until the stream ends
    while (status == Z_OK || status == Z_BUF_ERROR)
    {
        if (used == arrlenu(*retData))
        {
            arrsetlen(*retData, used + SDL_max(used, COMPRESS_GROWTH));
        }

        stream.next_out = *retData + used;
        stream.avail_out = (uInt)SDL_min(arrlenu(*retData) - used, UINT_MAX);
        status = inflate(&stream, Z_NO_FLUSH);
        used = (usz)(stream.next_out - *retData);

        // all the data was read, and there is room, yet the stream goes on
        if (status != Z_STREAM_END && stream.avail_in == 0 && stream.avail_out > 0)
        {
            status = Z_DATA_ERROR;
            stream.msg = "the data ends too early";
        }
    }

    arrsetlen(*retData, used);
    inflateEnd(&stream);

    if (status != Z_STREAM_END)
    {
        *retError = CompressFail("zlib cannot unpack the data: %s", stream.msg != nullptr ? stream.msg : "out of memory");
        return false;
    }

    return true;
}

#pragma endregion zlib

#pragma region zstd

static void *CompressZstdAlloc(void *opaque, size_t size)
{
    (void)opaque;
    return SDL_malloc(size);
}

static void CompressZstdFree(void *opaque, void *address)
{
    (void)opaque;
    SDL_free(address);
}

/// @brief zstd takes SDL's allocator, as the plugin's other libraries do (DESIGN 1.3).
static const ZSTD_customMem COMPRESS_ZSTD_MEMORY = {CompressZstdAlloc, CompressZstdFree, nullptr};

static bool CompressPackZstd(SHUSlice data, int level, u8 **retPacked, const char **retError)
{
    if (!ECS_Check(COMPRESS.plugin, level >= ZSTD_minCLevel() && level <= ZSTD_maxCLevel(), "zstd's levels are %d to %d, and 0 for its default", ZSTD_minCLevel(), ZSTD_maxCLevel()))
    {
        return false;
    }

    ZSTD_CCtx *context = ZSTD_createCCtx_advanced(COMPRESS_ZSTD_MEMORY);

    if (context == nullptr)
    {
        *retError = CompressFail("out of memory");
        return false;
    }

    arrsetlen(*retPacked, ZSTD_compressBound(data.size));
    size_t size = ZSTD_compressCCtx(context, *retPacked, arrlenu(*retPacked), data.data, data.size, level);
    ZSTD_freeCCtx(context);

    if (ZSTD_isError(size))
    {
        *retError = CompressFail("zstd cannot pack the data: %s", ZSTD_getErrorName(size));
        return false;
    }

    arrsetlen(*retPacked, size);
    return true;
}

static bool CompressUnpackZstd(SHUSlice data, u8 **retData, const char **retError)
{
    ZSTD_DCtx *context = ZSTD_createDCtx_advanced(COMPRESS_ZSTD_MEMORY);

    if (context == nullptr)
    {
        *retError = CompressFail("out of memory");
        return false;
    }

    ZSTD_inBuffer input = {data.data, data.size, 0};
    usz used = 0;
    size_t status = 0;

    // each frame ends with status 0; the data may hold many frames, and the buffer grows until all are read
    do
    {
        if (used == arrlenu(*retData))
        {
            arrsetlen(*retData, used + SDL_max(used, COMPRESS_GROWTH));
        }

        ZSTD_outBuffer output = {*retData, arrlenu(*retData), used};
        status = ZSTD_decompressStream(context, &output, &input);
        used = output.pos;
    } while (!ZSTD_isError(status) && (input.pos < input.size || (status != 0 && used == arrlenu(*retData))));

    arrsetlen(*retData, used);
    ZSTD_freeDCtx(context);

    if (ZSTD_isError(status) || status != 0)
    {
        *retError = CompressFail("zstd cannot unpack the data: %s", ZSTD_isError(status) ? ZSTD_getErrorName(status) : "the data ends too early");
        return false;
    }

    return true;
}

#pragma endregion zstd

#pragma region Streams

static void CompressPack(const char *format, SHUSlice data, int level, ECSValue *retPacked, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retPacked);
    CompressFormat chosen = CompressFormat_Gzip;

    if (!CompressReadFormat(format, false, &chosen))
    {
        return;
    }

    u8 *packed = nullptr;
    bool done = chosen == CompressFormat_Zstd ? CompressPackZstd(data, level, &packed, retError) : CompressPackZlib(data, chosen, level, &packed, retError);

    if (done)
    {
        CompressSetBytes(retPacked, packed, retError);
    }

    arrfree(packed);
}

static void CompressUnpack(const char *format, SHUSlice data, ECSValue *retData, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retData);
    CompressFormat chosen = CompressFormat_Gzip;

    if (!CompressReadFormat(format, true, &chosen) || (chosen == CompressFormat_Detect && !CompressDetect(data, &chosen, retError)))
    {
        return;
    }

    u8 *unpacked = nullptr;
    bool done = chosen == CompressFormat_Zstd ? CompressUnpackZstd(data, &unpacked, retError) : CompressUnpackZlib(data, chosen, &unpacked, retError);

    if (done)
    {
        CompressSetBytes(retData, unpacked, retError);
    }

    arrfree(unpacked);
}

/// @brief Defines a format's pack and unpack functions, which Lua finds in the format's namespace, such as compress.gzip.pack.
#define COMPRESS_FORMAT_FUNCTIONS(Name, format)                                                            \
    static void Compress##Name##Pack(SHUSlice data, int level, ECSValue *retPacked, const char **retError) \
    {                                                                                                      \
        CompressPack(format, data, level, retPacked, retError);                                            \
    }                                                                                                      \
                                                                                                           \
    static void Compress##Name##Unpack(SHUSlice data, ECSValue *retData, const char **retError)            \
    {                                                                                                      \
        CompressUnpack(format, data, retData, retError);                                                   \
    }

COMPRESS_FORMAT_FUNCTIONS(Gzip, "gzip")
COMPRESS_FORMAT_FUNCTIONS(Zlib, "zlib")
COMPRESS_FORMAT_FUNCTIONS(Deflate, "deflate")
COMPRESS_FORMAT_FUNCTIONS(Zstd, "zstd")

/// @brief Unpacks data whose format its first bytes tell: gzip, zlib or zstd.
static void CompressUnpackAny(SHUSlice data, ECSValue *retData, const char **retError)
{
    CompressUnpack("", data, retData, retError);
}

#pragma endregion Streams

#pragma region Files

static voidpf ZCALLBACK CompressFileOpen(voidpf opaque, const void *path, int mode)
{
    (void)opaque;
    u32 flags = (u32)mode;
    bool read = (flags & (u32)ZLIB_FILEFUNC_MODE_READWRITEFILTER) == (u32)ZLIB_FILEFUNC_MODE_READ;
    return SDL_IOFromFile(path, read ? "rb" : (flags & (u32)ZLIB_FILEFUNC_MODE_EXISTING) != 0 ? "r+b"
                                                                                              : "wb");
}

static uLong ZCALLBACK CompressFileRead(voidpf opaque, voidpf stream, void *buffer, uLong size)
{
    (void)opaque;
    return (uLong)SDL_ReadIO(stream, buffer, size);
}

static uLong ZCALLBACK CompressFileWrite(voidpf opaque, voidpf stream, const void *buffer, uLong size)
{
    (void)opaque;
    return (uLong)SDL_WriteIO(stream, buffer, size);
}

static ZPOS64_T ZCALLBACK CompressFileTell(voidpf opaque, voidpf stream)
{
    (void)opaque;
    return (ZPOS64_T)SDL_TellIO(stream);
}

static long ZCALLBACK CompressFileSeek(voidpf opaque, voidpf stream, ZPOS64_T offset, int origin)
{
    (void)opaque;
    SDL_IOWhence whence = origin == ZLIB_FILEFUNC_SEEK_CUR ? SDL_IO_SEEK_CUR : origin == ZLIB_FILEFUNC_SEEK_END ? SDL_IO_SEEK_END
                                                                                                                : SDL_IO_SEEK_SET;
    return SDL_SeekIO(stream, (Sint64)offset, whence) < 0 ? -1 : 0;
}

static int ZCALLBACK CompressFileClose(voidpf opaque, voidpf stream)
{
    (void)opaque;
    return SDL_CloseIO(stream) ? 0 : -1;
}

static int ZCALLBACK CompressFileError(voidpf opaque, voidpf stream)
{
    (void)opaque;
    return SDL_GetIOStatus(stream) == SDL_IO_STATUS_ERROR ? 1 : 0;
}

/// @brief minizip's files are SDL's streams.
static zlib_filefunc64_def COMPRESS_ZIP_FILES = {CompressFileOpen, CompressFileRead, CompressFileWrite, CompressFileTell, CompressFileSeek, CompressFileClose, CompressFileError, nullptr};

/// @brief Gives the name of the archive's current entry.
/// @return The name, which the caller frees with SDL_free, or nullptr if it cannot be read.
static char *CompressGetEntryName(unzFile archive, unz_file_info64 *retInfo)
{
    if (unzGetCurrentFileInfo64(archive, retInfo, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
    {
        return nullptr;
    }

    char *name = SDL_malloc(retInfo->size_filename + 1);

    if (name != nullptr && unzGetCurrentFileInfo64(archive, retInfo, name, retInfo->size_filename + 1, nullptr, 0, nullptr, 0) != UNZ_OK)
    {
        SDL_free(name);
        return nullptr;
    }

    return name;
}

/// @brief Reads the archive's current entry, unpacked, and checks it against its checksum.
static bool CompressReadEntry(unzFile archive, const unz_file_info64 *info, u8 **retData)
{
    if (info->uncompressed_size > SIZE_MAX || unzOpenCurrentFile(archive) != UNZ_OK)
    {
        return false;
    }

    arrsetlen(*retData, (usz)info->uncompressed_size);
    usz used = 0;
    int read = 1;

    while (read > 0 && used < arrlenu(*retData))
    {
        read = unzReadCurrentFile(archive, *retData + used, (unsigned)SDL_min(arrlenu(*retData) - used, COMPRESS_ZIP_CHUNK));
        used += read > 0 ? (usz)read : 0;
    }

    return unzCloseCurrentFile(archive) == UNZ_OK && read >= 0 && used == arrlenu(*retData);
}

/// @brief Opens an archive to read.
static unzFile CompressOpenArchive(const char *path, const char **retError)
{
    char *fullPath = nullptr;
    unzFile archive = ECSPath_GetFull(&fullPath, path) == SHUResult_Ok ? unzOpen2_64(fullPath, &COMPRESS_ZIP_FILES) : nullptr;
    ECSPath_Free(fullPath);

    if (archive == nullptr)
    {
        *retError = CompressFail("'%s' cannot be read as a zip archive", path);
    }

    return archive;
}

/// @brief Adds an entry to the list of an archive's entries.
static bool CompressAddEntry(ECSValue *entries, const char *name, const unz_file_info64 *info)
{
    ECSValue *entry = nullptr;
    ECSValue *field = nullptr;

    if (ECSValue_AddListItem(entries, &entry) != SHUResult_Ok)
    {
        return false;
    }

    ECSValue_SetTable(entry);
    bool added = ECSValue_SetTableField(entry, "name", &field) == SHUResult_Ok && ECSValue_SetString(field, name) == SHUResult_Ok;
    added = added && ECSValue_SetTableField(entry, "size", &field) == SHUResult_Ok;

    if (added)
    {
        ECSValue_SetInteger(field, (i64)info->uncompressed_size);
    }

    added = added && ECSValue_SetTableField(entry, "packedSize", &field) == SHUResult_Ok;

    if (added)
    {
        ECSValue_SetInteger(field, (i64)info->compressed_size);
    }

    return added;
}

static void CompressZipList(const char *path, ECSValue *retEntries, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retEntries);
    unzFile archive = CompressOpenArchive(path, retError);

    if (archive == nullptr)
    {
        return;
    }

    ECSValue_SetTable(retEntries);
    int status = unzGoToFirstFile(archive);

    while (status == UNZ_OK)
    {
        unz_file_info64 info = {0};
        char *name = CompressGetEntryName(archive, &info);
        bool added = name != nullptr && CompressAddEntry(retEntries, name, &info);
        SDL_free(name);
        status = added ? unzGoToNextFile(archive) : UNZ_ERRNO;
    }

    if (status != UNZ_END_OF_LIST_OF_FILE)
    {
        *retError = CompressFail("the zip archive '%s' cannot be read", path);
        ECSValue_SetNil(retEntries);
    }

    unzClose(archive);
}

static void CompressZipRead(const char *path, const char *name, ECSValue *retData, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retData);
    unzFile archive = CompressOpenArchive(path, retError);

    if (archive == nullptr)
    {
        return;
    }

    unz_file_info64 info = {0};
    u8 *data = nullptr;

    if (unzLocateFile(archive, name, 1) != UNZ_OK)
    {
        *retError = CompressFail("the zip archive '%s' has no '%s'", path, name);
    }
    else if (unzGetCurrentFileInfo64(archive, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK || !CompressReadEntry(archive, &info, &data))
    {
        *retError = CompressFail("'%s' of the zip archive '%s' cannot be read", name, path);
    }
    else
    {
        CompressSetBytes(retData, data, retError);
    }

    arrfree(data);
    unzClose(archive);
}

/// @brief Tells whether an entry's name stays inside the folder it is extracted to: it is not absolute and does not go up with '..'.
static bool CompressIsInside(const char *name)
{
    if (name[0] == '/' || name[0] == '\\' || SDL_strchr(name, ':') != nullptr)
    {
        return false;
    }

    for (const char *part = name; *part != '\0'; part++)
    {
        bool start = part == name || part[-1] == '/' || part[-1] == '\\';

        if (start && part[0] == '.' && part[1] == '.' && (part[2] == '\0' || part[2] == '/' || part[2] == '\\'))
        {
            return false;
        }
    }

    return true;
}

/// @brief Writes the archive's current entry into a folder: a folder makes the folder, and a file is written, with the folders above it.
static bool CompressExtractEntry(unzFile archive, const char *folder, const char **retError)
{
    unz_file_info64 info = {0};
    char *name = CompressGetEntryName(archive, &info);
    char *target = nullptr;
    u8 *data = nullptr;
    bool extracted = false;

    if (name == nullptr || !CompressIsInside(name))
    {
        *retError = CompressFail(name == nullptr ? "an entry's name cannot be read" : "the entry '%s' would go outside the folder", name);
    }
    else if (SDL_asprintf(&target, "%s/%s", folder, name) < 0)
    {
        *retError = CompressFail("out of memory");
    }
    else if (name[0] != '\0' && name[SDL_strlen(name) - 1] == '/')
    {
        extracted = SDL_CreateDirectory(target);
    }
    else
    {
        // the folders above the file come first
        const char *separator = SDL_strrchr(target, '/');
        char *parent = SDL_strndup(target, (usz)(separator - target));
        extracted = parent != nullptr && SDL_CreateDirectory(parent) && CompressReadEntry(archive, &info, &data) && SDL_SaveFile(target, data, arrlenu(data));
        SDL_free(parent);
    }

    if (!extracted && *retError == nullptr)
    {
        *retError = CompressFail("'%s' cannot be extracted to '%s'", name, target);
    }

    arrfree(data);
    SDL_free(target);
    SDL_free(name);
    return extracted;
}

static bool CompressZipExtract(const char *path, const char *folder, const char **retError)
{
    *retError = nullptr;
    unzFile archive = CompressOpenArchive(path, retError);
    char *fullFolder = nullptr;

    if (archive == nullptr || ECSPath_GetFull(&fullFolder, folder) != SHUResult_Ok)
    {
        *retError = *retError != nullptr ? *retError : CompressFail("out of memory");
        unzClose(archive);
        return false;
    }

    int status = unzGoToFirstFile(archive);

    while (status == UNZ_OK)
    {
        status = CompressExtractEntry(archive, fullFolder, retError) ? unzGoToNextFile(archive) : UNZ_ERRNO;
    }

    if (status != UNZ_END_OF_LIST_OF_FILE && *retError == nullptr)
    {
        *retError = CompressFail("the zip archive '%s' cannot be read", path);
    }

    ECSPath_Free(fullFolder);
    unzClose(archive);
    return *retError == nullptr;
}

/// @brief Writes a file into an archive, packed with deflate, with the current time.
static bool CompressWriteEntry(zipFile archive, const char *name, const u8 *data, usz size, int level)
{
    zip_fileinfo info = {0};
    SDL_Time now = 0;
    SDL_DateTime date = {0};

    // zip archives keep local times
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &date, true))
    {
        info.tmz_date = (tm_zip){date.second, date.minute, date.hour, date.day, date.month - 1, date.year};
    }

    if (zipOpenNewFileInZip64(archive, name, &info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, level == 0 ? Z_DEFAULT_COMPRESSION : level, size >= 0xFFFFFFFFu) != ZIP_OK)
    {
        return false;
    }

    bool written = true;

    for (usz used = 0; used < size && written;)
    {
        unsigned chunk = (unsigned)SDL_min(size - used, COMPRESS_ZIP_CHUNK);
        written = zipWriteInFileInZip(archive, data + used, chunk) == ZIP_OK;
        used += chunk;
    }

    return zipCloseFileInZip(archive) == ZIP_OK && written;
}

/// @brief Writes the files of a list into an archive.
static bool CompressWriteFiles(zipFile archive, const ECSValue *files, int level, const char **retError)
{
    for (usz i = 0; i < ECSValue_GetListCount(files); i++)
    {
        const ECSValue *file = ECSValue_GetListItem(files, i);
        const char *name = ECSValue_GetString(ECSValue_GetTableField(file, "name"), nullptr);
        SHUSlice data = ECSValue_GetBuffer(ECSValue_GetTableField(file, "data"), cs(nullptr, 0));

        if (name == nullptr || name[0] == '\0')
        {
            *retError = CompressFail("the file %zu has no name", i + 1);
            return false;
        }

        if (!CompressWriteEntry(archive, name, data.data, data.size, level))
        {
            *retError = CompressFail("'%s' cannot be written into the zip archive", name);
            return false;
        }
    }

    return true;
}

static bool CompressZipWrite(const char *path, const ECSValue *files, int level, const char **retError)
{
    *retError = nullptr;

    if (!ECS_Check(COMPRESS.plugin, level >= 0 && level <= 9, "a zip archive's levels are 1 to 9, and 0 for its default"))
    {
        return false;
    }

    char *fullPath = nullptr;
    zipFile archive = ECSPath_GetFull(&fullPath, path) == SHUResult_Ok ? zipOpen2_64(fullPath, APPEND_STATUS_CREATE, nullptr, &COMPRESS_ZIP_FILES) : nullptr;
    bool written = archive != nullptr && CompressWriteFiles(archive, files, level, retError);

    if (archive != nullptr && zipClose(archive, nullptr) != ZIP_OK)
    {
        written = false;
    }

    // a half-written archive is not left behind
    if (!written)
    {
        *retError = *retError != nullptr ? *retError : CompressFail("the zip archive '%s' cannot be written", path);

        if (archive != nullptr)
        {
            SDL_RemovePath(fullPath);
        }
    }

    ECSPath_Free(fullPath);
    return written;
}

#pragma endregion Files

#pragma region tar

/// @brief A tar archive is made of blocks of 512 bytes: a header for each entry, then its bytes; two empty blocks end it.
#define COMPRESS_TAR_BLOCK ((usz)512)

/// @brief The largest size that a ustar header writes: eleven octal digits.
#define COMPRESS_TAR_LARGEST 077777777777ull

/// @brief Writes a number into a header's field in octal, with a zero at its end, as ustar has it.
static void CompressTarOctal(u8 *field, usz width, u64 value)
{
    char text[24];
    SDL_snprintf(text, sizeof(text), "%0*llo", (int)(width - 1), (unsigned long long)value);
    SDL_memcpy(field, text, width - 1);
}

/// @brief Reads a header's octal field; a field may end with a zero or a space.
static u64 CompressTarReadOctal(const u8 *field, usz width)
{
    u64 value = 0;

    for (usz i = 0; i < width && field[i] >= '0' && field[i] <= '7'; i++)
    {
        value = value * 8 + (u64)(field[i] - '0');
    }

    return value;
}

/// @brief Adds an entry's header to an archive. A name longer than 100 bytes is split at a slash, into ustar's prefix of up to
/// 155 bytes and its name.
static bool CompressTarHeader(u8 **archive, const char *name, u64 size, bool folder, bool executable, const char **retError)
{
    usz length = SDL_strlen(name);
    usz split = 0;
    bool splits = false;

    if (length > 100)
    {
        for (usz i = length - 101; i < length && i <= 155 && !splits; i++)
        {
            splits = name[i] == '/';
            split = i;
        }

        if (!splits)
        {
            *retError = CompressFail("the name is too long for a tar archive: '%s'", name);
            return false;
        }
    }

    if (size > COMPRESS_TAR_LARGEST)
    {
        *retError = CompressFail("'%s' is too large for a tar archive", name);
        return false;
    }

    usz offset = arrlenu(*archive);
    arrsetlen(*archive, offset + COMPRESS_TAR_BLOCK);
    u8 *header = *archive + offset;
    SDL_memset(header, 0, COMPRESS_TAR_BLOCK);

    if (splits)
    {
        SDL_memcpy(header + 345, name, split);
        SDL_memcpy(header, name + split + 1, length - split - 1);
    }
    else
    {
        SDL_memcpy(header, name, length);
    }

    CompressTarOctal(header + 100, 8, folder || executable ? 0755 : 0644);
    CompressTarOctal(header + 108, 8, 0);
    CompressTarOctal(header + 116, 8, 0);
    CompressTarOctal(header + 124, 12, size);
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    CompressTarOctal(header + 136, 12, (u64)SDL_max(now / SDL_NS_PER_SECOND, 0));
    header[156] = folder ? '5' : '0';
    SDL_memcpy(header + 257, "ustar", 6);
    SDL_memcpy(header + 263, "00", 2);

    // the checksum is the sum of the header's bytes, with its own field as spaces
    SDL_memset(header + 148, ' ', 8);
    u64 sum = 0;

    for (usz i = 0; i < COMPRESS_TAR_BLOCK; i++)
    {
        sum += header[i];
    }

    // six digits, a zero and a space, as tar writes it
    CompressTarOctal(header + 148, 7, sum);
    header[154] = '\0';
    return true;
}

static void CompressTarPack(const ECSValue *files, ECSValue *retArchive, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retArchive);
    u8 *archive = nullptr;
    bool packed = true;

    for (usz i = 0; i < ECSValue_GetListCount(files) && packed; i++)
    {
        const ECSValue *file = ECSValue_GetListItem(files, i);
        const char *name = ECSValue_GetString(ECSValue_GetTableField(file, "name"), nullptr);
        SHUSlice data = ECSValue_GetBuffer(ECSValue_GetTableField(file, "data"), cs(nullptr, 0));
        bool executable = ECSValue_GetBool(ECSValue_GetTableField(file, "executable"), false);

        if (name == nullptr || name[0] == '\0')
        {
            *retError = CompressFail("the file %zu has no name", i + 1);
            packed = false;
            break;
        }

        bool folder = name[SDL_strlen(name) - 1] == '/';
        packed = CompressTarHeader(&archive, name, folder ? 0 : data.size, folder, executable, retError);

        if (packed && !folder && data.size > 0)
        {
            // the bytes, then zeros to the end of their last block
            usz offset = arrlenu(archive);
            arrsetlen(archive, offset + (data.size + COMPRESS_TAR_BLOCK - 1) / COMPRESS_TAR_BLOCK * COMPRESS_TAR_BLOCK);
            SDL_memcpy(archive + offset, data.data, data.size);
            SDL_memset(archive + offset + data.size, 0, arrlenu(archive) - offset - data.size);
        }
    }

    if (packed)
    {
        usz offset = arrlenu(archive);
        arrsetlen(archive, offset + COMPRESS_TAR_BLOCK * 2);
        SDL_memset(archive + offset, 0, COMPRESS_TAR_BLOCK * 2);
        CompressSetBytes(retArchive, archive, retError);
    }

    arrfree(archive);
}

/// @brief Adds a file of an archive to the list that tar.unpack gives.
static bool CompressTarAddFile(ECSValue *files, const char *name, SHUSliceView data, bool executable)
{
    ECSValue *file = nullptr;
    ECSValue *field = nullptr;

    if (ECSValue_AddListItem(files, &file) != SHUResult_Ok)
    {
        return false;
    }

    ECSValue_SetTable(file);
    bool added = ECSValue_SetTableField(file, "name", &field) == SHUResult_Ok && ECSValue_SetString(field, name) == SHUResult_Ok;
    added = added && ECSValue_SetTableField(file, "data", &field) == SHUResult_Ok;
    added = added && (data.size == 0 ? ECSValue_SetString(field, "") : ECSValue_SetBuffer(field, data)) == SHUResult_Ok;
    added = added && ECSValue_SetTableField(file, "executable", &field) == SHUResult_Ok;

    if (added)
    {
        ECSValue_SetBool(field, executable);
    }

    return added;
}

static void CompressTarUnpack(SHUSlice data, ECSValue *retFiles, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetTable(retFiles);
    const u8 *bytes = data.data;
    usz offset = 0;

    // an empty block ends the archive; without one, so does its end
    while (offset + COMPRESS_TAR_BLOCK <= data.size && bytes[offset] != '\0')
    {
        const u8 *header = bytes + offset;
        u64 sum = 0;

        for (usz i = 0; i < COMPRESS_TAR_BLOCK; i++)
        {
            sum += i >= 148 && i < 156 ? ' ' : header[i];
        }

        u64 size = CompressTarReadOctal(header + 124, 12);
        usz blocks = (usz)((size + COMPRESS_TAR_BLOCK - 1) / COMPRESS_TAR_BLOCK);

        if (sum != CompressTarReadOctal(header + 148, 8) || size > data.size - offset - COMPRESS_TAR_BLOCK || blocks * COMPRESS_TAR_BLOCK > data.size - offset - COMPRESS_TAR_BLOCK)
        {
            *retError = CompressFail("the tar archive is damaged at byte %zu", offset);
            ECSValue_SetNil(retFiles);
            return;
        }

        // the name is the prefix, a slash and the name, each ending at its first zero or at its field's end
        char name[257];
        usz prefixLength = SDL_strnlen((const char *)header + 345, 155);
        usz nameLength = SDL_strnlen((const char *)header, 100);
        usz length = 0;

        if (prefixLength > 0)
        {
            SDL_memcpy(name, header + 345, prefixLength);
            name[prefixLength] = '/';
            length = prefixLength + 1;
        }

        SDL_memcpy(name + length, header, nameLength);
        name[length + nameLength] = '\0';

        // files and folders are kept; links and other kinds are left out
        char kind = (char)header[156];
        bool kept = kind == '0' || kind == '\0' || kind == '5';

        if (kept && !CompressTarAddFile(retFiles, name, (SHUSliceView){bytes + offset + COMPRESS_TAR_BLOCK, kind == '5' ? 0 : (usz)size}, kind != '5' && (CompressTarReadOctal(header + 100, 8) & 0100u) != 0))
        {
            *retError = CompressFail("out of memory");
            ECSValue_SetNil(retFiles);
            return;
        }

        offset += COMPRESS_TAR_BLOCK + blocks * COMPRESS_TAR_BLOCK;
    }
}

#pragma endregion tar

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    COMPRESS.plugin = plugin;

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"gzip.pack", (ECSFunction)CompressGzipPack, "void(buffer data, int level, out value<string?> packed, out string error)",
         "Pack data with gzip, which zlib makes\n"
         "@param data The data\n"
         "@param level How hard to pack: 1 to 9, higher being smaller and slower; 0 for the default\n"
         "@param packed The packed data; nothing if it cannot be packed\n"
         "@param error Why the data cannot be packed; nothing if it can"},
        {"gzip.unpack", (ECSFunction)CompressGzipUnpack, "void(buffer data, out value<string?> unpacked, out string error)",
         "Unpack data that gzip packed\n"
         "@param data The packed data\n"
         "@param unpacked The unpacked data; nothing if it cannot be unpacked\n"
         "@param error Why the data cannot be unpacked, such as when it is damaged or ends too early; nothing if it can"},
        {"zlib.pack", (ECSFunction)CompressZlibPack, "void(buffer data, int level, out value<string?> packed, out string error)",
         "Pack data with zlib's format: deflate with a small header and a checksum\n"
         "@param data The data\n"
         "@param level How hard to pack: 1 to 9, higher being smaller and slower; 0 for the default\n"
         "@param packed The packed data; nothing if it cannot be packed\n"
         "@param error Why the data cannot be packed; nothing if it can"},
        {"zlib.unpack", (ECSFunction)CompressZlibUnpack, "void(buffer data, out value<string?> unpacked, out string error)",
         "Unpack data in zlib's format\n"
         "@param data The packed data\n"
         "@param unpacked The unpacked data; nothing if it cannot be unpacked\n"
         "@param error Why the data cannot be unpacked, such as when it is damaged or ends too early; nothing if it can"},
        {"deflate.pack", (ECSFunction)CompressDeflatePack, "void(buffer data, int level, out value<string?> packed, out string error)",
         "Pack data with deflate, without a header or a checksum\n"
         "@param data The data\n"
         "@param level How hard to pack: 1 to 9, higher being smaller and slower; 0 for the default\n"
         "@param packed The packed data; nothing if it cannot be packed\n"
         "@param error Why the data cannot be packed; nothing if it can"},
        {"deflate.unpack", (ECSFunction)CompressDeflateUnpack, "void(buffer data, out value<string?> unpacked, out string error)",
         "Unpack data that deflate packed, without a header or a checksum\n"
         "@param data The packed data\n"
         "@param unpacked The unpacked data; nothing if it cannot be unpacked\n"
         "@param error Why the data cannot be unpacked, such as when it is damaged or ends too early; nothing if it can"},
        {"zstd.pack", (ECSFunction)CompressZstdPack, "void(buffer data, int level, out value<string?> packed, out string error)",
         "Pack data with zstd\n"
         "@param data The data\n"
         "@param level How hard to pack: -7 to 22, higher being smaller and slower; 0 for the default\n"
         "@param packed The packed data; nothing if it cannot be packed\n"
         "@param error Why the data cannot be packed; nothing if it can"},
        {"zstd.unpack", (ECSFunction)CompressZstdUnpack, "void(buffer data, out value<string?> unpacked, out string error)",
         "Unpack data that zstd packed, which may hold many frames, one after another\n"
         "@param data The packed data\n"
         "@param unpacked The unpacked data; nothing if it cannot be unpacked\n"
         "@param error Why the data cannot be unpacked, such as when it is damaged or ends too early; nothing if it can"},
        {"unpack", (ECSFunction)CompressUnpackAny, "void(buffer data, out value<string?> unpacked, out string error)",
         "Unpack data that gzip, zlib or zstd packed, telling the format by the data's first bytes\n"
         "@param data The packed data; deflate's has no header, so deflate.unpack unpacks it\n"
         "@param unpacked The unpacked data; nothing if it cannot be unpacked\n"
         "@param error Why the data cannot be unpacked, such as an unknown format; nothing if it can"},
        {"tar.pack", (ECSFunction)CompressTarPack, "void(value<compress.TarFile[]> files, out value<string?> archive, out string error)",
         "Make a tar archive of files, in the ustar format, with the current time; gzip.pack packs it into a .tar.gz\n"
         "@param files Each file's name in the archive, what it holds, and whether it is executable; a name that ends with / is a folder\n"
         "@param archive The archive's bytes; nothing if it cannot be made\n"
         "@param error Why the archive cannot be made, such as a name too long for ustar; nothing if it can"},
        {"tar.unpack", (ECSFunction)CompressTarUnpack, "void(buffer data, out value<compress.TarFile[]?> files, out string error)",
         "Read the files and folders of a tar archive, in the archive's order; links and other kinds of entry are left out\n"
         "@param data The archive's bytes; gzip.unpack unpacks a .tar.gz first\n"
         "@param files Each file's name, what it holds, and whether it is executable; nothing if the archive cannot be read\n"
         "@param error Why the archive cannot be read, such as when it is damaged; nothing if it can"},
        {"zip.list", (ECSFunction)CompressZipList, "void(string path, out value<compress.ZipEntry[]?> entries, out string error)",
         "List the files and folders of a zip archive, in the archive's order\n"
         "@param path The archive's path; a relative path starts at the executable's folder\n"
         "@param entries Each entry's name, size and packed size; nothing if the archive cannot be read\n"
         "@param error Why the archive cannot be read; nothing if it can"},
        {"zip.read", (ECSFunction)CompressZipRead, "void(string path, string name, out value<string?> data, out string error)",
         "Read a file of a zip archive, unpacked, and check it against its checksum\n"
         "@param path The archive's path; a relative path starts at the executable's folder\n"
         "@param name The file's name in the archive, as zip.list gives it\n"
         "@param data What the file holds; nothing if it cannot be read\n"
         "@param error Why the file cannot be read, such as when the archive has no such file; nothing if it can"},
        {"zip.extract", (ECSFunction)CompressZipExtract, "bool(string path, string folder, out string error)",
         "Extract every file and folder of a zip archive into a folder, replacing files that are there\n"
         "@param path The archive's path; a relative path starts at the executable's folder\n"
         "@param folder The folder, which is made if it is missing; a relative path starts at the executable's folder\n"
         "@param error Why it stopped, such as an entry whose name would go outside the folder; nothing if every entry was extracted\n"
         "@return False if an entry cannot be extracted; the entries before it stay extracted"},
        {"zip.write", (ECSFunction)CompressZipWrite, "bool(string path, value<compress.ZipFile[]> files, int level, out string error)",
         "Write a zip archive of files, packed with deflate, replacing a file that is at the path\n"
         "@param path The archive's path; a relative path starts at the executable's folder\n"
         "@param files Each file's name in the archive and what it holds; a name that ends with / is a folder\n"
         "@param level How hard to pack: 1 to 9, higher being smaller and slower; 0 for the default\n"
         "@param error Why the archive cannot be written; nothing if it can\n"
         "@return False if the archive cannot be written; then no archive is left at the path"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
