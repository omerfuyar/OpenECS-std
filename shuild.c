// The build of OpenECS's standard plugins. It runs inside a checkout of OpenECS, in its std/ folder: OpenECS's shuild.c compiles this file with shu and shuild
// from OpenECS's dependencies/ into shuild.ignore here, and runs it with the build type and OpenECS's build folder (DESIGN 1).
// It builds each plugin into bin/plugins/, copies the presets into bin/presets/, and cglm's headers into include/cglm/.
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

/// @brief The number of items of an array.
#define COUNT(array) (sizeof(array) / sizeof(*(array)))

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

/// @brief A C++ library of dependencies/, which CMake builds into static archives in the cache, as shuild compiles only C. A plugin that takes one
/// also takes the C++ compiler's own library, so it still needs nothing but OpenECS (DESIGN 1).
typedef struct CMakeLibrary
{
    const char *name;        // its folder in dependencies/, and its build's folder in the cache
    const char *options;     // CMake's options
    const char *includes[3]; // the folders of dependencies/ of its interface, ending with NULL
    const char *built[2];    // the folders of its build of its interface, such as of the headers it writes, ending with NULL
    const char *archives[6]; // its archives in its build, in the order the linker takes them, ending with NULL
} CMakeLibrary;

/// @brief A plugin that compiles code of std's dependencies into itself: the folders of dependencies/ of the headers and sources its C files include, and the libraries it takes, whose headers it gets by linking them.
typedef struct PluginNeeds
{
    const char *name;
    const char *includes[3];     // ending with NULL
    const char *flags;           // definitions for its C files, or NULL
    const Library *libraries[3]; // ending with NULL
    const char *windowsLinks[4]; // the Windows libraries it uses, ending with NULL
    bool analyze;                // its C files hold only its own code and headers, so the static analyzer can afford them
    const CMakeLibrary *cmake[3]; // the C++ libraries it takes, ending with NULL
} PluginNeeds;

// mbedTLS's default configuration, with the changes that web needs; curl and web include its headers too, so all three see the same configuration
#define MBEDTLS_FLAGS " '-DMBEDTLS_USER_CONFIG_FILE=<mbedtls_user_config.h>'"

// curl is configured by a glue header, which HAVE_CONFIG_H names, in place of its configure script, for HTTP and HTTPS with mbedTLS
static const Library MBEDTLS = {"mbedtls", "mbedtls/library/", {"mbedtls/library", NULL}, {"mbedtls/include", "other/mbedtls", NULL}, MBEDTLS_FLAGS, {NULL}, {"bcrypt", NULL}};
static const Library CURL = {"curl", "curl/lib/", {"other/curl", "curl/lib", NULL}, {"curl/include", NULL}, MBEDTLS_FLAGS " -DHAVE_CONFIG_H -DBUILDING_LIBCURL -DCURL_STATICLIB", {&MBEDTLS, NULL}, {"ws2_32", "iphlpapi", "crypt32", "bcrypt"}};

// glslang compiles GLSL to SPIR-V, without its optimizer, its HLSL and its tools; SPIRV-Cross turns SPIR-V into HLSL, for Direct3D 12 (DESIGN 15)
static const CMakeLibrary GLSLANG = {"glslang",
                                     "-DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_SHARED_LIBS=OFF "
                                     "-DGLSLANG_ENABLE_INSTALL=OFF -DENABLE_SPVREMAPPER=OFF",
                                     {"glslang", NULL},
                                     {"include", NULL},
                                     {"glslang/libglslang.a", "glslang/libglslang-default-resource-limits.a", NULL}};
static const CMakeLibrary SPIRV_CROSS = {"SPIRV-Cross",
                                         "-DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF "
                                         "-DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF "
                                         "-DSPIRV_CROSS_SKIP_INSTALL=ON",
                                         {"SPIRV-Cross", NULL},
                                         {NULL},
                                         {"libspirv-cross-c.a", "libspirv-cross-hlsl.a", "libspirv-cross-glsl.a", "libspirv-cross-core.a", NULL}};

// every standard plugin links nothing but OpenECS and the SDL libraries beside it, and on Windows the system's libraries it uses; the code of its other libraries is compiled into it
static const PluginNeeds PLUGIN_NEEDS[] = {
    {"image", {"stb", "nanosvg/src", NULL}, NULL, {NULL}, {NULL}, false},
    {"audio", {"dr_libs", "stb", NULL}, NULL, {NULL}, {NULL}, false},
    {"net", {"SDL_net", "SDL_net/include", NULL}, MBEDTLS_FLAGS, {&MBEDTLS, NULL}, {"ws2_32", "iphlpapi", "crypt32", NULL}, false},
    {"gltf", {"cgltf", "cglm/include", NULL}, NULL, {NULL}, {NULL}, false},
    {"cglm", {"cglm/include", NULL}, NULL, {NULL}, {NULL}, false},
    {"web", {NULL}, MBEDTLS_FLAGS " -DCURL_STATICLIB", {&CURL, &MBEDTLS, NULL}, {"crypt32", NULL}, true},
    {"gpu", {NULL}, NULL, {NULL}, {NULL}, true, {&GLSLANG, &SPIRV_CROSS, NULL}},
};

#pragma endregion Setup

#pragma region Declarations

static void SetupConfiguration(int argc, char **argv);
static void SetBuildFlags(bool ownCode);
static void ForEachFolder(const char *path, void (*function)(const char *path, const char *name));
static void BuildPlugin(const char *path, const char *name);

#pragma endregion Declarations

int main(int argc, char **argv)
{
    SetupConfiguration(argc, argv);

    // cglm's headers go to OpenECS's include/, beside SDL's, so C plugins get cglm through the cglm plugin's header (DESIGN 16)
    SHU_UtilCopyFile("dependencies/cglm/include/cglm/", SHU_UtilFormat("%sinclude/cglm/", CONFIG.output));

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
    SHU_UtilFail(0, "%s: " SHUM_LOG_VALUE("%s"), what, argument);
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

            while (type < COUNT(BUILD_TYPE_STRINGS) && strcasecmp(name, BUILD_TYPE_STRINGS[type]) != 0)
            {
                type++;
            }

            if (type == COUNT(BUILD_TYPE_STRINGS))
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
            CONFIG.compiler = i + 1 < argc ? argv[++i] : CONFIG.compiler;
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

    SHU_CompilerConfigureTry(CONFIG.compiler);
    CONFIG.windows = SHU_CompilerGetTarget() == SHUPlatform_Windows;

    // a build for another system has a cache of its own, as OpenECS's build has a folder of its own
    SHUPlatform target = SHU_CompilerGetTarget();
    CONFIG.cache = SHU_UtilFormat(".shu/%s%s%s/", BUILD_TYPE_STRINGS[CONFIG.type], SHUM_PLATFORM_IS_HOST(target) ? "" : "-", SHUM_PLATFORM_IS_HOST(target) ? "" : SHUPlatform_GetString(target));
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

            if (SHU_CompilerGetFamily() == SHUCompiler_GCC)
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

/// @brief The compiler's tool of another name, such as g++ for gcc, or x86_64-w64-mingw32-clang++ for x86_64-w64-mingw32-clang.
/// @param gcc The tool's name for gcc. @param clang The tool's name for clang.
static char *CompilerTool(const char *gcc, const char *clang)
{
    const char *name = SHU_CompilerGetFamily() == SHUCompiler_Clang ? "clang" : "gcc";
    const char *found = NULL;

    for (const char *c = strstr(CONFIG.compiler, name); c != NULL; c = strstr(c + 1, name))
    {
        found = c;
    }

    return found == NULL ? SHU_UtilFormat("%s", CONFIG.compiler) : SHU_UtilFormat("%.*s%s%s", (int)(found - CONFIG.compiler), CONFIG.compiler, !strcmp(name, "clang") ? clang : gcc, found + strlen(name));
}

/// @brief The folder of a C++ library's build in the cache.
static char *CMakeFolder(const CMakeLibrary *library)
{
    return SHU_UtilFormat("%s%scmake/%s/", SHU_UtilGetScriptDirectory(), CONFIG.cache, library->name);
}

/// @brief Builds a C++ library of dependencies/ with CMake, with OpenECS's compiler and its C++ compiler, into static archives in the cache, unless they
/// are there. It is built for Release in every build type, as a shader takes long to compile in Debug, and like OpenECS's dependencies it gets neither
/// the warnings, nor the analyzer, nor the sanitizers. Its symbols are hidden, so they stay inside the plugin. Each step's output goes to a log beside it.
static void BuildCMakeLibrary(const CMakeLibrary *library)
{
    char *folder = CMakeFolder(library);
    bool built = true;

    for (usz i = 0; library->archives[i] != NULL; i++)
    {
        built = built && SHU_UtilFileExists(SHU_UtilFormat("%s%s", folder, library->archives[i])) == SHUFileType_Regular;
    }

    if (built)
    {
        return;
    }

    SHU_UtilLog(SHUM_LOG_NAME("%s") " (CMake): building", library->name);
    char *compilers = SHU_UtilFormat("-DCMAKE_C_COMPILER=\"%s\" -DCMAKE_CXX_COMPILER=\"%s\"", CONFIG.compiler, CompilerTool("g++", "clang++"));

    // a cross compiler builds for its system
    if (!SHUM_PLATFORM_IS_HOST(SHU_CompilerGetTarget()))
    {
        compilers = SHU_UtilFormat("%s -DCMAKE_SYSTEM_NAME=%s", compilers, SHUPlatform_GetString(SHU_CompilerGetTarget()));
    }

    char *logs = SHU_UtilFormat("%scmake/%s/", CONFIG.cache, library->name);
    SHU_UtilCreateDirectory(logs); // shuild makes a log's folder only with the system's separators
    SHU_UtilRunLogged(SHU_UtilFormat("%sconfigure.log", logs),
                      "cmake -S \"%sdependencies/%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=Release %s -DCMAKE_POSITION_INDEPENDENT_CODE=ON "
                      "-DCMAKE_C_VISIBILITY_PRESET=hidden -DCMAKE_CXX_VISIBILITY_PRESET=hidden -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON %s",
                      SHU_UtilGetScriptDirectory(), library->name, folder, compilers, library->options);
    SHU_UtilRunLogged(SHU_UtilFormat("%sbuild.log", logs), "cmake --build \"%s\" --parallel", folder);
    SHU_UtilLog(SHUM_LOG_NAME("%s") " (CMake): built", library->name);
}

/// @brief The C++ compiler's own static library, which C++ libraries need: libc++ for LLVM-MinGW's clang, libstdc++ elsewhere.
/// The C++ compiler gives its path, as OpenECS's C compiler may be another version than the C++ compiler that built the libraries.
static char *CppLibrary(void)
{
    const char *file = CONFIG.windows && SHU_CompilerGetFamily() == SHUCompiler_Clang ? "libc++.a" : "libstdc++.a";
    char *log = SHU_UtilFormat("%scmake/library.log", CONFIG.cache);
    SHU_UtilCreateDirectory(SHU_UtilFormat("%scmake/", CONFIG.cache));
    SHU_UtilRunLogged(log, "%s -print-file-name=%s", CompilerTool("g++", "clang++"), file);

    char path[1024] = {0};
    FILE *read = fopen(SHU_UtilFormat("%s%s", SHU_UtilGetScriptDirectory(), log), "r");

    bool given = read != NULL && fgets(path, sizeof(path), read) != NULL;

    if (read != NULL)
    {
        fclose(read);
    }

    if (!given)
    {
        SHU_UtilFail(1, "The C++ compiler does not give the path of " SHUM_LOG_VALUE("%s"), file);
    }
    path[strcspn(path, "\r\n")] = '\0';
    return SHU_UtilFormat("%s", path);
}

/// @brief The needs of a plugin, by its name; a plugin that compiles in no code of std's dependencies needs nothing, and keeps the analyzer.
static const PluginNeeds *FindNeeds(const char *name)
{
    static const PluginNeeds NOTHING = {.analyze = true};

    for (usz i = 0; i < COUNT(PLUGIN_NEEDS); i++)
    {
        if (strcmp(PLUGIN_NEEDS[i].name, name) == 0)
        {
            return &PLUGIN_NEEDS[i];
        }
    }

    return &NOTHING;
}

/// @brief The path from a folder back to this one: ../ for each folder in it.
static const char *PathBack(const char *folder)
{
    const char *back = "";

    for (const char *c = folder; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            back = SHU_UtilFormat("%s../", back);
        }
    }

    return back;
}

/// @brief Compiles the C files of a plugin's folder, root, into the plugin's native library in output. Does nothing if the folder has no C file.
/// The plugin sees OpenECS's plugin interface and SDL from the build's include/ folder, and, if it has needs, the folders of dependencies/ it compiles in, as system headers, so their warnings stay out.
static void BuildNative(const char *name, const char *root, const char *output)
{
    if (!HasCode(root))
    {
        return;
    }

    const PluginNeeds *needs = FindNeeds(name);

    // paths are named from the plugin's folder
    const char *up = PathBack(root);

    // the libraries it takes are compiled first, into static archives that its native library is linked with
    char *archives = SHU_UtilFormat("%sarchives/", CONFIG.cache);

    for (usz i = 0; needs->libraries[i] != NULL; i++)
    {
        BuildLibrary(needs->libraries[i], archives);
    }

    for (usz i = 0; needs->cmake[i] != NULL; i++)
    {
        BuildCMakeLibrary(needs->cmake[i]);
    }

    SHU_ModuleBegin(name, root);
    SetBuildFlags(true);

    // the static analyzer takes too much memory on the libraries that a plugin's C files include; the warnings and the sanitizers stay.
    // A plugin that takes its libraries only as archives keeps the analyzer, because its own files hold only their headers
    if (!needs->analyze && SHU_CompilerGetIdentifier() == SHUCompiler_GCC)
    {
        SHU_CompilerAddFlags(" -fno-analyzer");
    }

    for (usz i = 0; needs->includes[i] != NULL; i++)
    {
        char *flag = SHU_UtilFormat(" -isystem %sdependencies/%s", SHU_UtilGetScriptDirectory(), needs->includes[i]);
        SHU_CompilerAddFlags(flag);

        // shuild finds the headers a file includes in the module's folders, so the plugin is rebuilt when they change
        char *folder = SHU_UtilFormat("%sdependencies/%s", up, needs->includes[i]);
        SHU_ModuleAddIncludeDirectory(folder);
    }

    // a C++ library's headers are system headers too, those its build writes among them
    for (usz i = 0; needs->cmake[i] != NULL; i++)
    {
        for (usz j = 0; needs->cmake[i]->includes[j] != NULL; j++)
        {
            SHU_CompilerAddFlags(SHU_UtilFormat(" -isystem %sdependencies/%s", SHU_UtilGetScriptDirectory(), needs->cmake[i]->includes[j]));
        }

        for (usz j = 0; needs->cmake[i]->built[j] != NULL; j++)
        {
            SHU_CompilerAddFlags(SHU_UtilFormat(" -isystem %s%s", CMakeFolder(needs->cmake[i]), needs->cmake[i]->built[j]));
        }
    }

    if (needs->flags != NULL)
    {
        SHU_CompilerAddFlags(needs->flags);
    }

    char *include = SHU_UtilFormat("%s%sinclude/", up, CONFIG.output);
    SHU_ModuleAddSourceFile("./");
    SHU_ModuleAddIncludeDirectory(include);

    // stb_ds comes through OpenECS's glue header, stbSDL3.h, beside stb_ds.h in the build's include/stb/ (OpenECS DESIGN 1.7); a plugin compiles its
    // implementation in, so the folder is a system one, which keeps stb_ds's warnings out
    SHU_CompilerAddFlags(SHU_UtilFormat(" -isystem %s%sinclude/stb", SHU_UtilGetExecutablePath(), CONFIG.output));

    // the linker takes what the plugin uses of each library, in the order they are listed
    for (usz i = 0; needs->libraries[i] != NULL; i++)
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

        for (usz i = 0; needs->windowsLinks[i] != NULL; i++)
        {
            SHU_ModuleLinkLibrary(needs->windowsLinks[i]);
        }
    }

    // the archives of the C++ libraries go after the plugin's files, then the C++ compiler's own library and the C compiler's runtime, all compiled in;
    // on Linux, what they define stays inside the plugin, so it meets no other plugin's copy
    if (needs->cmake[0] != NULL)
    {
        for (usz i = 0; needs->cmake[i] != NULL; i++)
        {
            for (usz j = 0; needs->cmake[i]->archives[j] != NULL; j++)
            {
                SHU_CompilerAddLinkerFlags(SHU_UtilFormat(" \"%s%s\"", CMakeFolder(needs->cmake[i]), needs->cmake[i]->archives[j]));
            }
        }

        SHU_CompilerAddLinkerFlags(SHU_UtilFormat(" \"%s\" -static-libgcc", CppLibrary()));

        if (!CONFIG.windows)
        {
            SHU_CompilerAddLinkerFlags(" -Wl,--exclude-libs,ALL");
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
