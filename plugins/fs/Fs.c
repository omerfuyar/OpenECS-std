// The fs standard plugin: what Lua's io and os libraries cannot do with files and folders (DESIGN 10).
// It lists folders, tells what a path is, makes folders, copies files and finds the user's folders, with SDL's filesystem functions.
// It watches folders for changes with the system's own way, inotify on Linux, which SDL does not offer.
// Lua's os.rename and os.remove move and remove files and folders, so fs does not.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#ifdef __linux__
#include <sys/inotify.h>
#include <unistd.h>
#endif

#pragma region Source Only

/// @brief How often, in seconds, the changes of watched folders are read.
#define FS_WATCH_INTERVAL 0.1

/// @brief How many reads of changes one call makes at most; the rest waits for the next call.
#define FS_WATCH_READS 8

/// @brief The function that hears a change in a watched folder: void(string folder, string name, string change). It is a kept callback (OpenECS DESIGN 10.4).
typedef void (*FsChangedFunction)(const char *folder, const char *name, const char *change);

/// @brief A watched folder. Its handle is fs.watch.
typedef struct FsWatch
{
    char *folder;  // as it was given, ending with a separator
    int descriptor; // inotify's watch, which watches of the same folder share
    FsChangedFunction Changed;
    bool stopped; // stopped while changes were being told; freed after them
} FsWatch;

static struct
{
    ECSPlugin plugin;
    int inotify;      // -1 until the first watch
    FsWatch **watches;
    usz watchCount;
    ECSTimer timer;   // reads the changes while a folder is watched
    bool telling;     // the changes are being told, so stopped watches wait to be freed
} FS = {.inotify = -1};

/// @brief A full path: a relative path starts at the executable's folder, as the paths of the other standard plugins do. Free it with SDL_free.
static char *FsFull(const char *path)
{
    char *full = nullptr;
    return SDL_asprintf(&full, "%s%s", ECSPath_IsAbsolute(path) ? "" : SDL_GetBasePath(), path) < 0 ? nullptr : full;
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

/// @brief Sets a table's fields to what a path is: its type, its size in bytes, and when it was last changed, in seconds since 1970.
static SHUResult FsSetInfo(ECSValue *table, const SDL_PathInfo *info)
{
    ECSValue *field = nullptr;
    ECSValue_SetTable(table);
    SHU_ReturnResult(ECSValue_TableSetField(table, "type", &field));
    SHU_ReturnResult(ECSValue_SetString(field, FsTypeName(info->type)));
    SHU_ReturnResult(ECSValue_TableSetField(table, "size", &field));
    ECSValue_SetInteger(field, (i64)info->size);
    SHU_ReturnResult(ECSValue_TableSetField(table, "modified", &field));
    ECSValue_SetNumber(field, (f64)info->modify_time / (f64)SDL_NS_PER_SECOND);
    return SHUResult_Ok;
}

static void FsInfo(const char *path, ECSValue *retInfo)
{
    char *full = FsFull(path);
    SDL_PathInfo info = {0};

    if (full == nullptr || !SDL_GetPathInfo(full, &info) || FsSetInfo(retInfo, &info) != SHUResult_Ok)
    {
        ECSValue_SetNil(retInfo);
    }

    SDL_free(full);
}

static int SDLCALL FsCompareNames(const void *a, const void *b)
{
    return SDL_strcmp(*(char *const *)a, *(char *const *)b);
}

/// @brief Fills a list with the entries of a folder that a pattern matches, by name: each a table of its name and its info.
static SHUResult FsFillList(const char *folder, char **names, int count, ECSValue *list)
{
    ECSValue_SetTable(list);
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

        SHU_ReturnResult(ECSValue_ListAddItem(list, &item));
        SHU_ReturnResult(FsSetInfo(item, &info));
        SHU_ReturnResult(ECSValue_TableSetField(item, "name", &name));
        SHU_ReturnResult(ECSValue_SetString(name, names[i]));
    }

    return SHUResult_Ok;
}

static void FsList(const char *path, const char *pattern, ECSValue *retEntries)
{
    char *full = FsFull(path);
    int count = 0;

    // an empty pattern matches every name; a star stops at a slash, so the list holds the folder's own entries, not those of its folders
    char **names = full == nullptr ? nullptr : SDL_GlobDirectory(full, pattern[0] == '\0' ? "*" : pattern, 0, &count);

    if (names == nullptr || FsFillList(full, names, count, retEntries) != SHUResult_Ok)
    {
        ECSValue_SetNil(retEntries);
    }

    SDL_free(names);
    SDL_free(full);
}

static bool FsMakeFolder(const char *path)
{
    char *full = FsFull(path);
    bool made = full != nullptr && SDL_CreateDirectory(full);
    SDL_free(full);
    return made;
}

static bool FsCopy(const char *from, const char *to)
{
    char *fullFrom = FsFull(from);
    char *fullTo = FsFull(to);
    bool copied = fullFrom != nullptr && fullTo != nullptr && SDL_CopyFile(fullFrom, fullTo);
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
        {"home", SDL_FOLDER_HOME},           {"desktop", SDL_FOLDER_DESKTOP}, {"documents", SDL_FOLDER_DOCUMENTS}, {"downloads", SDL_FOLDER_DOWNLOADS},
        {"music", SDL_FOLDER_MUSIC},         {"pictures", SDL_FOLDER_PICTURES}, {"videos", SDL_FOLDER_VIDEOS},     {"screenshots", SDL_FOLDER_SCREENSHOTS},
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

/// @brief Frees a watch, and stops inotify's watch if no other watch shares it.
static void FsFreeWatch(FsWatch *watch)
{
    bool shared = false;

    for (usz i = 0; i < FS.watchCount; i++)
    {
        shared = shared || (FS.watches[i] != watch && !FS.watches[i]->stopped && FS.watches[i]->descriptor == watch->descriptor);
    }

#ifdef __linux__
    if (!shared && watch->descriptor >= 0)
    {
        inotify_rm_watch(FS.inotify, watch->descriptor);
    }
#endif

    for (usz i = 0; i < FS.watchCount; i++)
    {
        if (FS.watches[i] == watch)
        {
            FS.watches[i] = FS.watches[--FS.watchCount];
            break;
        }
    }

    ECSService_ReleaseCallback((ECSFunction)watch->Changed);
    SDL_free(watch->folder);
    SDL_free(watch);

    if (FS.watchCount == 0 && FS.timer != nullptr)
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

#ifdef __linux__
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
            const char *change = event->mask & (IN_CREATE | IN_MOVED_TO) ? "added" : event->mask & (IN_DELETE | IN_MOVED_FROM) ? "removed" : event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB) ? "changed" : nullptr;

            // the watches may change while they are told, so they are counted again each time
            for (usz i = 0; change != nullptr && event->len > 0 && i < FS.watchCount; i++)
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

    for (usz i = FS.watchCount; i > 0; i--)
    {
        if (i <= FS.watchCount && FS.watches[i - 1]->stopped)
        {
            FsFreeWatch(FS.watches[i - 1]);
        }
    }
}
#endif

static FsWatch *FsWatchFolder(const char *folder, FsChangedFunction Changed)
{
#ifdef __linux__
    char *full = FsFull(folder);
    FsWatch *watch = full == nullptr || Changed == nullptr ? nullptr : SDL_calloc(1, sizeof(FsWatch));
    FsWatch **watches = watch == nullptr ? nullptr : SDL_realloc(FS.watches, (FS.watchCount + 1) * sizeof(FsWatch *));

    if (FS.inotify < 0 && watches != nullptr)
    {
        FS.inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    }

    if (watches != nullptr)
    {
        FS.watches = watches;
    }

    // IN_ONLYDIR: a file is not a folder to watch
    int descriptor = watches == nullptr || FS.inotify < 0 ? -1 : inotify_add_watch(FS.inotify, full, IN_ONLYDIR | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
    usz length = SDL_strlen(folder);

    if (descriptor < 0 || (FS.timer == nullptr && ECSTimer_Start(FS.plugin, &FS.timer, FS_WATCH_INTERVAL, true, FsReadChanges, nullptr) != SHUResult_Ok) ||
        SDL_asprintf(&watch->folder, "%s%s", folder, length > 0 && ECSPath_IsSeparator(folder[length - 1]) ? "" : "/") < 0)
    {
        // inotify's watch stops unless another watch shares it, and the timer stops if nothing is watched
        bool shared = false;

        for (usz i = 0; descriptor >= 0 && i < FS.watchCount; i++)
        {
            shared = shared || FS.watches[i]->descriptor == descriptor;
        }

        if (descriptor >= 0 && !shared)
        {
            inotify_rm_watch(FS.inotify, descriptor);
        }

        if (FS.watchCount == 0 && FS.timer != nullptr)
        {
            ECSTimer_Stop(&FS.timer);
        }

        ECS_LogWarn(FS.plugin, "Cannot watch the folder '%s'", folder);
        SDL_free(watch);
        SDL_free(full);
        ECSService_ReleaseCallback((ECSFunction)Changed);
        return nullptr;
    }

    SDL_free(full);
    watch->descriptor = descriptor;
    watch->Changed = Changed;
    FS.watches[FS.watchCount++] = watch;
    return watch;
#else
    ECS_LogWarn(FS.plugin, "Watching folders works on Linux only, so '%s' is not watched", folder);
    ECSService_ReleaseCallback((ECSFunction)Changed);
    return nullptr;
#endif
}

/// @brief The destructor of a watch's handle.
static void FsWatchDestroy(void *object)
{
    FsStop(object);
}

/// @brief Stops a watch now, instead of when no one uses its handle.
static void FsUnwatch(FsWatch *watch)
{
    ECSHandle_Forget(watch);
    FsStop(watch);
}

/// @brief Stops every watch; the Destroy function of the plugin.
static void FsQuit(void *data)
{
    (void)data;

    while (FS.watchCount > 0)
    {
        FsFreeWatch(FS.watches[0]);
    }

#ifdef __linux__
    if (FS.inotify >= 0)
    {
        close(FS.inotify);
    }
#endif

    SDL_free(FS.watches);
    SDL_zero(FS);
    FS.inotify = -1;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    FS.plugin = plugin;
    const ECSPluginDesc desc = {.Destroy = FsQuit};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "watch", FsWatchDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"info", (ECSFunction)FsInfo, "void(string path, out value<fs.Info?> info)", "Tell what a path is: its type (file, folder or other), size and when it changed; nothing if it does not exist"},
        {"list", (ECSFunction)FsList, "void(string folder, string pattern, out value<fs.Entry[]?> entries)", "List the entries of a folder whose names match a pattern, such as *.lua, by name; an empty pattern matches every name"},
        {"makeFolder", (ECSFunction)FsMakeFolder, "bool(string path)", "Make a folder, and the folders above it that are missing"},
        {"copy", (ECSFunction)FsCopy, "bool(string from, string to)", "Copy a file, replacing what is at the new path"},
        {"watch", (ECSFunction)FsWatchFolder, "handle<fs.watch>(string folder, keep fn<void(string folder, string name, string change)> changed)", "Watch a folder: changed gets each entry that is added, removed or changed in it, with the change"},
        {"unwatch", (ECSFunction)FsUnwatch, "void(handle<fs.watch> watch)", "Stop watching a folder now, instead of when no one uses its handle"},
        {"folder", (ECSFunction)FsFolder, "string(string name)", "Give a folder by name: executable, home, desktop, documents, downloads, music, pictures, videos, screenshots or templates"},
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
