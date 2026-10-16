// The build of OpenECS's standard plugins. It runs inside a checkout of OpenECS, in its std/ folder: OpenECS's shuild.c compiles this file with shu and shuild
// from OpenECS's dependencies/ into shuild.ignore here, and runs it with the build type and OpenECS's build folder (DESIGN 1).
// It builds each plugin into bin/plugins/, and copies the presets into bin/presets/.
#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
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
    const char *output;   // OpenECS's build folder, such as ../build/Debug/
    const char *compiler; // OpenECS's compiler, such as gcc, or x86_64-w64-mingw32-clang for Windows
    bool windows;         // the compiler compiles for Windows
    char *cache;          // shuild's cache, such as .shu/Debug/
} CONFIG = {BuildType_Debug, NULL, "gcc", false, NULL};

/// @brief A library of dependencies/ with many C files, which the build compiles into a static archive that a plugin's library takes.
typedef struct Library
{
    const char *name;
    const char *sources;             // the folder of its C files, in dependencies/
    const char *includes[3];         // the folders of dependencies/ that only its own files include, ending with NULL
    const char *headers[3];          // the folders of dependencies/ of its interface, which shuild gives the modules that link it, ending with NULL
    const char *flags;               // the definitions it is configured with
    const struct Library *links[2];  // the libraries it uses, whose headers it gets by linking them, ending with NULL
    const char *windowsLinks[5];     // the Windows libraries it uses, ending with NULL
} Library;

/// @brief A plugin that compiles code of std's dependencies into itself: the folders of dependencies/ of the headers and sources its C files include, and the libraries it takes, whose headers it gets by linking them.
typedef struct PluginNeeds
{
    const char *name;
    const char *includes[3];     // ending with NULL
    const char *flags;           // definitions for its C files, or NULL
    const Library *libraries[3]; // ending with NULL
    const char *windowsLinks[4]; // the Windows libraries it uses, ending with NULL
    bool analyze;                // its C files hold only its own code and headers, so the static analyzer can afford them
} PluginNeeds;

// mbedTLS's default configuration, with the changes that web needs; curl and web include its headers too, so all three see the same configuration
#define MBEDTLS_FLAGS " '-DMBEDTLS_USER_CONFIG_FILE=<mbedtls_user_config.h>'"

// curl is configured by a glue header, which HAVE_CONFIG_H names, in place of its configure script, for HTTP and HTTPS with mbedTLS
static const Library MBEDTLS = {"mbedtls", "mbedtls/library/", {"mbedtls/library", NULL}, {"mbedtls/include", "other/mbedtls", NULL}, MBEDTLS_FLAGS, {NULL}, {"bcrypt", NULL}};
static const Library CURL = {"curl", "curl/lib/", {"other/curl", "curl/lib", NULL}, {"curl/include", NULL}, MBEDTLS_FLAGS " -DHAVE_CONFIG_H -DBUILDING_LIBCURL -DCURL_STATICLIB", {&MBEDTLS, NULL}, {"ws2_32", "iphlpapi", "crypt32", "bcrypt"}};

// every standard plugin links nothing but OpenECS and the SDL libraries beside it, and on Windows the system's libraries it uses; the code of its other libraries is compiled into it
static const PluginNeeds PLUGIN_NEEDS[] = {
    {"image", {"stb", "nanosvg/src", NULL}, NULL, {NULL}, {NULL}, false},
    {"audio", {"dr_libs", "stb", NULL}, NULL, {NULL}, {NULL}, false},
    {"net", {"SDL_net", "SDL_net/include", NULL}, MBEDTLS_FLAGS, {&MBEDTLS, NULL}, {"ws2_32", "iphlpapi", NULL}, false},
    {"gltf", {"cgltf", NULL}, NULL, {NULL}, {NULL}, false},
    {"web", {NULL}, MBEDTLS_FLAGS " -DCURL_STATICLIB", {&CURL, &MBEDTLS, NULL}, {NULL}, true},
};

#pragma endregion Setup

#pragma region Declarations

static void SetupConfiguration(int argc, char **argv);
static void SetBuildFlags(bool ownCode);
static void ForEachFolder(const char *path, void (*function)(const char *path, const char *name));
static void BuildPlugin(const char *path, const char *name);

#pragma endregion Declarations

/// @brief The compiler that the flags name, or gcc.
static const char *CompilerArgument(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++)
    {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--compiler"))
        {
            return argv[i + 1];
        }
    }

    return CONFIG.compiler;
}

int main(int argc, char **argv)
{
    CONFIG.compiler = CompilerArgument(argc, argv);
    SHU_CompilerTryConfigure(CONFIG.compiler);
    CONFIG.windows = SHU_CompilerGetTarget() == SHUM_PLATFORM_WINDOWS;
    SetupConfiguration(argc, argv);

    // each folder of plugins/ is a standard plugin
    ForEachFolder("plugins/", BuildPlugin);

    // the presets go to bin/presets/, beside OpenECS's own
    char *folder = SHU_UtilFormat("%sbin/presets/", CONFIG.output);
    SHU_UtilCopyFile("presets/", folder);

    return 0;
}

static void PrintUsageAndExit(int status)
{
    printf("Usage: ./shuild.ignore -o FOLDER [FLAG...]\n\n"
           "OpenECS's shuild.c runs this build; see OpenECS's README.md.\n\n"
           "Flags:\n"
           "  -o, --output FOLDER  OpenECS's build folder, named from this folder, such as ../build/Debug/\n"
           "  -b, --build TYPE     Build type: debug (the default), release, relwithdebinfo or minsizerel\n"
           "  -c, --compiler CC    OpenECS's compiler: gcc (the default), clang, or one for Windows, such as x86_64-w64-mingw32-clang\n"
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
        else if (!strcmp(flag, "-c") || !strcmp(flag, "--compiler"))
        {
            // main configured the compiler already
            i++;
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

    // a build for another system has a cache of its own, as OpenECS's build has a folder of its own
    u8 target = SHU_CompilerGetTarget();
    CONFIG.cache = SHU_UtilFormat(".shu/%s%s%s/", BUILD_TYPE_STRINGS[CONFIG.type], SHUM_PLATFORM_IS_HOST(target) ? "" : "-", SHUM_PLATFORM_IS_HOST(target) ? "" : SHUM_PLATFORM_GET_STRING(target));
    SHU_CacheConfigure(CONFIG.cache);
}

// the plugins get OpenECS's warnings, and in Debug gcc's static analyzer and the sanitizers, as OpenECS's own code does, except where MinGW has no sanitizers;
// the libraries they compile in get none of them, like OpenECS's dependencies (OpenECS DESIGN 17.3), and are C17 with GNU extensions, compiled to go into a plugin's library
static void SetBuildFlags(bool ownCode)
{
    SHU_CompilerClearFlags();
    SHU_CompilerAddFlags(ownCode ? SHUM_FLAGS_STANDARD_C23 : " -std=gnu17 -w");
    SHU_CompilerAddFlags(" -fvisibility=hidden");

    // code for a DLL is always position independent
    if (!ownCode && !CONFIG.windows)
    {
        SHU_CompilerAddFlags(" -fPIC");
    }

    switch (CONFIG.type)
    {
    case BuildType_Debug:
        if (ownCode)
        {
            SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_MID);

            if (SHU_CompilerGetIdentifier() == SHUM_COMPILER_GCC)
            {
                SHU_CompilerAddFlags(" -fanalyzer");
            }

            if (!CONFIG.windows)
            {
                SHU_CompilerAddFlags(" -fsanitize=address,undefined -fno-omit-frame-pointer");
            }
        }
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_DEBUG);
        SHU_CompilerAddDefinitions("DEBUG", NULL, "SDL_ASSERT_LEVEL", "2");
        break;
    case BuildType_Release:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_HIGH);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SDL_ASSERT_LEVEL", "0");
        break;
    case BuildType_RelWithDebInfo:
        if (ownCode)
        {
            SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_LOW);
        }
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_MID);
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
        char *name = SHU_UtilFormat("%.*s", (int)(strlen(entry) - strlen(visit->path) - 1), entry + strlen(visit->path));
        visit->function(visit->path, name);
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

/// @brief Compiles a library of dependencies/ into a static archive in a folder. Like OpenECS's dependencies, it gets neither the warnings, nor the static analyzer, nor the sanitizers (OpenECS DESIGN 17.3).
static void BuildLibrary(const Library *library, const char *folder)
{
    // the libraries it uses are built first, so it gets their headers, and the modules that link it get them as libraries too
    for (usz i = 0; library->links[i] != NULL; i++)
    {
        BuildLibrary(library->links[i], folder);
    }

    SHU_ModuleBegin(library->name, "dependencies/");
    SetBuildFlags(false);
    SHU_CompilerAddFlags(library->flags);

    for (usz i = 0; library->includes[i] != NULL; i++)
    {
        SHU_ModuleAddIncludeDirectory(library->includes[i]);
    }

    for (usz i = 0; library->headers[i] != NULL; i++)
    {
        SHU_ModuleAddPublicIncludeDirectory(library->headers[i]);
    }

    for (usz i = 0; library->links[i] != NULL; i++)
    {
        if (i == 0)
        {
            SHU_ModuleAddLibraryDirectory(folder);
        }

        SHU_ModuleLinkLibrary(library->links[i]->name);
    }

    // the system's libraries go to the plugin that links this one
    for (usz i = 0; CONFIG.windows && library->windowsLinks[i] != NULL; i++)
    {
        SHU_ModuleLinkLibrary(library->windowsLinks[i]);
    }

    // each C file compiles alone, in parallel, and the compiler finds its headers with the library's definitions
    SHU_ModuleAddSourceFile(library->sources);
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
    const char *up = "";

    for (const char *c = root; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            up = SHU_UtilFormat("%s../", up);
        }
    }

    // the libraries it takes are compiled first, into static archives that its native library is linked with
    char *archives = SHU_UtilFormat("%sarchives/", CONFIG.cache);

    for (usz i = 0; needs != NULL && needs->libraries[i] != NULL; i++)
    {
        BuildLibrary(needs->libraries[i], archives);
    }

    SHU_ModuleBegin(name, root);
    SetBuildFlags(true);

    // the static analyzer takes too much memory on the libraries that a plugin's C files include; the warnings and the sanitizers stay.
    // A plugin that takes its libraries only as archives keeps the analyzer, because its own files hold only their headers
    if (needs != NULL && !needs->analyze && SHU_CompilerGetIdentifier() == SHUM_COMPILER_GCC)
    {
        SHU_CompilerAddFlags(" -fno-analyzer");
    }

    for (usz i = 0; needs != NULL && needs->includes[i] != NULL; i++)
    {
        char *flag = SHU_UtilFormat(" -isystem %sdependencies/%s", SHU_UtilGetExecutablePath(), needs->includes[i]);
        SHU_CompilerAddFlags(flag);

        // shuild finds the headers a file includes in the module's folders, so the plugin is rebuilt when they change
        char *folder = SHU_UtilFormat("%sdependencies/%s", up, needs->includes[i]);
        SHU_ModuleAddIncludeDirectory(folder);
    }

    if (needs != NULL && needs->flags != NULL)
    {
        SHU_CompilerAddFlags(needs->flags);
    }

    char *include = SHU_UtilFormat("%s%sinclude/", up, CONFIG.output);
    SHU_ModuleAddSourceFile("./");
    SHU_ModuleAddIncludeDirectory(include);

    // the linker takes what the plugin uses of each library, in the order they are listed
    for (usz i = 0; needs != NULL && needs->libraries[i] != NULL; i++)
    {
        if (i == 0)
        {
            SHU_ModuleAddLibraryDirectory(archives);
        }

        SHU_ModuleLinkLibrary(needs->libraries[i]->name);
    }

    // a DLL calls OpenECS and SDL through their import libraries, in OpenECS's lib/ (OpenECS DESIGN 17.2); elsewhere the calls are resolved when it is loaded
    if (CONFIG.windows)
    {
        SHU_ModuleAddLibraryDirectory(SHU_UtilFormat("%slib/", CONFIG.output));
        SHU_ModuleLinkLibrary("OpenECS");
        SHU_ModuleLinkLibrary("SDL3_ttf");
        SHU_ModuleLinkLibrary("SDL3");

        for (usz i = 0; needs != NULL && needs->windowsLinks[i] != NULL; i++)
        {
            SHU_ModuleLinkLibrary(needs->windowsLinks[i]);
        }
    }

    SHU_ModuleCompile(output, SHUModuleType_LibraryDynamic);
}

/// @brief Copies a file or folder of a plugin's folder into the plugin's folder of bin/plugins/, if it is a Lua file, the manifest among them, its header.h or a folder.
static void CopyPluginFile(const char *path, SHUFileType type, void *data)
{
    const char *output = (const char *)data;

    if (type == SHUFileType_Directory)
    {
        // the entry is the folder's path, with a separator at its end, a backslash on Windows; its copy keeps its name
        const char *name = path + strlen(path) - 1;

        while (name > path && name[-1] != '/' && name[-1] != SHUM_PATH_SEPARATOR)
        {
            name--;
        }

        char *folder = SHU_UtilFormat("%s%s", output, name);
        SHU_UtilCopyFile(path, folder);
    }
    else if (SHU_UtilMatch(path, "*.lua") || SHU_UtilMatch(path, "*/header.h") || SHU_UtilMatch(path, "*" SHUM_PATH_SEPARATOR_STR "header.h"))
    {
        SHU_UtilCopyFile(path, output);
    }
}

/// @brief Builds a standard plugin into bin/plugins/: its C files make its native library, and its Lua files, the manifest among them, its header.h and its folders are copied.
/// OpenECS --definitions reads types.lua and header.h from there (OpenECS DESIGN 10.10).
static void BuildPlugin(const char *path, const char *name)
{
    char *root = SHU_UtilFormat("%s%s/", path, name);
    char *output = SHU_UtilFormat("%sbin/plugins/%s/", CONFIG.output, name);
    SHU_UtilCreateDirectory(output);
    SHU_UtilListDirectory(root, false, CopyPluginFile, (void *)output);
    BuildNative(name, root, output);
}
