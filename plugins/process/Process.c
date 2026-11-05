// The process standard plugin: runs programs with SDL's process functions, for terminals, build tools and linters (DESIGN 20).
// A started program is a handle; a plugin takes what its output and errors hold when it wants, such as from a timer, as net does, so nothing blocks the tool.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

/// @brief A program that runs, or ran.
typedef struct ProcessHandle
{
    SDL_Process *process;
    u8 *output; // stb_ds array: what the last read took, kept until the next read
    u8 *errors; // stb_ds array: what the last readErrors took, kept until the next readErrors
    bool exited;
    i32 code; // the exit code, once it exited
} ProcessHandle;

static struct
{
    ECSPlugin plugin;
    char *error; // why the last call failed, kept until the next one
    void *ran;   // what the last run gave, kept until the next run
} PROCESS = {0};

/// @brief Keeps why a call failed, with SDL's error, and gives it.
static const char *ProcessFail(const char *what)
{
    SDL_free(PROCESS.error);
    PROCESS.error = nullptr;

    if (SDL_asprintf(&PROCESS.error, "%s: %s", what, SDL_GetError()) < 0)
    {
        PROCESS.error = nullptr;
    }

    return PROCESS.error == nullptr ? "out of memory" : PROCESS.error;
}

/// @brief Makes the list of arguments that SDL takes from a list of texts, ending with nullptr; nullptr if the list is empty or holds something else.
/// @return The list; free it with SDL_free. Its texts are the value's.
static const char **ProcessArguments(const ECSValue *command)
{
    usz count = ECSValue_GetListCount(command);
    const char **arguments = count == 0 ? nullptr : SDL_calloc(count + 1, sizeof(char *));

    for (usz i = 0; arguments != nullptr && i < count; i++)
    {
        arguments[i] = ECSValue_GetString(ECSValue_GetListItem(command, i), nullptr);

        if (arguments[i] == nullptr)
        {
            SDL_free((void *)arguments);
            return nullptr;
        }
    }

    return arguments;
}

/// @brief Creates a program's process: its arguments, its folder unless it is empty, and how its input and output go.
static SDL_Process *ProcessCreate(const char **arguments, const char *folder, bool input, bool errorsToOutput)
{
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)arguments);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, input ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);

    // SDL sends the errors into the output only when their own option is not set
    if (errorsToOutput)
    {
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    }
    else
    {
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_APP);
    }

    if (folder[0] != '\0')
    {
        SDL_SetStringProperty(properties, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, folder);
    }

    SDL_Process *process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    return process;
}

/// @brief Takes everything a stream of a program holds now, without waiting, into a buffer that it replaces.
static SHUSlice ProcessTake(SDL_IOStream *stream, u8 **buffer)
{
    arrsetlen(*buffer, 0);
    u8 chunk[4096];

    for (usz read = 1; stream != nullptr && read > 0;)
    {
        read = SDL_ReadIO(stream, chunk, sizeof(chunk));

        if (read > 0)
        {
            SDL_memcpy(arraddnptr(*buffer, read), chunk, read);
        }
    }

    return cs(*buffer, arrlenu(*buffer));
}

/// @brief Frees a program's handle; a program that still runs is killed first, so none is left behind.
static void ProcessDestroy(void *object)
{
    ProcessHandle *handle = object;

    if (!handle->exited && SDL_KillProcess(handle->process, true))
    {
        SDL_WaitProcess(handle->process, true, nullptr);
    }

    SDL_DestroyProcess(handle->process);
    arrfree(handle->output);
    arrfree(handle->errors);
    SDL_free(handle);
}

static ProcessHandle *ProcessStart(const ECSValue *command, const char *folder, const char **retError)
{
    *retError = nullptr;
    const char **arguments = ProcessArguments(command);

    if (arguments == nullptr)
    {
        *retError = "the command is a list of texts: the program, then its arguments";
        return nullptr;
    }

    ProcessHandle *handle = SDL_calloc(1, sizeof(ProcessHandle));

    if (handle != nullptr)
    {
        handle->process = ProcessCreate(arguments, folder, true, false);
    }

    SDL_free((void *)arguments);

    if (handle == nullptr || handle->process == nullptr)
    {
        *retError = ProcessFail(handle == nullptr ? "out of memory" : "the program cannot start");
        SDL_free(handle);
        return nullptr;
    }

    return handle;
}

static SHUSlice ProcessRead(ProcessHandle *handle)
{
    return ProcessTake(SDL_GetProcessOutput(handle->process), &handle->output);
}

static SHUSlice ProcessReadErrors(ProcessHandle *handle)
{
    return ProcessTake(SDL_GetPointerProperty(SDL_GetProcessProperties(handle->process), SDL_PROP_PROCESS_STDERR_POINTER, nullptr), &handle->errors);
}

static bool ProcessWrite(ProcessHandle *handle, SHUSlice data)
{
    SDL_IOStream *input = SDL_GetProcessInput(handle->process);
    return input != nullptr && SDL_WriteIO(input, data.data, data.size) == data.size;
}

// a program that reads its input to the end, such as sort, needs it closed
static void ProcessCloseInput(ProcessHandle *handle)
{
    SDL_IOStream *input = SDL_GetProcessInput(handle->process);

    if (input != nullptr)
    {
        SDL_CloseIO(input);
        SDL_SetPointerProperty(SDL_GetProcessProperties(handle->process), SDL_PROP_PROCESS_STDIN_POINTER, nullptr);
    }
}

static bool ProcessExited(ProcessHandle *handle, i32 *retCode)
{
    if (!handle->exited)
    {
        int code = 0;
        handle->exited = SDL_WaitProcess(handle->process, false, &code);
        handle->code = code;
    }

    *retCode = handle->exited ? handle->code : 0;
    return handle->exited;
}

static void ProcessKill(ProcessHandle *handle, bool force)
{
    if (!handle->exited)
    {
        SDL_KillProcess(handle->process, force);
    }
}

static SHUSlice ProcessRun(const ECSValue *command, const char *folder, SHUSlice input, i32 *retCode, const char **retError)
{
    *retCode = -1;
    *retError = nullptr;
    SDL_free(PROCESS.ran);
    PROCESS.ran = nullptr;
    const char **arguments = ProcessArguments(command);

    if (arguments == nullptr)
    {
        *retError = "the command is a list of texts: the program, then its arguments";
        return cs(nullptr, 0);
    }

    SDL_Process *process = ProcessCreate(arguments, folder, input.size > 0, true);
    SDL_free((void *)arguments);

    if (process == nullptr)
    {
        *retError = ProcessFail("the program cannot start");
        return cs(nullptr, 0);
    }

    // the input goes first, then the program's output and errors are read until it exits
    SDL_IOStream *stream = SDL_GetProcessInput(process);

    if (stream != nullptr)
    {
        SDL_WriteIO(stream, input.data, input.size);
        SDL_CloseIO(stream);
        SDL_SetPointerProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_STDIN_POINTER, nullptr);
    }

    usz size = 0;
    int code = -1;
    PROCESS.ran = SDL_ReadProcess(process, &size, &code);
    SDL_DestroyProcess(process);
    *retCode = code;

    if (PROCESS.ran == nullptr)
    {
        *retError = ProcessFail("the program's output cannot be read");
        return cs(nullptr, 0);
    }

    return cs(PROCESS.ran, size);
}

static void ProcessFree(void *data)
{
    (void)data;
    SDL_free(PROCESS.error);
    SDL_free(PROCESS.ran);
    SDL_zero(PROCESS);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    PROCESS.plugin = plugin;
    const ECSPluginDesc desc = {.Destroy = ProcessFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "process", ProcessDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"start", (ECSFunction)ProcessStart, "handle<process.process>(value<string[]> command, string folder, out string error)",
         "Start a program; it runs beside the tool, and its output and errors wait until they are read\n"
         "@param command The program, found on PATH unless it is a path, then its arguments, such as { \"git\", \"status\" }\n"
         "@param folder The folder it runs in; empty for the tool's\n"
         "@param error Why it cannot start; nothing if it starts\n"
         "@return The program; nothing if it cannot start. A program whose handle is gone is killed"},
        {"read", (ECSFunction)ProcessRead, "buffer(handle<process.process> process)",
         "Take what the program wrote to its output since the last read, without waiting\n"
         "@param process The program\n"
         "@return The output, kept until the next read; empty if nothing came"},
        {"readErrors", (ECSFunction)ProcessReadErrors, "buffer(handle<process.process> process)",
         "Take what the program wrote to its errors since the last readErrors, without waiting\n"
         "@param process The program\n"
         "@return The errors, kept until the next readErrors; empty if nothing came"},
        {"write", (ECSFunction)ProcessWrite, "bool(handle<process.process> process, buffer data)",
         "Write to the program's input\n"
         "@param process The program\n"
         "@param data What to write\n"
         "@return false if its input is closed"},
        {"closeInput", (ECSFunction)ProcessCloseInput, "void(handle<process.process> process)",
         "Close the program's input, for a program that reads its input to the end\n"
         "@param process The program"},
        {"exited", (ECSFunction)ProcessExited, "bool(handle<process.process> process, out int code)",
         "Tell whether the program exited, without waiting\n"
         "@param process The program\n"
         "@param code Its exit code once it exited, negative for a signal on Linux; 0 while it runs\n"
         "@return true once it exited"},
        {"kill", (ECSFunction)ProcessKill, "void(handle<process.process> process, bool force)",
         "Ask the program to stop, or stop it at once\n"
         "@param process The program\n"
         "@param force true to stop it at once, false to ask it"},
        {"run", (ECSFunction)ProcessRun, "buffer(value<string[]> command, string folder, buffer input, out int code, out string error)",
         "Run a program to its end and give its output and errors together; the tool waits for it, so it suits short programs\n"
         "@param command The program, found on PATH unless it is a path, then its arguments\n"
         "@param folder The folder it runs in; empty for the tool's\n"
         "@param input What the program reads as its input, which is closed after it; empty for none\n"
         "@param code Its exit code; -1 if it cannot start\n"
         "@param error Why it cannot run; nothing if it ran\n"
         "@return Its output and errors, kept until the next run; nothing if it cannot run"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a handle, such as program:read(); C calls the functions themselves (OpenECS DESIGN 10.6)
    const char *methods[] = {"read", "readErrors", "write", "closeInput", "exited", "kill"};

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, "process", methods[i], methods[i]));
    }

    return SHUResult_Ok;
}
