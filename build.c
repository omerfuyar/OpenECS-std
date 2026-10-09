// The build of OpenECS's standard plugins. OpenECS's shuild.c includes this file when this repository is checked out in its std/ folder, and calls Shuild_Std.
// It uses what shuild.c defines before it: CONFIG, OUTPUT_DIRECTORY, ForEachFolder, CopyFile, Shuild_Plugin and Shuild_NativePlugin.

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
    // each folder of plugins/ is a standard plugin, built like OpenECS's first-party plugins into bin/plugins/
    ForEachFolder("std/plugins/", Shuild_Plugin);

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
