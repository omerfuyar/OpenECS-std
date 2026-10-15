// The build of OpenECS's standard plugins. It runs inside a checkout of OpenECS, in its std/ folder: OpenECS's shuild.c compiles this file with shu and shuild
// from OpenECS's dependencies/ into shuild.ignore here, and runs it with the build type and OpenECS's build folder (DESIGN 1).
// It builds each plugin into bin/plugins/, and copies the presets into bin/presets/.
#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#define SHUC_NO_RUN_LOG
#define SHUC_MAX_COMMAND_BUFFER_SIZE 65536 // a library of many files is packed with one command that names each of them
#include "shuild/shuild.h"


#pragma region Setup

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

static const char *const BUILD_TYPE_STRINGS[] = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
#define BuildType_String(buildType) BUILD_TYPE_STRINGS[(buildType)]

/// @brief What the flags choose.
static struct
{
    BuildType type;
    const char *output; // OpenECS's build folder, such as ../build/Debug/
} CONFIG = {BuildType_Debug, NULL};

/// @brief A library of dependencies/ with many C files, which the build compiles into a static archive that a plugin's library takes whole.
typedef struct Library
{
    const char *name;
    const char *sources;     // the folder of its C files, in dependencies/
    const char *includes[6]; // the folders of dependencies/ that its files include, ending with NULL
    const char *flags;       // the definitions it is configured with
    const char *unity;       // for a library compiled as one file that includes all of its C files: the definitions at the file's top; or NULL
} Library;

/// @brief A plugin that compiles code of std's dependencies into itself: the folders of dependencies/ that its C files include, and the libraries it takes.
typedef struct PluginNeeds
{
    const char *name;
    const char *includes[5];     // ending with NULL
    const char *flags;           // definitions for its C files, or NULL
    const Library *libraries[3]; // ending with NULL
    bool analyze;                // its C files hold only its own code and headers, so the static analyzer can afford them
} PluginNeeds;

// mbedTLS's default configuration, with the changes that web needs; curl and web include its headers too, so all three see the same configuration
#define MBEDTLS_FLAGS " '-DMBEDTLS_USER_CONFIG_FILE=<mbedtls_user_config.h>'"

// curl is configured by a glue header in place of its configure script, for HTTP and HTTPS with mbedTLS;
// it is compiled as one file, as its own unity builds are, because shuild finds the headers a file includes without the build's definitions, and curl's need HAVE_CONFIG_H
static const Library CURL = {"curl", "curl/lib/", {"other/curl", "curl/include", "curl/lib", "mbedtls/include", "other/mbedtls", NULL}, MBEDTLS_FLAGS,
                             "#define HAVE_CONFIG_H\n#define BUILDING_LIBCURL\n#define CURL_STATICLIB\n"};
static const Library MBEDTLS = {"mbedtls", "mbedtls/library/", {"mbedtls/include", "mbedtls/library", "other/mbedtls", NULL}, MBEDTLS_FLAGS, NULL};

// every standard plugin links nothing but OpenECS and the SDL libraries beside it; the code of its other libraries is compiled into it
static const PluginNeeds PLUGIN_NEEDS[] = {
    {"image", {"stb", "nanosvg/src", NULL}, NULL, {NULL}, false},
    {"audio", {"dr_libs", "stb", NULL}, NULL, {NULL}, false},
    {"net", {"SDL_net", "SDL_net/include", "mbedtls/include", "other/mbedtls"}, MBEDTLS_FLAGS, {&MBEDTLS, NULL}, false},
    {"gltf", {"cgltf", NULL}, NULL, {NULL}, false},
    {"web", {"curl/include", "mbedtls/include", "other/mbedtls", NULL}, MBEDTLS_FLAGS, {&CURL, &MBEDTLS, NULL}, true},
};

#pragma endregion Setup

#pragma region Declarations

/// @brief Runs a shell command, and stops the build if it fails.
#define RunCheck(cmd, ...) SHU_Assert(!SHU_UtilRun(cmd, ##__VA_ARGS__), "Running " SHUM_COLOR_RED("'%s'") " failed", cmd)

static void SetupConfiguration(int argc, char **argv);
static void SetBuildFlags(void);
static void ForEachFolder(const char *path, void (*function)(const char *path, const char *name));
static void BuildPlugin(const char *path, const char *name);

#pragma endregion Declarations

int main(int argc, char **argv)
{
    SHU_CompilerTryConfigure("gcc");
    SetupConfiguration(argc, argv);

    // each folder of plugins/ is a standard plugin
    ForEachFolder("plugins/", BuildPlugin);

    // the presets go to bin/presets/, beside OpenECS's own
    SHUI_String folder;
    SHUI_SFormat(&folder, "%sbin/presets/", CONFIG.output);
    SHU_UtilCreateDirectory(folder.data);
    RunCheck("cp -rp presets/. %s", folder.data);

    return 0;
}

static void PrintUsageAndExit(int status)
{
    printf("Usage: ./shuild.ignore -o FOLDER [FLAG...]\n\n"
           "OpenECS's shuild.c runs this build; see OpenECS's README.md.\n\n"
           "Flags:\n"
           "  -o, --output FOLDER  OpenECS's build folder, named from this folder, such as ../build/Debug/\n"
           "  -b, --build TYPE     Build type: debug (the default), release, relwithdebinfo or minsizerel\n"
           "  -h, --help           Show this help\n");
    exit(status);
}

/// @brief Stops the build with what is wrong, the argument, and the usage.
static void Refuse(const char *what, const char *argument)
{
    SHU_LogError(0, "%s: " SHUM_COLOR_RED("'%s'"), what, argument);
    PrintUsageAndExit(1);
}

static void SetupConfiguration(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char *flag = argv[i];

        if (!strcmp(flag, "-b") || !strcmp(flag, "--build"))
        {
            const char *name = i + 1 < argc ? argv[++i] : "";
            usz type = 0;

            while (type < sizeof(BUILD_TYPE_STRINGS) / sizeof(*BUILD_TYPE_STRINGS) && strcasecmp(name, BuildType_String(type)) != 0)
            {
                type++;
            }

            if (type == sizeof(BUILD_TYPE_STRINGS) / sizeof(*BUILD_TYPE_STRINGS))
            {
                Refuse("Unknown build type", name);
            }

            CONFIG.type = (BuildType)type;
        }
        else if (!strcmp(flag, "-o") || !strcmp(flag, "--output"))
        {
            CONFIG.output = i + 1 < argc ? argv[++i] : NULL;
        }
        else if (!strcmp(flag, "-h") || !strcmp(flag, "--help"))
        {
            PrintUsageAndExit(0);
        }
        else
        {
            Refuse(flag[0] == '-' ? "Unknown flag" : "Unknown argument", flag);
        }
    }

    if (CONFIG.output == NULL || CONFIG.output[0] == '\0' || CONFIG.output[strlen(CONFIG.output) - 1] != '/')
    {
        Refuse("The output folder is missing, or does not end with /", CONFIG.output == NULL ? "" : CONFIG.output);
    }

    SHUI_String cache;
    SHUI_SFormat(&cache, ".shu/%s/", BUILD_TYPE_STRINGS[CONFIG.type]);
    SHU_CacheConfigure(cache.data);
}

// the plugins get OpenECS's warnings, and in Debug its static analyzer and sanitizers, as OpenECS's own code does
static void SetBuildFlags(void)
{
    SHU_CompilerClearFlags();
    SHU_CompilerAddFlags(SHUM_FLAGS_STANDARD_C23 " -fvisibility=hidden");

    switch (CONFIG.type)
    {
    case BuildType_Debug:
        SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_MID " -fanalyzer -fsanitize=address,undefined -fno-omit-frame-pointer" SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_DEBUG);
        SHU_CompilerAddDefinitions("DEBUG", NULL, "SDL_ASSERT_LEVEL", "2");
        break;
    case BuildType_Release:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_HIGH);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SDL_ASSERT_LEVEL", "0");
        break;
    case BuildType_RelWithDebInfo:
        SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_LOW SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_MID);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SDL_ASSERT_LEVEL", "0");
        break;
    case BuildType_MinSizeRel:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_SIZE);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SDL_ASSERT_LEVEL", "0");
        break;
    }
}

/// @brief A folder that ForEachFolder lists, and the function it calls.
typedef struct FolderVisit
{
    const char *path;
    void (*function)(const char *path, const char *name);
} FolderVisit;

static void VisitFolder(const char *entry, SHUFileType type, void *data)
{
    const FolderVisit *visit = (const FolderVisit *)data;

    if (type == SHUFileType_Directory)
    {
        // the entry is the folder's path, with a separator at its end; the function gets its name alone
        SHUI_String name;
        SHUI_SFormat(&name, "%.*s", (int)(strlen(entry) - strlen(visit->path) - 1), entry + strlen(visit->path));
        visit->function(visit->path, name.data);
    }
}

/// @brief Calls a function for each folder in a folder, sorted by name; names starting with a dot are left out.
static void ForEachFolder(const char *path, void (*function)(const char *path, const char *name))
{
    FolderVisit visit = {path, function};
    SHU_UtilListDirectory(path, false, VisitFolder, &visit);
}

static void FindCode(const char *path, SHUFileType type, void *data)
{
    *(bool *)data = *(bool *)data || (type == SHUFileType_Regular && SHU_UtilMatch(path, "*.c"));
}

/// @brief Tells whether a folder holds a C file.
static bool HasCode(const char *path)
{
    bool code = false;
    SHU_UtilListDirectory(path, false, FindCode, &code);
    return code;
}

static void WriteInclude(const char *path, SHUFileType type, void *data)
{
    if (type == SHUFileType_Regular && SHU_UtilMatch(path, "*.c"))
    {
        fprintf((FILE *)data, "#include \"%s%s\"\n", SHU_UtilGetExecutablePath(), path);
    }
}

/// @brief Writes an #include of each C file in a folder and in its folders, sorted by path.
static void WriteIncludes(FILE *file, const char *folder)
{
    SHU_UtilListDirectory(folder, true, WriteInclude, file);
}

/// @brief Writes a library's unity file, and replaces the old one only if it changed, so the library compiles again only when its files change.
static void WriteUnity(const Library *library, const char *path)
{
    SHUI_String fresh;
    SHUI_String sources;
    SHUI_SFormat(&fresh, "%s.new", path);
    SHUI_SFormat(&sources, "dependencies/%s", library->sources);

    FILE *file = fopen(fresh.data, "w");
    SHU_Assert(file != NULL, "Cannot write " SHUM_COLOR_RED("'%s'"), fresh.data);
    fputs(library->unity, file);
    WriteIncludes(file, sources.data);
    fclose(file);
    RunCheck("cmp -s %s %s && rm %s || mv %s %s", fresh.data, path, fresh.data, fresh.data, path);
}

/// @brief Compiles a library of dependencies/ into a static archive in a folder. Like OpenECS's dependencies, it gets neither the warnings, nor the static analyzer, nor the sanitizers (OpenECS DESIGN 17.3).
static void BuildLibrary(const Library *library, const char *folder)
{
    SHU_ModuleBegin(library->name, "dependencies/");
    SHU_CompilerClearFlags();
    SHU_CompilerAddFlags(" -std=gnu17 -fPIC -fvisibility=hidden -w");
    SHU_CompilerAddFlags(CONFIG.type == BuildType_Debug ? SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_DEBUG : SHUM_FLAGS_OPTIMIZATION_HIGH " -DNDEBUG");
    SHU_CompilerAddFlags(library->flags);

    for (usz i = 0; library->includes[i] != NULL; i++)
    {
        SHU_ModuleAddIncludeDirectory(library->includes[i]);
    }

    if (library->unity == NULL)
    {
        SHU_ModuleAddSourceFile(library->sources);
    }
    else
    {
        // the unity file is kept in other/, beside the library's glue header, and git ignores it
        SHUI_String unity;
        SHUI_SFormat(&unity, "dependencies/other/%s/ignore.unity.c", library->name);
        WriteUnity(library, unity.data);
        SHUI_SFormat(&unity, "other/%s/ignore.unity.c", library->name);
        SHU_ModuleAddSourceFile(unity.data);
    }

    SHU_ModuleCompile(folder, SHUModuleType_LibraryStatic);
}

/// @brief Compiles the C files of a plugin's folder, root, into the plugin's native library in output. Does nothing if the folder has no C file.
/// The plugin sees OpenECS's plugin interface and SDL from the build's include/ folder, and, if it has needs, the folders of dependencies/ it compiles in, as system headers, so their warnings stay out.
static void BuildNative(const char *name, const char *root, const char *output)
{
    if (!HasCode(root))
    {
        return;
    }

    const PluginNeeds *needs = NULL;

    for (usz i = 0; i < sizeof(PLUGIN_NEEDS) / sizeof(*PLUGIN_NEEDS); i++)
    {
        needs = strcmp(PLUGIN_NEEDS[i].name, name) == 0 ? &PLUGIN_NEEDS[i] : needs;
    }

    // paths are named from the plugin's folder, which is one level deeper for each folder in root
    SHUI_String up = {0};

    for (const char *c = root; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            SHUI_SAppendC(&up, "../");
        }
    }

    SHUI_String library;
    SHUI_SFormat(&library, "%slib%s.so", output, name);

    SHU_ModuleBegin(name, root);
    SetBuildFlags();

    // the static analyzer takes too much memory on the libraries that a plugin's C files include; the warnings and the sanitizers stay.
    // A plugin that takes its libraries only as archives keeps the analyzer, because its own files hold only their headers
    if (needs != NULL && !needs->analyze)
    {
        SHU_CompilerAddFlags(" -fno-analyzer");
    }

    for (usz i = 0; needs != NULL && needs->includes[i] != NULL; i++)
    {
        SHUI_String flag;
        SHUI_SFormat(&flag, " -isystem %sdependencies/%s", SHU_UtilGetExecutablePath(), needs->includes[i]);
        SHU_CompilerAddFlags(flag.data);

        // shuild finds the headers a file includes in the module's folders, so the plugin is rebuilt when they change
        SHUI_String folder;
        SHUI_SFormat(&folder, "%sdependencies/%s", up.data, needs->includes[i]);
        SHU_ModuleAddIncludeDirectory(folder.data);
    }

    SHUI_String include;
    SHUI_SFormat(&include, "%s%sinclude/", up.data, CONFIG.output);
    SHU_ModuleAddSourceFile("./");
    SHU_ModuleAddIncludeDirectory(include.data);

    if (needs == NULL || needs->libraries[0] == NULL)
    {
        SHU_ModuleCompile(output, SHUModuleType_LibraryDynamic);
        return;
    }

    // a plugin that takes libraries is packed into an archive too, and its native library is linked from the archives: all of its own, and what it uses of theirs
    SHU_CompilerAddFlags(" -fPIC");
    SHU_CompilerAddFlags(needs->flags);
    SHUI_String archives;
    SHUI_SFormat(&archives, ".shu/%s/archives/", BUILD_TYPE_STRINGS[CONFIG.type]);
    SHU_ModuleCompile(archives.data, SHUModuleType_LibraryStatic);

    SHUI_String link = {0};

    for (usz i = 0; needs->libraries[i] != NULL; i++)
    {
        BuildLibrary(needs->libraries[i], archives.data);
        SHUI_SAppendC(&link, " ");
        SHUI_SAppendC(&link, archives.data);
        SHUI_SAppendC(&link, "lib");
        SHUI_SAppendC(&link, needs->libraries[i]->name);
        SHUI_SAppendC(&link, ".a");
    }

    RunCheck("gcc -shared -o %s -Wl,--whole-archive %slib%s.a -Wl,--no-whole-archive%s", library.data, archives.data, name, link.data);
}

/// @brief Builds a standard plugin into bin/plugins/: its C files make its native library, and its Lua files, the manifest among them, its header.h and its folders are copied.
/// OpenECS --definitions reads types.lua and header.h from there (OpenECS DESIGN 10.10).
static void BuildPlugin(const char *path, const char *name)
{
    SHUI_String root;
    SHUI_String output;
    SHUI_SFormat(&root, "%s%s/", path, name);
    SHUI_SFormat(&output, "%sbin/plugins/%s/", CONFIG.output, name);
    SHU_UtilCreateDirectory(output.data);
    RunCheck("find %s -mindepth 1 -maxdepth 1 \\( -name '*.lua' -o -name header.h -o -type d \\) -exec cp -rp {} %s \\;", root.data, output.data);
    BuildNative(name, root.data, output.data);
}
