// The build of OpenECS's standard plugins. It runs inside a checkout of OpenECS, in its std/ folder: OpenECS's shuild.c compiles this file with shu and shuild
// from OpenECS's dependencies/ into shuild.ignore here, and runs it with the build type and OpenECS's build folder (DESIGN 1).
// It builds each plugin into bin/plugins/, copies the presets into bin/presets/, and, with --tests, the tests into std/tests/ beside bin/.
#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#define SHUC_NO_RUN_LOG
#include "shuild/shuild.h"

#include <dirent.h>

#pragma region Setup

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

static const char *const BUILD_TYPE_NAMES[] = {"debug", "release", "relwithdebinfo", "minsizerel"};
static const char *const BUILD_TYPE_FOLDERS[] = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"};

/// @brief What the flags choose.
static struct
{
    BuildType type;
    const char *output; // OpenECS's build folder, such as ../build/Debug/
    bool tests;         // also build the tests
} CONFIG = {BuildType_Debug, NULL, false};

/// @brief A plugin that compiles code of std's dependencies into itself: the folders of dependencies/ that its C files include.
typedef struct PluginNeeds
{
    const char *name;
    const char *includes[4]; // ending with NULL
} PluginNeeds;

// every standard plugin links nothing but OpenECS and the SDL libraries beside it; the code of its other libraries is compiled into it
static const PluginNeeds PLUGIN_NEEDS[] = {
    {"image", {"stb", "nanosvg/src", NULL}},
    {"audio", {"dr_libs", "stb", NULL}},
    {"net", {"SDL_net", "SDL_net/include", NULL}},
    {"gltf", {"cgltf", NULL}},
};

#pragma endregion Setup

static void PrintUsage(void)
{
    printf("Usage: ./shuild.ignore -o FOLDER [FLAG...]\n\n"
           "OpenECS's shuild.c runs this build; see OpenECS's README.md.\n\n"
           "Flags:\n"
           "  -o, --output FOLDER  OpenECS's build folder, named from this folder, such as ../build/Debug/\n"
           "  -b, --build TYPE     Build type: debug (the default), release, relwithdebinfo or minsizerel\n"
           "  -t, --tests          Also build the tests, into std/tests/ beside bin/\n"
           "  -h, --help           Show this help\n");
}

/// @brief Stops the build with what is wrong, the argument, and the usage.
static void Refuse(const char *what, const char *argument)
{
    SHU_LogError(0, "%s: " SHUM_COLOR_RED("'%s'"), what, argument);
    PrintUsage();
    exit(1);
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

            while (type < sizeof(BUILD_TYPE_NAMES) / sizeof(*BUILD_TYPE_NAMES) && strcasecmp(name, BUILD_TYPE_NAMES[type]) != 0)
            {
                type++;
            }

            if (type == sizeof(BUILD_TYPE_NAMES) / sizeof(*BUILD_TYPE_NAMES))
            {
                Refuse("Unknown build type", name);
            }

            CONFIG.type = (BuildType)type;
        }
        else if (!strcmp(flag, "-o") || !strcmp(flag, "--output"))
        {
            CONFIG.output = i + 1 < argc ? argv[++i] : NULL;
        }
        else if (!strcmp(flag, "-t") || !strcmp(flag, "--tests"))
        {
            CONFIG.tests = true;
        }
        else if (!strcmp(flag, "-h") || !strcmp(flag, "--help"))
        {
            PrintUsage();
            exit(0);
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
    SHUI_SFormat(&cache, ".shu/%s/", BUILD_TYPE_FOLDERS[CONFIG.type]);
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

static bool EndsWith(const char *name, const char *suffix)
{
    size_t nameLength = strlen(name);
    size_t suffixLength = strlen(suffix);
    return nameLength >= suffixLength && strcmp(name + nameLength - suffixLength, suffix) == 0;
}

/// @brief Calls a function for each folder in a folder, in no order; names starting with a dot are left out.
static void ForEachFolder(const char *path, void (*function)(const char *path, const char *name))
{
    DIR *folder = opendir(path);
    struct dirent *entry = NULL;

    while (folder != NULL && (entry = readdir(folder)) != NULL)
    {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.')
        {
            function(path, entry->d_name);
        }
    }

    if (folder != NULL)
    {
        closedir(folder);
    }
}

/// @brief Tells whether a folder holds a C file.
static bool HasCode(const char *path)
{
    bool code = false;
    DIR *folder = opendir(path);
    struct dirent *entry = NULL;

    while (folder != NULL && (entry = readdir(folder)) != NULL)
    {
        code = code || EndsWith(entry->d_name, ".c");
    }

    if (folder != NULL)
    {
        closedir(folder);
    }

    return code;
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

    SHU_ModuleBegin(name, root);
    SetBuildFlags();

    // the static analyzer takes too much memory on the libraries compiled into a plugin; the warnings and the sanitizers stay
    if (needs != NULL)
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
    SHU_ModuleCompile(output, SHUModuleType_LibraryDynamic);
}

/// @brief Builds a standard plugin into bin/plugins/: its C files make its native library, and its Lua files, the manifest among them, and its folders are copied.
static void BuildPlugin(const char *path, const char *name)
{
    SHUI_String root;
    SHUI_String output;
    SHUI_SFormat(&root, "%s%s/", path, name);
    SHUI_SFormat(&output, "%sbin/plugins/%s/", CONFIG.output, name);
    SHU_UtilCreateDirectory(output.data);
    SHU_UtilRun("find %s -mindepth 1 -maxdepth 1 \\( -name '*.lua' -o -type d \\) -exec cp -r {} %s \\;", root.data, output.data);
    BuildNative(name, root.data, output.data);
}

/// @brief Builds a test plugin in its copy beside bin/.
static void BuildTestPlugin(const char *path, const char *name)
{
    SHUI_String root;
    SHUI_String output;
    SHUI_SFormat(&root, "%s%s/", path, name);
    SHUI_SFormat(&output, "%sstd/%s", CONFIG.output, root.data);
    BuildNative(name, root.data, output.data);
}

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
    SHU_UtilRun("cp -r presets/. %s", folder.data);

    // the tests go to std/tests/ beside bin/, apart from OpenECS's own, with their presets and test plugins
    if (CONFIG.tests)
    {
        SHUI_SFormat(&folder, "%sstd/", CONFIG.output);
        SHU_UtilRun("rm -rf %s && mkdir -p %s && cp -r tests %s", folder.data, folder.data, folder.data);
        ForEachFolder("tests/plugins/", BuildTestPlugin);
    }

    return 0;
}
