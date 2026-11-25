// The process standard plugin: runs programs with SDL's process functions, for terminals, build tools and linters (DESIGN 20).
// A started program is a handle; a plugin takes what its output and errors hold when it wants, such as from a timer, as net does, so nothing blocks the tool.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#if defined(SDL_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

/// @brief A program that runs, or ran.
typedef struct ProcessHandle
{
    SDL_Process *process;
    u8 *output;   // stb_ds array: what the last read took, kept until the next read
    u8 *errors;   // stb_ds array: what the last readErrors took, kept until the next readErrors
    u8 *unsent;   // stb_ds array: what write took that the program's input pipe had no room for yet, sent at each later call
    bool closing; // closeInput was called while bytes were unsent, so the input closes once they are sent
    bool exited;
    i32 code; // the exit code, once it exited
} ProcessHandle;

static struct
{
    ECSPlugin plugin;
    void *ran; // what the last run gave, kept until the next run
} PROCESS = {0};

/// @brief Fails a call, as ECS_Fail does, with SDL's error, and gives why.
static const char *ProcessFail(const char *what)
{
    const char *error = nullptr;
    ECS_Fail(PROCESS.plugin, &error, "%s: %s", what, SDL_GetError());
    return error;
}

/// @brief Makes the list of arguments that SDL takes from a list of texts, ending with nullptr. A list that is empty or holds something else is a misuse.
/// @return The list, to free with SDL_free, whose texts are the value's; nullptr if it is not made, with why in retError when memory runs out.
static const char **ProcessArguments(const ECSValue *command, const char **retError)
{
    usz count = ECSValue_GetListCount(command);
    bool texts = count > 0;

    for (usz i = 0; texts && i < count; i++)
    {
        texts = ECSValue_GetType(ECSValue_GetListItem(command, i)) == ECSValueType_String;
    }

    if (!ECS_Check(PROCESS.plugin, texts, "The command is a list of texts: the program, then its arguments."))
    {
        return nullptr;
    }

    const char **arguments = SDL_calloc(count + 1, sizeof(char *));

    if (arguments == nullptr)
    {
        ECS_Fail(PROCESS.plugin, retError, "out of memory");
        return nullptr;
    }

    for (usz i = 0; i < count; i++)
    {
        arguments[i] = ECSValue_GetString(ECSValue_GetListItem(command, i), "");
    }

    return arguments;
}

/// @brief Makes the environment of a program: the tool's, with each NAME=VALUE text of a list set in it. A list that holds something else is a misuse.
/// @return The environment, to destroy with SDL_DestroyEnvironment; nullptr for an empty list, which keeps the tool's, and with retValid false when
/// it is not made, with why in retError when memory runs out.
static SDL_Environment *ProcessEnvironment(const ECSValue *environment, bool *retValid, const char **retError)
{
    usz count = ECSValue_GetListCount(environment);
    *retValid = true;

    for (usz i = 0; *retValid && i < count; i++)
    {
        const ECSValue *item = ECSValue_GetListItem(environment, i);
        const char *text = ECSValue_GetString(item, "");
        *retValid = ECSValue_GetType(item) == ECSValueType_String && text[0] != '=' && SDL_strchr(text, '=') != nullptr;
    }

    if (!ECS_Check(PROCESS.plugin, *retValid, "The environment is a list of texts, each NAME=VALUE."))
    {
        return nullptr;
    }

    if (count == 0)
    {
        return nullptr;
    }

    SDL_Environment *made = SDL_CreateEnvironment(true);

    for (usz i = 0; made != nullptr && i < count; i++)
    {
        const char *text = ECSValue_GetString(ECSValue_GetListItem(environment, i), "");
        const char *equals = SDL_strchr(text, '=');
        char *name = SDL_strndup(text, (size_t)(equals - text));

        if (name == nullptr || !SDL_SetEnvironmentVariable(made, name, equals + 1, true))
        {
            SDL_DestroyEnvironment(made);
            made = nullptr;
        }

        SDL_free(name);
    }

    if (made == nullptr)
    {
        *retValid = false;
        ECS_Fail(PROCESS.plugin, retError, "out of memory");
    }

    return made;
}

/// @brief Creates a program's process: its arguments, its folder unless it is empty, its environment unless it is nullptr, and how its input and
/// output go.
static SDL_Process *ProcessCreate(const char **arguments, const char *folder, SDL_Environment *environment, bool input, bool errorsToOutput)
{
    SDL_PropertiesID properties = SDL_CreateProperties();

    if (environment != nullptr)
    {
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    }

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

/// @brief Closes a program's input, which tells a program that reads it to the end that it ended.
static void ProcessCloseStream(SDL_Process *process)
{
    SDL_IOStream *input = SDL_GetProcessInput(process);

    if (input != nullptr)
    {
        SDL_CloseIO(input);
        SDL_SetPointerProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_STDIN_POINTER, nullptr);
    }
}

#if defined(SDL_PLATFORM_WINDOWS)
/// @brief How much one write gives a Windows pipe: a pipe that does not wait takes nothing of a write larger than its free room.
#define PROCESS_PIPE_CHUNK 512
#endif

/// @brief Writes into a program's input what its pipe takes now, without waiting, as SDL's stream writes only that.
/// @return The bytes written; fewer than given when the pipe is full, or when it broke, which retBroken tells.
static usz ProcessSend(SDL_IOStream *input, const u8 *data, usz size, bool *retBroken)
{
#if defined(SDL_PLATFORM_WINDOWS)
    // the pipe's handle takes small writes itself: SDL's stream keeps a small write in its buffer, which it drops when a full pipe takes none of it
    HANDLE pipe = SDL_GetPointerProperty(SDL_GetIOProperties(input), SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, nullptr);
    usz sent = 0;
    *retBroken = pipe == nullptr;

    while (!*retBroken && sent < size)
    {
        DWORD written = 0;

        if (!WriteFile(pipe, data + sent, (DWORD)SDL_min(size - sent, PROCESS_PIPE_CHUNK), &written, nullptr))
        {
            *retBroken = true;
        }
        else if (written == 0)
        {
            break;
        }

        sent += written;
    }

    return sent;
#else
    usz written = SDL_WriteIO(input, data, size);
    *retBroken = written < size && SDL_GetIOStatus(input) == SDL_IO_STATUS_ERROR;
    return written;
#endif
}

/// @brief Sends the bytes that write kept, as far as the pipe takes them, and closes the input once they are sent if closeInput asked for it.
/// @return false if the input broke, as when the program ended; the bytes that were not sent are dropped then.
static bool ProcessSendUnsent(ProcessHandle *handle)
{
    SDL_IOStream *input = SDL_GetProcessInput(handle->process);
    bool broken = false;

    if (input != nullptr && arrlenu(handle->unsent) > 0)
    {
        usz written = ProcessSend(input, handle->unsent, arrlenu(handle->unsent), &broken);
        arrdeln(handle->unsent, 0, written);
    }

    if (broken)
    {
        arrsetlen(handle->unsent, 0);
    }

    if (handle->closing && arrlenu(handle->unsent) == 0)
    {
        ProcessCloseStream(handle->process);
        handle->closing = false;
    }

    return !broken;
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
    arrfree(handle->unsent);
    SDL_free(handle);
}

static ProcessHandle *ProcessStart(const ECSValue *command, const char *folder, const ECSValue *environment, const char **retError)
{
    *retError = nullptr;
    bool valid = false;
    SDL_Environment *variables = ProcessEnvironment(environment, &valid, retError);
    const char **arguments = valid ? ProcessArguments(command, retError) : nullptr;

    if (arguments == nullptr)
    {
        SDL_DestroyEnvironment(variables);
        return nullptr;
    }

    ProcessHandle *handle = SDL_calloc(1, sizeof(ProcessHandle));

    if (handle != nullptr)
    {
        handle->process = ProcessCreate(arguments, folder, variables, true, false);
    }

    SDL_free((void *)arguments);
    SDL_DestroyEnvironment(variables);

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
    (void)ProcessSendUnsent(handle);
    return ProcessTake(SDL_GetProcessOutput(handle->process), &handle->output);
}

static SHUSlice ProcessReadErrors(ProcessHandle *handle)
{
    (void)ProcessSendUnsent(handle);
    return ProcessTake(SDL_GetPointerProperty(SDL_GetProcessProperties(handle->process), SDL_PROP_PROCESS_STDERR_POINTER, nullptr), &handle->errors);
}

static bool ProcessWrite(ProcessHandle *handle, SHUSlice data, const char **retError)
{
    *retError = nullptr;
    SDL_IOStream *input = SDL_GetProcessInput(handle->process);

    if (!ECS_Check(PROCESS.plugin, input != nullptr && !handle->closing, "The program's input is closed."))
    {
        return false;
    }

    // what the pipe has no room for waits in the handle, after what waits already, and goes at the next calls
    if (data.size > 0)
    {
        SDL_memcpy(arraddnptr(handle->unsent, data.size), data.data, data.size);
    }

    if (!ProcessSendUnsent(handle))
    {
        *retError = ProcessFail("the input cannot be written");
        return false;
    }

    return true;
}

// a program that reads its input to the end, such as sort, needs it closed; bytes that wait are sent first
static void ProcessCloseInput(ProcessHandle *handle)
{
    handle->closing = true;
    (void)ProcessSendUnsent(handle);
}

static bool ProcessExited(ProcessHandle *handle, i32 *retCode)
{
    (void)ProcessSendUnsent(handle);

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

static SHUSlice ProcessRun(const ECSValue *command, const char *folder, const ECSValue *environment, SHUSlice input, i32 *retCode, const char **retError)
{
    *retCode = -1;
    *retError = nullptr;
    SDL_free(PROCESS.ran);
    PROCESS.ran = nullptr;
    bool valid = false;
    SDL_Environment *variables = ProcessEnvironment(environment, &valid, retError);
    const char **arguments = valid ? ProcessArguments(command, retError) : nullptr;

    if (arguments == nullptr)
    {
        SDL_DestroyEnvironment(variables);
        return cs(nullptr, 0);
    }

    SDL_Process *process = ProcessCreate(arguments, folder, variables, input.size > 0, true);
    SDL_free((void *)arguments);
    SDL_DestroyEnvironment(variables);

    if (process == nullptr)
    {
        *retError = ProcessFail("the program cannot start");
        return cs(nullptr, 0);
    }

    // the pipe takes only part of a large input, and the program may wait for its output to be read before it reads more,
    // so the input goes while the output is read; then the rest of the output is read until the program exits
    u8 *output = nullptr;
    SDL_IOStream *stream = SDL_GetProcessInput(process);
    u8 *taken = nullptr;
    bool broken = false;

    for (usz sent = 0; stream != nullptr && sent < input.size && !broken;)
    {
        usz written = ProcessSend(stream, (const u8 *)input.data + sent, input.size - sent, &broken);
        sent += written;
        usz took = ProcessTake(SDL_GetProcessOutput(process), &taken).size;

        if (took > 0)
        {
            SDL_memcpy(arraddnptr(output, took), taken, took);
        }

        if (written == 0 && took == 0)
        {
            SDL_Delay(1);
        }
    }

    arrfree(taken);
    ProcessCloseStream(process);
    usz size = 0;
    int code = -1;
    void *rest = SDL_ReadProcess(process, &size, &code);
    SDL_DestroyProcess(process);
    *retCode = code;

    if (rest == nullptr)
    {
        arrfree(output);
        *retError = ProcessFail("the program's output cannot be read");
        return cs(nullptr, 0);
    }

    PROCESS.ran = SDL_malloc(arrlenu(output) + size + 1);

    if (PROCESS.ran == nullptr)
    {
        arrfree(output);
        SDL_free(rest);
        *retError = ProcessFail("out of memory");
        return cs(nullptr, 0);
    }

    if (output != nullptr)
    {
        SDL_memcpy(PROCESS.ran, output, arrlenu(output));
    }

    SDL_memcpy((u8 *)PROCESS.ran + arrlenu(output), rest, size);
    usz length = arrlenu(output) + size;
    ((u8 *)PROCESS.ran)[length] = 0;
    arrfree(output);
    SDL_free(rest);
    return cs(PROCESS.ran, length);
}

static void ProcessFree(void *data)
{
    (void)data;
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
        {"start", (ECSFunction)ProcessStart, "handle<process.process>(value<string[]> command, string folder, value<string[]> environment, out string error)",
         "Start a program; it runs beside the tool, and its output and errors wait until they are read\n"
         "@param command The program, found on PATH unless it is a path, then its arguments, such as { \"git\", \"status\" }\n"
         "@param folder The folder it runs in; empty for the tool's\n"
         "@param environment Variables set for it, each NAME=VALUE, beside the tool's own; empty for the tool's alone\n"
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
        {"write", (ECSFunction)ProcessWrite, "bool(handle<process.process> process, buffer data, out string error)",
         "Write to the program's input, which must not be closed\n"
         "@param process The program\n"
         "@param data What to write\n"
         "@param error Why it cannot be written, as when the program ended; nothing if it is\n"
         "@return True if it is written"},
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
        {"run", (ECSFunction)ProcessRun, "buffer(value<string[]> command, string folder, value<string[]> environment, buffer input, out int code, out string error)",
         "Run a program to its end and give its output and errors together; the tool waits for it, so it suits short programs\n"
         "@param command The program, found on PATH unless it is a path, then its arguments\n"
         "@param folder The folder it runs in; empty for the tool's\n"
         "@param environment Variables set for it, each NAME=VALUE, beside the tool's own; empty for the tool's alone\n"
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
