// The fs standard plugin: what Lua's io and os libraries cannot do with files and folders (DESIGN 10).
// It lists folders, tells what a path is, makes folders, copies files and finds the user's folders, with SDL's filesystem functions.
// It watches folders for changes with inotify on Linux and ReadDirectoryChangesW on Windows, which SDL does not offer, and elsewhere by reading the folders again and again.
// Lua's os.rename and os.remove move and remove files and folders, so fs does not.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#ifdef __linux__
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

/// @brief How often, in seconds, the changes of watched folders are read: where there is no inotify or ReadDirectoryChangesW, by reading the folders again.
#if defined(__linux__) || defined(_WIN32)
#define FS_READ_INTERVAL 0.1
#else
#define FS_READ_INTERVAL 0.5
#endif

/// @brief How many reads of changes one call makes at most; the rest waits for the next call.
#define FS_WATCH_READS 8

/// @brief The bytes of changes that one read of a watched folder gives at most on Windows; more are lost, as when it overflows.
#define FS_WATCH_BUFFER 65536

/// @brief The function that hears a change in a watched folder: void(string folder, string name, string change). It is a kept callback (OpenECS DESIGN 10.4).
typedef void (*FsChangedFunction)(const char *folder, const char *name, const char *change);

/// @brief An entry of a watched folder as it was last read, where there is no inotify: a change of its size or time is a change of the file.
typedef struct FsEntry
{
    char *name;
    SDL_Time modified;
    Uint64 size;
} FsEntry;

/// @brief A watched folder. Its handle is fs.watch.
typedef struct FsWatch
{
    char *folder;   // as it was given, ending with a separator
    int descriptor; // inotify's watch, which watches of the same folder share, or -1
    FsChangedFunction Changed;
    bool stopped;     // stopped while changes were being told; freed after them
    char *fullPath;   // the folder's full path, ending with a separator, which is read again where there is no inotify
    FsEntry *entries; // stb_ds array of its entries as they were last read, sorted by name, where there is no inotify
#ifdef _WIN32
    HANDLE directory;      // the folder, opened for ReadDirectoryChangesW, or nullptr
    OVERLAPPED overlapped; // the read of changes that waits for the folder to change
    DWORD *buffer;         // what the read gives: FILE_NOTIFY_INFORMATION records, which start on DWORDs
    bool reading;          // a read waits; false after a read failed
#endif
} FsWatch;

static struct
{
    ECSPlugin plugin;
    int inotify;       // -1 until the first watch
    FsWatch **watches; // stb_ds array
    ECSTimer timer;    // reads the changes while a folder is watched
    bool telling;      // the changes are being told, so stopped watches wait to be freed
} FS = {.inotify = -1};

/// @brief The separator that a folder's path needs at its end: none if it has one, or /.
static const char *FsMissingSeparator(const char *folder)
{
    usz length = SDL_strlen(folder);
    return length > 0 && ECSPath_IsSeparator(folder[length - 1]) ? "" : "/";
}

static const char *FsTypeName(SDL_PathType type)
{
    switch (type)
    {
    case SDL_PATHTYPE_FILE:
        return "file";
    case SDL_PATHTYPE_DIRECTORY:
        return "folder";
    default:
        return "other";
    }
}

/// @brief Makes a value a table of what a path is: its type, its size in bytes, and when it was last changed, in seconds since 1970.
static SHUResult FsFillInfo(ECSValue *retInfo, const SDL_PathInfo *info)
{
    ECSValue *field = nullptr;
    ECSValue_SetTable(retInfo);
    SHU_ReturnResult(ECSValue_SetTableField(retInfo, "type", &field));
    SHU_ReturnResult(ECSValue_SetString(field, FsTypeName(info->type)));
    SHU_ReturnResult(ECSValue_SetTableField(retInfo, "size", &field));
    ECSValue_SetInteger(field, (i64)info->size);
    SHU_ReturnResult(ECSValue_SetTableField(retInfo, "modified", &field));
    ECSValue_SetNumber(field, (f64)info->modify_time / (f64)SDL_NS_PER_SECOND);
    return SHUResult_Ok;
}

static void FsInfo(const char *path, ECSValue *retInfo)
{
    char *fullPath = nullptr;
    SDL_PathInfo info = {0};

    if (ECSPath_GetFull(&fullPath, path) || !SDL_GetPathInfo(fullPath, &info) || FsFillInfo(retInfo, &info) != SHUResult_Ok)
    {
        ECSValue_SetNil(retInfo);
    }

    SDL_free(fullPath);
}

static int SDLCALL FsCompareNames(const void *a, const void *b)
{
    return SDL_strcmp(*(char *const *)a, *(char *const *)b);
}

/// @brief Makes a value the list of the entries of a folder, sorted by name: each a table of its name and its info.
static SHUResult FsFillEntries(const char *folder, char **names, int count, ECSValue *retEntries)
{
    ECSValue_SetTable(retEntries);
    SDL_qsort(names, (size_t)count, sizeof(char *), FsCompareNames);

    for (int i = 0; i < count; i++)
    {
        char *path = nullptr;
        SDL_PathInfo info = {0};
        ECSValue *item = nullptr;
        ECSValue *name = nullptr;

        if (SDL_asprintf(&path, "%s/%s", folder, names[i]) < 0)
        {
            return SHUResult_ErrAllocation;
        }

        bool found = SDL_GetPathInfo(path, &info);
        SDL_free(path);

        if (!found)
        {
            continue;
        }

        SHU_ReturnResult(ECSValue_AddListItem(retEntries, &item));
        SHU_ReturnResult(FsFillInfo(item, &info));
        SHU_ReturnResult(ECSValue_SetTableField(item, "name", &name));
        SHU_ReturnResult(ECSValue_SetString(name, names[i]));
    }

    return SHUResult_Ok;
}

static void FsList(const char *path, const char *pattern, ECSValue *retEntries)
{
    char *fullPath = nullptr;
    int count = 0;

    // an empty pattern matches every name; a star stops at a slash, so the list holds the folder's own entries, not those of its folders
    char **names = ECSPath_GetFull(&fullPath, path) ? nullptr : SDL_GlobDirectory(fullPath, pattern[0] == '\0' ? "*" : pattern, 0, &count);

    if (names == nullptr || FsFillEntries(fullPath, names, count, retEntries) != SHUResult_Ok)
    {
        ECSValue_SetNil(retEntries);
    }

    SDL_free(names);
    SDL_free(fullPath);
}

static bool FsMakeFolder(const char *path)
{
    char *fullPath = nullptr;
    bool made = ECSPath_GetFull(&fullPath, path) == SHUResult_Ok && SDL_CreateDirectory(fullPath);
    SDL_free(fullPath);
    return made;
}

static bool FsCopy(const char *from, const char *to)
{
    char *fullFrom = nullptr;
    char *fullTo = nullptr;
    bool copied = ECSPath_GetFull(&fullFrom, from) == SHUResult_Ok && ECSPath_GetFull(&fullTo, to) == SHUResult_Ok && SDL_CopyFile(fullFrom, fullTo);
    SDL_free(fullFrom);
    SDL_free(fullTo);
    return copied;
}

static const char *FsFolder(const char *name)
{
    static const struct
    {
        const char *name;
        SDL_Folder folder;
    } folders[] = {
        {"home", SDL_FOLDER_HOME},
        {"desktop", SDL_FOLDER_DESKTOP},
        {"documents", SDL_FOLDER_DOCUMENTS},
        {"downloads", SDL_FOLDER_DOWNLOADS},
        {"music", SDL_FOLDER_MUSIC},
        {"pictures", SDL_FOLDER_PICTURES},
        {"videos", SDL_FOLDER_VIDEOS},
        {"screenshots", SDL_FOLDER_SCREENSHOTS},
        {"templates", SDL_FOLDER_TEMPLATES},
    };

    if (SDL_strcmp(name, "executable") == 0)
    {
        return SDL_GetBasePath();
    }

    for (usz i = 0; i < SDL_arraysize(folders); i++)
    {
        if (SDL_strcmp(name, folders[i].name) == 0)
        {
            return SDL_GetUserFolder(folders[i].folder);
        }
    }

    return nullptr;
}

#pragma region Watches

static void FsFreeEntries(FsEntry *entries)
{
    for (usz i = 0; i < arrlenu(entries); i++)
    {
        SDL_free(entries[i].name);
    }

    arrfree(entries);
}

/// @brief Frees a watch, and stops inotify's watch if no other watch shares it; the timer stops when nothing is watched.
static void FsFreeWatch(FsWatch *watch)
{
#ifdef __linux__
    bool shared = false;

    for (usz i = 0; i < arrlenu(FS.watches); i++)
    {
        shared = shared || (FS.watches[i] != watch && !FS.watches[i]->stopped && FS.watches[i]->descriptor == watch->descriptor);
    }

    if (!shared && watch->descriptor >= 0)
    {
        inotify_rm_watch(FS.inotify, watch->descriptor);
    }
#elif defined(_WIN32)
    // the read that waits is cancelled, and ends, before its buffer is freed
    if (watch->directory != nullptr && watch->directory != INVALID_HANDLE_VALUE)
    {
        DWORD length = 0;

        if (watch->reading && CancelIoEx(watch->directory, &watch->overlapped))
        {
            GetOverlappedResult(watch->directory, &watch->overlapped, &length, TRUE);
        }

        CloseHandle(watch->directory);
    }

    SDL_free(watch->buffer);
#endif

    for (usz i = 0; i < arrlenu(FS.watches); i++)
    {
        if (FS.watches[i] == watch)
        {
            arrdelswap(FS.watches, i);
            break;
        }
    }

    ECSService_ReleaseCallback((ECSFunction)watch->Changed);
    FsFreeEntries(watch->entries);
    SDL_free(watch->fullPath);
    SDL_free(watch->folder);
    SDL_free(watch);

    if (arrlenu(FS.watches) == 0 && FS.timer != nullptr)
    {
        ECSTimer_Stop(&FS.timer);
    }
}

/// @brief Stops a watch: now, or after the changes being told.
static void FsStop(FsWatch *watch)
{
    if (FS.telling)
    {
        watch->stopped = true;
        return;
    }

    FsFreeWatch(watch);
}

/// @brief Frees the watches that were stopped while the changes were being told.
static void FsFreeStoppedWatches(void)
{
    for (usz i = arrlenu(FS.watches); i > 0; i--)
    {
        if (i <= arrlenu(FS.watches) && FS.watches[i - 1]->stopped)
        {
            FsFreeWatch(FS.watches[i - 1]);
        }
    }
}

#ifdef __linux__
/// @brief Starts watching a folder with inotify.
static bool FsStartWatching(FsWatch *watch, const char *fullPath)
{
    if (FS.inotify < 0)
    {
        FS.inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    }

    // IN_ONLYDIR: a file is not a folder to watch
    watch->descriptor = FS.inotify < 0 ? -1 : inotify_add_watch(FS.inotify, fullPath, IN_ONLYDIR | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
    return watch->descriptor >= 0;
}

/// @brief Reads the changes of the watched folders, and tells each watch of its folder's.
static void FsReadChanges(void *data)
{
    (void)data;

    alignas(struct inotify_event) char buffer[4096];
    ssize_t length = 0;
    FS.telling = true;

    // a folder that changes all the time cannot hold the main thread: the rest waits for the next call
    for (usz reads = 0; reads < FS_WATCH_READS && (length = read(FS.inotify, buffer, sizeof(buffer))) > 0; reads++)
    {
        for (char *at = buffer; at < buffer + length;)
        {
            const struct inotify_event *event = (const struct inotify_event *)(void *)at;
            const char *change = event->mask & (IN_CREATE | IN_MOVED_TO) ? "added" : event->mask & (IN_DELETE | IN_MOVED_FROM)            ? "removed"
                                                                                 : event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB) ? "changed"
                                                                                                                                          : nullptr;

            // the watches may change while they are told, so they are counted again each time
            for (usz i = 0; change != nullptr && event->len > 0 && i < arrlenu(FS.watches); i++)
            {
                FsWatch *watch = FS.watches[i];

                if (!watch->stopped && watch->descriptor == event->wd)
                {
                    watch->Changed(watch->folder, event->name, change);
                }
            }

            at += sizeof(struct inotify_event) + event->len;
        }
    }

    FS.telling = false;
    FsFreeStoppedWatches();
}

#elif defined(_WIN32)
/// @brief Starts a watch's next read of changes, which ends when its folder changes.
static bool FsStartRead(FsWatch *watch)
{
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES;
    watch->reading = ReadDirectoryChangesW(watch->directory, watch->buffer, FS_WATCH_BUFFER, FALSE, filter, nullptr, &watch->overlapped, nullptr) != 0;
    return watch->reading;
}

/// @brief Starts watching a folder with ReadDirectoryChangesW.
static bool FsStartWatching(FsWatch *watch, const char *fullPath)
{
    int wideLength = MultiByteToWideChar(CP_UTF8, 0, fullPath, -1, nullptr, 0);
    WCHAR *wide = wideLength <= 0 ? nullptr : SDL_malloc((usz)wideLength * sizeof(WCHAR));

    if (wide == nullptr || MultiByteToWideChar(CP_UTF8, 0, fullPath, -1, wide, wideLength) <= 0)
    {
        SDL_free(wide);
        return false;
    }

    // a file is opened too, but its read of changes fails
    watch->directory = CreateFileW(wide, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    watch->buffer = SDL_malloc(FS_WATCH_BUFFER);
    SDL_free(wide);
    return watch->directory != INVALID_HANDLE_VALUE && watch->buffer != nullptr && FsStartRead(watch);
}

/// @brief Tells a watch the changes that its ended read gave.
static void FsTellChanges(FsWatch *watch, DWORD length)
{
    for (const char *at = (const char *)watch->buffer; length > 0 && !watch->stopped;)
    {
        const FILE_NOTIFY_INFORMATION *info = (const FILE_NOTIFY_INFORMATION *)(const void *)at;
        DWORD action = info->Action;
        const char *change = action == FILE_ACTION_ADDED || action == FILE_ACTION_RENAMED_NEW_NAME     ? "added"
                             : action == FILE_ACTION_REMOVED || action == FILE_ACTION_RENAMED_OLD_NAME ? "removed"
                             : action == FILE_ACTION_MODIFIED                                          ? "changed"
                                                                                                       : nullptr;

        // the name is UTF-16, without a zero at its end
        char name[MAX_PATH * 4];
        int size = WideCharToMultiByte(CP_UTF8, 0, info->FileName, (int)(info->FileNameLength / sizeof(WCHAR)), name, (int)sizeof(name) - 1, nullptr, nullptr);

        if (change != nullptr && size > 0)
        {
            name[size] = '\0';
            watch->Changed(watch->folder, name, change);
        }

        if (info->NextEntryOffset == 0)
        {
            break;
        }

        at += info->NextEntryOffset;
    }
}

/// @brief Tells each watch the changes of its folder that its read gave, and starts its next read. A read that has not ended waits for the next call.
static void FsReadChanges(void *data)
{
    (void)data;
    FS.telling = true;

    // the watches may change while they are told, so they are counted again each time
    for (usz i = 0; i < arrlenu(FS.watches); i++)
    {
        FsWatch *watch = FS.watches[i];
        DWORD length = 0;

        if (watch->stopped || !watch->reading)
        {
            continue;
        }

        // a read that overflowed lost its changes, and gives none; the next read goes on
        if (!GetOverlappedResult(watch->directory, &watch->overlapped, &length, FALSE))
        {
            DWORD error = GetLastError();

            if (error == ERROR_IO_INCOMPLETE)
            {
                continue;
            }

            length = 0;

            if (error != ERROR_NOTIFY_ENUM_DIR)
            {
                watch->reading = false;
                ECS_LogWarn(FS.plugin, "Cannot watch the folder '%s' any more: error %lu", watch->folder, (unsigned long)error);
                continue;
            }
        }

        FsTellChanges(watch, length);

        if (!watch->stopped && !FsStartRead(watch))
        {
            ECS_LogWarn(FS.plugin, "Cannot watch the folder '%s' any more: error %lu", watch->folder, (unsigned long)GetLastError());
        }
    }

    FS.telling = false;
    FsFreeStoppedWatches();
}

#else
/// @brief What FsReadFolder gathers.
typedef struct FsFolderRead
{
    const char *fullPath;
    FsEntry *entries; // stb_ds array
    bool failed;
} FsFolderRead;

static SDL_EnumerationResult SDLCALL FsAddEntry(void *data, const char *folder, const char *name)
{
    (void)folder;
    FsFolderRead *read = data;
    char *path = nullptr;
    SDL_PathInfo info = {0};

    if (SDL_asprintf(&path, "%s%s", read->fullPath, name) < 0)
    {
        read->failed = true;
        return SDL_ENUM_FAILURE;
    }

    bool found = SDL_GetPathInfo(path, &info);
    SDL_free(path);
    char *copy = found ? SDL_strdup(name) : nullptr;

    // an entry removed while the folder is read is left out
    if (copy != nullptr)
    {
        arrput(read->entries, ((FsEntry){.name = copy, .modified = info.modify_time, .size = info.size}));
    }

    return SDL_ENUM_CONTINUE;
}

static int SDLCALL FsCompareEntries(const void *a, const void *b)
{
    return SDL_strcmp(((const FsEntry *)a)->name, ((const FsEntry *)b)->name);
}

/// @brief Reads a folder's entries, sorted by name, into an stb_ds array.
/// @return Whether it could be read; if not, the entries are left empty.
static bool FsReadFolder(const char *fullPath, FsEntry **retEntries)
{
    FsFolderRead read = {fullPath, nullptr, false};

    if (!SDL_EnumerateDirectory(fullPath, FsAddEntry, &read) || read.failed)
    {
        FsFreeEntries(read.entries);
        *retEntries = nullptr;
        return false;
    }

    SDL_qsort(read.entries, arrlenu(read.entries), sizeof(FsEntry), FsCompareEntries);
    *retEntries = read.entries;
    return true;
}

/// @brief Starts watching a folder by reading it now, so the next read finds what changed after the watch began.
static bool FsStartWatching(FsWatch *watch, const char *fullPath)
{
    SDL_PathInfo info = {0};
    return SDL_GetPathInfo(fullPath, &info) && info.type == SDL_PATHTYPE_DIRECTORY && SDL_asprintf(&watch->fullPath, "%s%s", fullPath, FsMissingSeparator(fullPath)) >= 0 &&
           FsReadFolder(watch->fullPath, &watch->entries);
}

/// @brief Tells a watch of the entries added, removed and changed between two reads of its folder, which are sorted by name, so they are walked together.
static void FsTellChanges(FsWatch *watch, const FsEntry *old, const FsEntry *entries)
{
    usz oldCount = arrlenu(old);
    usz count = arrlenu(entries);

    for (usz oldAt = 0, at = 0; oldAt < oldCount || at < count;)
    {
        int order = oldAt == oldCount ? 1 : at == count ? -1
                                                        : SDL_strcmp(old[oldAt].name, entries[at].name);
        const char *name = order < 0 ? old[oldAt].name : entries[at].name;
        const char *change = order < 0 ? "removed" : order > 0                                                                        ? "added"
                                                 : old[oldAt].modified != entries[at].modified || old[oldAt].size != entries[at].size ? "changed"
                                                                                                                                      : nullptr;

        if (change != nullptr && !watch->stopped)
        {
            watch->Changed(watch->folder, name, change);
        }

        oldAt += order <= 0 ? 1 : 0;
        at += order >= 0 ? 1 : 0;
    }
}

/// @brief Reads each watched folder again, and tells its watch of the entries added, removed and changed since the last read.
static void FsReadChanges(void *data)
{
    (void)data;
    FS.telling = true;

    // the watches may change while they are told, so they are counted again each time
    for (usz i = 0; i < arrlenu(FS.watches); i++)
    {
        FsWatch *watch = FS.watches[i];
        FsEntry *entries = nullptr;

        if (watch->stopped || !FsReadFolder(watch->fullPath, &entries))
        {
            continue;
        }

        FsEntry *old = watch->entries;
        watch->entries = entries;
        FsTellChanges(watch, old, entries);
        FsFreeEntries(old);
    }

    FS.telling = false;
    FsFreeStoppedWatches();
}
#endif

static void *FsWatchFolder(const char *folder, FsChangedFunction Changed)
{
    FsWatch *watch = Changed == nullptr ? nullptr : SDL_calloc(1, sizeof(FsWatch));

    if (watch == nullptr)
    {
        ECS_LogWarn(FS.plugin, "Cannot watch the folder '%s'", folder);

        if (Changed != nullptr)
        {
            ECSService_ReleaseCallback((ECSFunction)Changed);
        }

        return nullptr;
    }

    // the watch is added first, so a watch that fails is freed as any other, which stops inotify's watch and the timer when nothing else uses them
    watch->descriptor = -1;
    watch->Changed = Changed;
    arrput(FS.watches, watch);
    char *fullPath = nullptr;
    bool watching = ECSPath_GetFull(&fullPath, folder) == SHUResult_Ok && SDL_asprintf(&watch->folder, "%s%s", folder, FsMissingSeparator(folder)) >= 0 && FsStartWatching(watch, fullPath) &&
                    (FS.timer != nullptr || ECSTimer_Start(FS.plugin, &FS.timer, FS_READ_INTERVAL, true, FsReadChanges, nullptr) == SHUResult_Ok);
    SDL_free(fullPath);

    if (!watching)
    {
        ECS_LogWarn(FS.plugin, "Cannot watch the folder '%s'", folder);
        FsStop(watch);
        return nullptr;
    }

    return watch;
}

/// @brief The Destroy function of the handle type fs.watch.
static void FsWatchDestroy(void *object)
{
    FsStop(object);
}

/// @brief Stops a watch now, instead of when no one uses its handle.
static void FsUnwatch(void *watch)
{
    ECSHandle_Free(FS.plugin, "watch", watch);
}

#pragma endregion Watches

/// @brief Stops every watch; the Destroy function of the plugin.
static void FsFree(void *data)
{
    (void)data;

    while (arrlenu(FS.watches) > 0)
    {
        FsFreeWatch(FS.watches[0]);
    }

#ifdef __linux__
    if (FS.inotify >= 0)
    {
        close(FS.inotify);
    }
#endif

    arrfree(FS.watches);
    SDL_zero(FS);
    FS.inotify = -1;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    FS.plugin = plugin;
    const ECSPluginDesc desc = {.Destroy = FsFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "watch", FsWatchDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"info", (ECSFunction)FsInfo, "void(string path, out value<fs.Info?> info)",
         "Tell what a path is: its type (file, folder or other), size and when it changed; nothing if it does not exist\n"
         "@param path The path; a relative path starts at the executable's folder\n"
         "@param info Its type, \"file\", \"folder\" or \"other\", its size in bytes, and modified, when it last changed, in seconds since 1970; nothing if the path does not exist"},
        {"list", (ECSFunction)FsList, "void(string folder, string pattern, out value<fs.Entry[]?> entries)",
         "List the entries of a folder whose names match a pattern, such as *.lua, by name; an empty pattern matches every name\n"
         "@param folder The folder's path; a relative path starts at the executable's folder\n"
         "@param pattern The names to list: * stands for any characters but /, and ? for one; empty for every name\n"
         "@param entries The entries, sorted by name, each with its name and the fields of info; the entries of the folder's folders are not listed; nothing if the folder cannot be read"},
        {"makeFolder", (ECSFunction)FsMakeFolder, "bool(string path)",
         "Make a folder, and the folders above it that are missing\n"
         "@param path The folder's path; a relative path starts at the executable's folder\n"
         "@return False if it fails"},
        {"copy", (ECSFunction)FsCopy, "bool(string from, string to)",
         "Copy a file, replacing what is at the new path\n"
         "@param from The file's path; a relative path starts at the executable's folder\n"
         "@param to The new path; a relative path starts at the executable's folder\n"
         "@return False if it fails"},
        {"watch", (ECSFunction)FsWatchFolder, "handle<fs.watch>(string folder, keep fn<void(string folder, string name, string change)> changed)",
         "Watch a folder: changed gets each entry that is added, removed or changed in it, with the change\n"
         "@param folder The folder's path; a relative path starts at the executable's folder\n"
         "@param changed Called on the main thread for each change: its folder is the folder as given, ending with /, its name the entry's name, and its change \"added\", \"removed\" or \"changed\"; not called after the watch stops\n"
         "@return The watch, or nothing if the folder cannot be watched, or it is a file"},
        {"unwatch", (ECSFunction)FsUnwatch, "void(handle<fs.watch> watch)",
         "Stop watching a folder now, instead of when no one uses its handle\n"
         "@param watch The watch, as watch gives it; its handle must not be used after"},
        {"folder", (ECSFunction)FsFolder, "string(string name)",
         "Give a folder by name: executable, home, desktop, documents, downloads, music, pictures, videos, screenshots or templates\n"
         "@param name The folder's name: executable, the executable's folder, or one of the user's folders: home, desktop, documents, downloads, music, pictures, videos, screenshots or templates\n"
         "@return The folder's path, ending with /; nothing if the system does not have it or the name is not one of these"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a handle, such as handle:stop(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"watch", "stop", "unwatch"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function));
    }

    return SHUResult_Ok;
}
