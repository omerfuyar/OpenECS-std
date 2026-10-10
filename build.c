// The build of OpenECS's standard plugins. OpenECS's shuild.c includes this file when this repository is checked out in its std/ folder, and calls Shuild_Std.
// It uses what shuild.c defines before it: CONFIG, OUTPUT_DIRECTORY, BUILD_DIRECTORY, BuildType_String, IsBuilt, SetBuildFlags, ForEachFolder, Shuild_Plugin and Shuild_NativePlugin.

/// @brief A plugin that needs more than the plugin interface: headers of std's dependencies, and libraries to link.
typedef struct StdPluginNeeds
{
    const char *name;
    const char *includes[4];  // folders of dependencies/, ending with NULL
    const char *libraries[4]; // libraries in the build's lib/ folder, ending with NULL
} StdPluginNeeds;

static const StdPluginNeeds STD_PLUGIN_NEEDS[] = {
    {"image", {"stb", "nanosvg/src", NULL}, {NULL}},
    {"audio", {"dr_libs", "stb", NULL}, {NULL}},
    {"net", {NULL}, {"SDL3_net", NULL}},
    {"gltf", {"cgltf", NULL}, {NULL}},
};

/// @brief Builds SDL3_net as a shared library into the build's lib/ folder, as OpenECS builds SDL3_ttf.
static void Shuild_StdSDL_net(void)
{
    SHU_LogInfo("Starting to build " SHUM_COLOR_MAGENTA("'SDL3_net'") "...");

    const char *root = SHU_UtilGetExecutablePath();
    SHUI_String sourceDir;
    SHUI_String buildDir;
    SHUI_String outputPrefixDir;
    SHUI_SFormat(&sourceDir, "%sstd/dependencies/SDL_net/", root);
    SHUI_SFormat(&buildDir, "%s%sSDL3_net/", root, BUILD_DIRECTORY.data);
    SHUI_SFormat(&outputPrefixDir, "%s%s/", root, OUTPUT_DIRECTORY.data);

    SHU_UtilRun(
        "cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s "
        "-DCMAKE_INSTALL_PREFIX=\"%s\" -DCMAKE_PREFIX_PATH=\"%s\" "
        "-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "
        "-DBUILD_SHARED_LIBS=ON -DSDLNET_INSTALL=ON -DSDLNET_SAMPLES=OFF "
        "--log-level=WARNING",
        sourceDir.data, buildDir.data, BuildType_String(CONFIG.type),
        outputPrefixDir.data, outputPrefixDir.data);

    SHU_UtilRun("cmake --build \"%s\" --parallel > /dev/null", buildDir.data);
    SHU_UtilRun("cmake --install \"%s\" > /dev/null", buildDir.data);

    SHU_LogInfo("Done building " SHUM_COLOR_MAGENTA("'SDL3_net'") "\n");
}

/// @brief Builds a plugin's native library with its needs: like Shuild_NativePlugin, with the dependencies' headers as system headers, so their warnings stay out, and libraries found next to the executable.
/// Compiler flags are not paths of the module, so the headers' folders are full paths.
static void Shuild_StdNativePlugin(const StdPluginNeeds *needs, const char *root, const char *output)
{
    // paths are relative to the plugin's folder, so they climb one level for each folder in root
    SHUI_String up = {0};

    for (const char *c = root; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            SHUI_SAppendC(&up, "../");
        }
    }

    SHUI_String include;
    SHUI_SFormat(&include, "%s%sinclude/", up.data, OUTPUT_DIRECTORY.data);

    SHU_ModuleBegin(needs->name, root);
    SetBuildFlags(true);
    SHU_CompilerAddFlags(" -fvisibility=hidden '-Wl,-rpath,$ORIGIN/../..'");

    // the plugin compiles its dependencies' code into itself, which the static analyzer takes too much memory for; warnings and sanitizers stay
    SHU_CompilerAddFlags(" -fno-analyzer");

    for (usz i = 0; needs->includes[i] != NULL; i++)
    {
        SHUI_String flag;
        SHUI_SFormat(&flag, " -isystem %sstd/dependencies/%s", SHU_UtilGetExecutablePath(), needs->includes[i]);
        SHU_CompilerAddFlags(flag.data);
    }

    // shuild links a shared library from its objects alone, so a plugin that links a library is compiled and linked in one command, with shuild's flags
    if (needs->libraries[0] != NULL)
    {
        char flags[SHUC_MAX_COMMAND_BUFFER_SIZE];
        SHU_CompilerGetFlags((SHUSlice){.data = flags, .size = sizeof(flags)});

        SHUI_String libraries;
        SHUI_SFormat(&libraries, " -L%slib/", OUTPUT_DIRECTORY.data);

        for (usz i = 0; needs->libraries[i] != NULL; i++)
        {
            SHUI_SAppendC(&libraries, " -l");
            SHUI_SAppendC(&libraries, needs->libraries[i]);
        }

        SHU_UtilRun("gcc -shared -fPIC %s -I%sinclude/ -o %slib%s.so %s*.c%s", flags, OUTPUT_DIRECTORY.data, output, needs->name, root, libraries.data);
        return;
    }

    SHU_ModuleAddSourceFile("./");
    SHU_ModuleAddIncludeDirectory(include.data);

    for (usz i = 0; needs->includes[i] != NULL; i++)
    {
        SHUI_String folder;
        SHUI_SFormat(&folder, "%sstd/dependencies/%s", up.data, needs->includes[i]);
        SHU_ModuleAddIncludeDirectory(folder.data);
    }

    SHU_ModuleCompile(output, SHUModuleType_LibraryDynamic);
}

/// @brief Builds a standard plugin: one with needs on its own, the others as OpenECS builds first-party plugins.
static void Shuild_StdPlugin(const char *path, const char *name)
{
    for (usz i = 0; i < sizeof(STD_PLUGIN_NEEDS) / sizeof(*STD_PLUGIN_NEEDS); i++)
    {
        if (strcmp(STD_PLUGIN_NEEDS[i].name, name) == 0)
        {
            SHUI_String root;
            SHUI_String output;
            SHUI_SFormat(&root, "%s%s/", path, name);
            SHUI_SFormat(&output, "%sbin/plugins/%s/", OUTPUT_DIRECTORY.data, name);
            SHU_UtilCreateDirectory(output.data);

            // its Lua files, the manifest among them, and its folders are copied, as Shuild_Plugin does
            SHU_UtilRun("find %s -mindepth 1 -maxdepth 1 \\( -name '*.lua' -o -type d \\) -exec cp -r {} %s \\;", root.data, output.data);
            Shuild_StdNativePlugin(&STD_PLUGIN_NEEDS[i], root.data, output.data);
            return;
        }
    }

    Shuild_Plugin(path, name);
}

/// @brief Builds the native plugin of a test plugin folder in its copy beside bin/, as OpenECS's own test plugins are.
static void Shuild_StdTestPlugin(const char *path, const char *name)
{
    SHUI_String root;
    SHUI_String output;
    SHUI_SFormat(&root, "%s%s/", path, name);
    SHUI_SFormat(&output, "%stests/plugins/%s/", OUTPUT_DIRECTORY.data, name);
    Shuild_NativePlugin(name, root.data, output.data);
}

static void Shuild_Std(void)
{
    // the libraries that plugins link are built once, and go next to the executable, as OpenECS's SDL libraries do
    if (!IsBuilt("SDL3_net", true))
    {
        Shuild_StdSDL_net();
    }

    SHU_UtilRun("cp -L %slib/libSDL3_net.so.0 %sbin/", OUTPUT_DIRECTORY.data, OUTPUT_DIRECTORY.data);

    // each folder of plugins/ is a standard plugin, built into bin/plugins/
    ForEachFolder("std/plugins/", Shuild_StdPlugin);

    // the presets go to bin/presets/, beside OpenECS's own
    SHUI_String presets;
    SHUI_SFormat(&presets, "%sbin/presets/", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(presets.data);
    SHU_UtilRun("cp -r std/presets/. %s", presets.data);

    // the tests join OpenECS's tests beside bin/, so the same runner runs them
    if (CONFIG.tests)
    {
        SHU_UtilRun("mkdir -p %stests && cp -r std/tests/. %stests/", OUTPUT_DIRECTORY.data, OUTPUT_DIRECTORY.data);
        ForEachFolder("std/tests/plugins/", Shuild_StdTestPlugin);
    }
}
