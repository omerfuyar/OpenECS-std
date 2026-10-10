// The fs standard plugin: what Lua's io and os libraries cannot do with files and folders (DESIGN 9).
// It lists folders, tells what a path is, makes folders, and copies, moves and removes files and folders, with SDL's filesystem functions.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief A name of the plugin: "fs." and a local name.
#define FS_NAME(localName) "fs." localName

/// @brief A full path: a relative path starts at the executable's folder, as the paths of the other standard plugins do. Free it with SDL_free.
static char *FsFull(const char *path)
{
    char *full = NULL;
    return SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0 ? NULL : full;
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
    ECSValue *field = NULL;
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

    if (full == NULL || !SDL_GetPathInfo(full, &info) || FsSetInfo(retInfo, &info) != SHUResult_Ok)
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
        char *path = NULL;
        SDL_PathInfo info = {0};
        ECSValue *item = NULL;
        ECSValue *name = NULL;

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
    char **names = full == NULL ? NULL : SDL_GlobDirectory(full, pattern[0] == '\0' ? "*" : pattern, 0, &count);

    if (names == NULL || FsFillList(full, names, count, retEntries) != SHUResult_Ok)
    {
        ECSValue_SetNil(retEntries);
    }

    SDL_free(names);
    SDL_free(full);
}

static bool FsMakeFolder(const char *path)
{
    char *full = FsFull(path);
    bool made = full != NULL && SDL_CreateDirectory(full);
    SDL_free(full);
    return made;
}

static bool FsCopy(const char *from, const char *to)
{
    char *fullFrom = FsFull(from);
    char *fullTo = FsFull(to);
    bool copied = fullFrom != NULL && fullTo != NULL && SDL_CopyFile(fullFrom, fullTo);
    SDL_free(fullFrom);
    SDL_free(fullTo);
    return copied;
}

static bool FsMove(const char *from, const char *to)
{
    char *fullFrom = FsFull(from);
    char *fullTo = FsFull(to);
    bool moved = fullFrom != NULL && fullTo != NULL && SDL_RenamePath(fullFrom, fullTo);
    SDL_free(fullFrom);
    SDL_free(fullTo);
    return moved;
}

static bool FsRemove(const char *path)
{
    char *full = FsFull(path);
    bool removed = full != NULL && SDL_RemovePath(full);
    SDL_free(full);
    return removed;
}

/// @brief Gives a folder of the computer by name: the executable's folder, the current folder, or one of the user's folders. A user's folder that the system does not have gives nil.
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

    return NULL;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {FS_NAME("info"), (ECSFunction)FsInfo, "void(string path, out value info)", "Tell what a path is: its type (file, folder or other), size and when it changed; nothing if it does not exist"},
        {FS_NAME("list"), (ECSFunction)FsList, "void(string folder, string pattern, out value entries)", "List the entries of a folder whose names match a pattern, such as *.lua, by name; an empty pattern matches every name"},
        {FS_NAME("makeFolder"), (ECSFunction)FsMakeFolder, "bool(string path)", "Make a folder, and the folders above it that are missing"},
        {FS_NAME("copy"), (ECSFunction)FsCopy, "bool(string from, string to)", "Copy a file, replacing what is at the new path"},
        {FS_NAME("move"), (ECSFunction)FsMove, "bool(string from, string to)", "Move or rename a file or a folder"},
        {FS_NAME("remove"), (ECSFunction)FsRemove, "bool(string path)", "Remove a file or an empty folder"},
        {FS_NAME("folder"), (ECSFunction)FsFolder, "string(string name)", "Give a folder by name: executable, home, desktop, documents, downloads, music, pictures, videos, screenshots or templates"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
}
