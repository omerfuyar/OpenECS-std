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

#pragma region Source Only

/// @brief How often, in seconds, the changes of watched folders are read.
#define FS_WATCH_INTERVAL 0.1

/// @brief How many reads of changes one call makes at most; the rest waits for the next call.
#define FS_WATCH_READS 8

/// @brief The bytes of changes that one read of a watched folder gives at most on Windows; more are lost, as when it overflows.
#define FS_WATCH_BUFFER 65536

/// @brief How often, in seconds, a watched folder is read again where there is no inotify.
#define FS_POLL_INTERVAL 0.5

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
    char *folder;  // as it was given, ending with a separator
    int descriptor; // inotify's watch, which watches of the same folder share, or -1
    FsChangedFunction Changed;
    bool stopped; // stopped while changes were being told; freed after them
    char *full;   // the folder's full path, which is read again where there is no inotify
    FsEntry *entries; // its entries as they were last read, sorted by name, where there is no inotify
    usz entryCount;
#ifdef _WIN32
    HANDLE directory;       // the folder, opened for ReadDirectoryChangesW, or nullptr
    OVERLAPPED overlapped;  // the read of changes that waits for the folder to change
    DWORD *buffer;          // what the read gives: FILE_NOTIFY_INFORMATION records, which start on DWORDs
    bool reading;           // a read waits; false after a read failed
#endif
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

    for (usz i = 0; i < FS.watchCount; i++)
    {
        if (FS.watches[i] == watch)
        {
            FS.watches[i] = FS.watches[--FS.watchCount];
            break;
        }
    }

    ECSService_ReleaseCallback((ECSFunction)watch->Changed);

    for (usz i = 0; i < watch->entryCount; i++)
    {
        SDL_free(watch->entries[i].name);
    }

    SDL_free(watch->entries);
    SDL_free(watch->full);
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

#elif defined(_WIN32)
/// @brief Starts a watch's next read of changes, which ends when its folder changes.
static bool FsStartRead(FsWatch *watch)
{
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES;
    watch->reading = ReadDirectoryChangesW(watch->directory, watch->buffer, FS_WATCH_BUFFER, FALSE, filter, nullptr, &watch->overlapped, nullptr) != 0;
    return watch->reading;
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
    for (usz i = 0; i < FS.watchCount; i++)
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

    for (usz i = FS.watchCount; i > 0; i--)
    {
        if (i <= FS.watchCount && FS.watches[i - 1]->stopped)
        {
            FsFreeWatch(FS.watches[i - 1]);
        }
    }
}

#else
/// @brief What FsReadFolder gathers.
typedef struct FsEntries
{
    const char *folder;
    FsEntry *entries;
    usz count;
    bool failed;
} FsEntries;

static SDL_EnumerationResult SDLCALL FsAddEntry(void *data, const char *folder, const char *name)
{
    (void)folder;
    FsEntries *read = data;
    char *path = nullptr;
    SDL_PathInfo info = {0};
    FsEntry *entries = SDL_realloc(read->entries, (read->count + 1) * sizeof(FsEntry));

    if (entries == nullptr || SDL_asprintf(&path, "%s%s", read->folder, name) < 0)
    {
        read->entries = entries != nullptr ? entries : read->entries;
        read->failed = true;
        return SDL_ENUM_FAILURE;
    }

    read->entries = entries;
    bool found = SDL_GetPathInfo(path, &info);
    SDL_free(path);

    // an entry removed while the folder is read is left out
    if (found && (entries[read->count].name = SDL_strdup(name)) != nullptr)
    {
        entries[read->count].modified = info.modify_time;
        entries[read->count].size = info.size;
        read->count++;
    }

    return SDL_ENUM_CONTINUE;
}

static int SDLCALL FsCompareEntries(const void *a, const void *b)
{
    return SDL_strcmp(((const FsEntry *)a)->name, ((const FsEntry *)b)->name);
}

/// @brief Reads a folder's entries, sorted by name.
/// @return Whether it could be read; if not, the entries are left empty.
static bool FsReadFolder(const char *full, FsEntry **retEntries, usz *retCount)
{
    FsEntries read = {full, nullptr, 0, false};

    if (!SDL_EnumerateDirectory(full, FsAddEntry, &read) || read.failed)
    {
        for (usz i = 0; i < read.count; i++)
        {
            SDL_free(read.entries[i].name);
        }

        SDL_free(read.entries);
        *retEntries = nullptr;
        *retCount = 0;
        return false;
    }

    SDL_qsort(read.entries, read.count, sizeof(FsEntry), FsCompareEntries);
    *retEntries = read.entries;
    *retCount = read.count;
    return true;
}

/// @brief Reads each watched folder again, and tells its watch of the entries added, removed and changed since the last read.
static void FsReadChanges(void *data)
{
    (void)data;
    FS.telling = true;

    // the watches may change while they are told, so they are counted again each time
    for (usz i = 0; i < FS.watchCount; i++)
    {
        FsWatch *watch = FS.watches[i];
        FsEntry *entries = nullptr;
        usz count = 0;

        if (watch->stopped || !FsReadFolder(watch->full, &entries, &count))
        {
            continue;
        }

        // both lists are sorted by name, so they are walked together
        FsEntry *old = watch->entries;
        usz oldCount = watch->entryCount;
        watch->entries = entries;
        watch->entryCount = count;

        for (usz a = 0, b = 0; a < oldCount || b < count;)
        {
            int order = a == oldCount ? 1 : b == count ? -1 : SDL_strcmp(old[a].name, entries[b].name);
            const char *name = order < 0 ? old[a].name : entries[b].name;
            const char *change = order < 0 ? "removed" : order > 0 ? "added" : old[a].modified != entries[b].modified || old[a].size != entries[b].size ? "changed" : nullptr;

            if (change != nullptr && !watch->stopped)
            {
                watch->Changed(watch->folder, name, change);
            }

            a += order <= 0 ? 1 : 0;
            b += order >= 0 ? 1 : 0;
        }

        for (usz j = 0; j < oldCount; j++)
        {
            SDL_free(old[j].name);
        }

        SDL_free(old);
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
#elif defined(_WIN32)
    char *full = FsFull(folder);
    usz length = SDL_strlen(folder);
    int wideLength = full == nullptr ? 0 : MultiByteToWideChar(CP_UTF8, 0, full, -1, nullptr, 0);
    WCHAR *wide = wideLength <= 0 ? nullptr : SDL_malloc((usz)wideLength * sizeof(WCHAR));
    FsWatch *watch = wide == nullptr || Changed == nullptr ? nullptr : SDL_calloc(1, sizeof(FsWatch));
    FsWatch **watches = watch == nullptr ? nullptr : SDL_realloc(FS.watches, (FS.watchCount + 1) * sizeof(FsWatch *));
    FS.watches = watches != nullptr ? watches : FS.watches;

    if (watches != nullptr && MultiByteToWideChar(CP_UTF8, 0, full, -1, wide, wideLength) > 0)
    {
        // a file is opened too, but its read of changes fails
        watch->directory = CreateFileW(wide, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        watch->buffer = SDL_malloc(FS_WATCH_BUFFER);
    }

    if (watches == nullptr || watch->directory == nullptr || watch->directory == INVALID_HANDLE_VALUE || watch->buffer == nullptr || !FsStartRead(watch) ||
        SDL_asprintf(&watch->folder, "%s%s", folder, length > 0 && ECSPath_IsSeparator(folder[length - 1]) ? "" : "/") < 0 ||
        (FS.timer == nullptr && ECSTimer_Start(FS.plugin, &FS.timer, FS_WATCH_INTERVAL, true, FsReadChanges, nullptr) != SHUResult_Ok))
    {
        ECS_LogWarn(FS.plugin, "Cannot watch the folder '%s'", folder);

        // the watch is freed as one that was added, which stops the timer if nothing is watched
        if (watches != nullptr)
        {
            watch->Changed = Changed;
            FS.watches[FS.watchCount++] = watch;
            FsFreeWatch(watch);
        }
        else
        {
            SDL_free(watch);
            ECSService_ReleaseCallback((ECSFunction)Changed);
        }

        SDL_free(wide);
        SDL_free(full);
        return nullptr;
    }

    SDL_free(wide);
    SDL_free(full);
    watch->descriptor = -1;
    watch->Changed = Changed;
    FS.watches[FS.watchCount++] = watch;
    return watch;
#else
    // the folder is read now, so the next read finds what changed after the watch began
    char *full = FsFull(folder);
    usz length = SDL_strlen(folder);
    FsWatch *watch = full == nullptr || Changed == nullptr ? nullptr : SDL_calloc(1, sizeof(FsWatch));
    FsWatch **watches = watch == nullptr ? nullptr : SDL_realloc(FS.watches, (FS.watchCount + 1) * sizeof(FsWatch *));
    FS.watches = watches != nullptr ? watches : FS.watches;
    SDL_PathInfo info = {0};

    if (watches == nullptr || !SDL_GetPathInfo(full, &info) || info.type != SDL_PATHTYPE_DIRECTORY ||
        SDL_asprintf(&watch->full, "%s%s", full, ECSPath_IsSeparator(full[SDL_strlen(full) - 1]) ? "" : "/") < 0 ||
        SDL_asprintf(&watch->folder, "%s%s", folder, length > 0 && ECSPath_IsSeparator(folder[length - 1]) ? "" : "/") < 0 ||
        !FsReadFolder(watch->full, &watch->entries, &watch->entryCount) ||
        (FS.timer == nullptr && ECSTimer_Start(FS.plugin, &FS.timer, FS_POLL_INTERVAL, true, FsReadChanges, nullptr) != SHUResult_Ok))
    {
        if (FS.watchCount == 0 && FS.timer != nullptr)
        {
            ECSTimer_Stop(&FS.timer);
        }

        ECS_LogWarn(FS.plugin, "Cannot watch the folder '%s'", folder);

        if (watch != nullptr)
        {
            for (usz i = 0; i < watch->entryCount; i++)
            {
                SDL_free(watch->entries[i].name);
            }

            SDL_free(watch->entries);
            SDL_free(watch->full);
            SDL_free(watch->folder);
        }

        SDL_free(watch);
        SDL_free(full);
        ECSService_ReleaseCallback((ECSFunction)Changed);
        return nullptr;
    }

    SDL_free(full);
    watch->descriptor = -1;
    watch->Changed = Changed;
    FS.watches[FS.watchCount++] = watch;
    return watch;
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
