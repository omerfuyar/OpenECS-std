// The gamepad standard plugin: game controllers through SDL's gamepad functions, for games, emulators and tools driven by a controller (DESIGN 21).
// A plugin reads a gamepad's buttons and axes when it wants, or takes them as events. SDL tells the changes as it reads the devices, in the main loop,
// and an event watch turns them into this plugin's events, so nothing polls.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief A gamepad that a plugin opened.
typedef struct GamepadHandle
{
    SDL_Gamepad *gamepad;
} GamepadHandle;

/// @brief A virtual gamepad: SDL shows it as a gamepad that is plugged in, and the plugin presses its buttons and moves its axes.
typedef struct GamepadVirtual
{
    SDL_JoystickID id;
    SDL_Joystick *joystick;
} GamepadVirtual;

static struct
{
    ECSPlugin plugin;
    bool sdlStarted;
} GAMEPAD = {0};

/// @brief Fails a call, as ECS_Fail does, with SDL's error, and gives why.
static const char *GamepadFail(const char *what)
{
    const char *error = nullptr;
    ECS_Fail(GAMEPAD.plugin, &error, "%s: %s", what, SDL_GetError());
    return error;
}

/// @brief An axis's value from -1 to 1, or 0 to 1 for a trigger.
static f32 GamepadAxisValue(i16 value)
{
    return SDL_max((f32)value / (f32)SDL_JOYSTICK_AXIS_MAX, -1.0F);
}

/// @brief Sets a field of a table, and gives it; nullptr if there is no memory.
static ECSValue *GamepadField(ECSValue *table, const char *name)
{
    ECSValue *field = nullptr;
    return ECSValue_SetTableField(table, name, &field) ? nullptr : field;
}

/// @brief Emits an event of a gamepad with its id, and with a name and a number or a bool, or a text.
static void GamepadEmit(const char *event, SDL_JoystickID id, const char *name, const char *text, const ECSValue *extra)
{
    ECSValue *value = nullptr;

    if (ECSValue_Create(&value))
    {
        return;
    }

    ECSValue_SetTable(value);
    ECSValue *field = GamepadField(value, "id");
    bool filled = field != nullptr;

    if (filled)
    {
        ECSValue_SetInteger(field, id);
    }

    if (filled && text != nullptr)
    {
        field = GamepadField(value, name);
        filled = field != nullptr && ECSValue_SetString(field, text) == SHUResult_Ok;
    }

    if (filled && extra != nullptr)
    {
        field = GamepadField(value, ECSValue_GetType(extra) == ECSValueType_Bool ? "down" : "value");
        filled = field != nullptr;

        if (filled && ECSValue_GetType(extra) == ECSValueType_Bool)
        {
            ECSValue_SetBool(field, ECSValue_GetBool(extra, false));
        }
        else if (filled)
        {
            ECSValue_SetNumber(field, ECSValue_GetNumber(extra, 0.0));
        }
    }

    if (!filled || ECSEvent_Emit(GAMEPAD.plugin, event, value))
    {
        ECS_LogWarn(GAMEPAD.plugin, "'%s' cannot be emitted.", event);
    }

    ECSValue_Destroy(&value);
}

// SDL reads the devices in the main loop and pushes their events there; events from other threads, such as an application's own, are not gamepads'
static bool GamepadWatch(void *data, SDL_Event *event)
{
    (void)data;

    if (!SDL_IsMainThread())
    {
        return true;
    }

    ECSValue *extra = nullptr;

    if (ECSValue_Create(&extra))
    {
        return true;
    }

    switch (event->type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
        GamepadEmit("gamepad.added", event->gdevice.which, "name", SDL_GetGamepadNameForID(event->gdevice.which), nullptr);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        GamepadEmit("gamepad.removed", event->gdevice.which, nullptr, nullptr, nullptr);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        ECSValue_SetBool(extra, event->gbutton.down);
        GamepadEmit("gamepad.button", event->gbutton.which, "button", SDL_GetGamepadStringForButton((SDL_GamepadButton)event->gbutton.button), extra);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        ECSValue_SetNumber(extra, GamepadAxisValue(event->gaxis.value));
        GamepadEmit("gamepad.axis", event->gaxis.which, "axis", SDL_GetGamepadStringForAxis((SDL_GamepadAxis)event->gaxis.axis), extra);
        break;
    default:
        break;
    }

    ECSValue_Destroy(&extra);
    return true;
}

static void GamepadDestroy(void *object)
{
    GamepadHandle *handle = object;
    SDL_CloseGamepad(handle->gamepad);
    SDL_free(handle);
}

static void GamepadVirtualDestroy(void *object)
{
    GamepadVirtual *handle = object;
    SDL_CloseJoystick(handle->joystick);
    SDL_DetachVirtualJoystick(handle->id);
    SDL_free(handle);
}

static void GamepadList(ECSValue *retGamepads)
{
    ECSValue_SetTable(retGamepads);
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);

    for (int i = 0; ids != nullptr && i < count; i++)
    {
        const char *name = SDL_GetGamepadNameForID(ids[i]);
        const char *type = SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(ids[i]));
        ECSValue *item = nullptr;

        if (ECSValue_AddListItem(retGamepads, &item))
        {
            break;
        }

        ECSValue_SetTable(item);
        ECSValue *field = GamepadField(item, "id");

        if (field == nullptr)
        {
            break;
        }

        ECSValue_SetInteger(field, ids[i]);

        if ((field = GamepadField(item, "name")) == nullptr || ECSValue_SetString(field, name == nullptr ? "" : name) || (field = GamepadField(item, "type")) == nullptr ||
            ECSValue_SetString(field, type == nullptr ? "unknown" : type))
        {
            break;
        }
    }

    SDL_free(ids);
}

static GamepadHandle *GamepadOpen(i32 id, const char **retError)
{
    *retError = nullptr;
    GamepadHandle *handle = SDL_calloc(1, sizeof(GamepadHandle));

    if (handle == nullptr || (handle->gamepad = SDL_OpenGamepad((SDL_JoystickID)id)) == nullptr)
    {
        *retError = GamepadFail(handle == nullptr ? "out of memory" : "the gamepad cannot open");
        SDL_free(handle);
        return nullptr;
    }

    return handle;
}

static i32 GamepadId(GamepadHandle *handle)
{
    return (i32)SDL_GetGamepadID(handle->gamepad);
}

static const char *GamepadName(GamepadHandle *handle)
{
    const char *name = SDL_GetGamepadName(handle->gamepad);
    return name == nullptr ? "" : name;
}

static bool GamepadConnected(GamepadHandle *handle)
{
    return SDL_GamepadConnected(handle->gamepad);
}

/// @brief Gives a button by SDL's name of it; a name that is not a button is a misuse.
static SDL_GamepadButton GamepadFindButton(const char *button)
{
    SDL_GamepadButton index = SDL_GetGamepadButtonFromString(button);
    (void)ECS_Check(GAMEPAD.plugin, index != SDL_GAMEPAD_BUTTON_INVALID, "'%s' is not a gamepad's button.", button);
    return index;
}

/// @brief Gives an axis by SDL's name of it; a name that is not an axis is a misuse.
static SDL_GamepadAxis GamepadFindAxis(const char *axis)
{
    SDL_GamepadAxis index = SDL_GetGamepadAxisFromString(axis);
    (void)ECS_Check(GAMEPAD.plugin, index != SDL_GAMEPAD_AXIS_INVALID, "'%s' is not a gamepad's axis.", axis);
    return index;
}

static bool GamepadButton(GamepadHandle *handle, const char *button)
{
    return SDL_GetGamepadButton(handle->gamepad, GamepadFindButton(button));
}

static f32 GamepadAxis(GamepadHandle *handle, const char *axis)
{
    return GamepadAxisValue(SDL_GetGamepadAxis(handle->gamepad, GamepadFindAxis(axis)));
}

/// @brief A strength from 0 to 1 as SDL's rumble takes it.
static u16 GamepadStrength(f32 strength)
{
    return (u16)(SDL_clamp(strength, 0.0F, 1.0F) * (f32)0xFFFF);
}

static bool GamepadRumble(GamepadHandle *handle, f32 low, f32 high, f32 seconds, const char **retError)
{
    *retError = nullptr;

    if (!SDL_RumbleGamepad(handle->gamepad, GamepadStrength(low), GamepadStrength(high), (u32)(SDL_max(seconds, 0.0F) * 1000.0F)))
    {
        *retError = GamepadFail("the gamepad cannot rumble");
        return false;
    }

    return true;
}

static GamepadVirtual *GamepadAttachVirtual(const char *name, const char **retError)
{
    *retError = nullptr;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;

    GamepadVirtual *handle = SDL_calloc(1, sizeof(GamepadVirtual));

    if (handle == nullptr || (handle->id = SDL_AttachVirtualJoystick(&desc)) == 0)
    {
        *retError = GamepadFail(handle == nullptr ? "out of memory" : "the virtual gamepad cannot attach");
        SDL_free(handle);
        return nullptr;
    }

    if ((handle->joystick = SDL_OpenJoystick(handle->id)) == nullptr)
    {
        *retError = GamepadFail("the virtual gamepad cannot open");
        SDL_DetachVirtualJoystick(handle->id);
        SDL_free(handle);
        return nullptr;
    }

    return handle;
}

static i32 GamepadVirtualId(GamepadVirtual *handle)
{
    return (i32)handle->id;
}

// a virtual gamepad's buttons and axes are in SDL's gamepad order, as it is a gamepad. SDL reads a virtual gamepad once in each pass of the main loop,
// so a press and a release in one pass would give no event; it reads it at once instead, and each change gives its event
static bool GamepadVirtualPress(GamepadVirtual *handle, const char *button, bool down, const char **retError)
{
    *retError = nullptr;
    SDL_GamepadButton index = GamepadFindButton(button);

    if (index == SDL_GAMEPAD_BUTTON_INVALID)
    {
        return false;
    }

    if (!SDL_SetJoystickVirtualButton(handle->joystick, index, down))
    {
        *retError = GamepadFail("the button cannot be set");
        return false;
    }

    SDL_UpdateJoysticks();
    return true;
}

static bool GamepadVirtualMove(GamepadVirtual *handle, const char *axis, f32 value, const char **retError)
{
    *retError = nullptr;
    SDL_GamepadAxis index = GamepadFindAxis(axis);

    if (index == SDL_GAMEPAD_AXIS_INVALID)
    {
        return false;
    }

    if (!SDL_SetJoystickVirtualAxis(handle->joystick, index, (i16)(SDL_clamp(value, -1.0F, 1.0F) * (f32)SDL_JOYSTICK_AXIS_MAX)))
    {
        *retError = GamepadFail("the axis cannot be set");
        return false;
    }

    SDL_UpdateJoysticks();
    return true;
}

// SDL ignores gamepads while none of the tool's windows has the keyboard's focus, as games want, unless this setting says otherwise
static void GamepadBackgroundChanged(void *data)
{
    (void)data;
    bool background = ECSValue_GetBool(ECSSetting_Get("gamepad.background"), false);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, background ? "1" : "0");
}

static void GamepadFree(void *data)
{
    (void)data;
    SDL_RemoveEventWatch(GamepadWatch, nullptr);
    SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);

    if (GAMEPAD.sdlStarted)
    {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }

    SDL_zero(GAMEPAD);
}

/// @brief Registers the functions, the methods of the handles and the events.
static SHUResult GamepadRegister(ECSPlugin plugin)
{
    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"list", (ECSFunction)GamepadList, "void(out value<gamepad.Info[]> gamepads)",
         "List the gamepads that are plugged in\n"
         "@param gamepads Each one's id, name and type, such as \"xbox360\" or \"ps5\""},
        {"open", (ECSFunction)GamepadOpen, "handle<gamepad.gamepad>(int id, out string error)",
         "Open a gamepad, to read its buttons and axes and rumble it\n"
         "@param id Its id, from list or from the event gamepad.added\n"
         "@param error Why it cannot open; nothing if it opens\n"
         "@return The gamepad; nothing if it cannot open"},
        {"id", (ECSFunction)GamepadId, "int(handle<gamepad.gamepad> gamepad)",
         "Give the gamepad's id, which its events carry\n"
         "@param gamepad The gamepad\n"
         "@return Its id"},
        {"name", (ECSFunction)GamepadName, "string(handle<gamepad.gamepad> gamepad)",
         "Give the gamepad's name\n"
         "@param gamepad The gamepad\n"
         "@return Its name"},
        {"connected", (ECSFunction)GamepadConnected, "bool(handle<gamepad.gamepad> gamepad)",
         "Tell whether the gamepad is still plugged in\n"
         "@param gamepad The gamepad\n"
         "@return false once it is unplugged"},
        {"button", (ECSFunction)GamepadButton, "bool(handle<gamepad.gamepad> gamepad, string button)",
         "Tell whether a button is down\n"
         "@param gamepad The gamepad\n"
         "@param button SDL's name of it: a, b, x, y, back, guide, start, leftstick, rightstick, leftshoulder, rightshoulder, dpup, dpdown, dpleft, dpright, misc1\n"
         "@return true while it is down"},
        {"axis", (ECSFunction)GamepadAxis, "float(handle<gamepad.gamepad> gamepad, string axis)",
         "Give where a stick or a trigger is\n"
         "@param gamepad The gamepad\n"
         "@param axis SDL's name of it: leftx, lefty, rightx, righty, lefttrigger, righttrigger\n"
         "@return -1 to 1 for a stick, with y growing down, and 0 to 1 for a trigger"},
        {"rumble", (ECSFunction)GamepadRumble, "bool(handle<gamepad.gamepad> gamepad, float low, float high, float seconds, out string error)",
         "Rumble the gamepad; a new rumble replaces the last one, and a strength of 0 for both stops it\n"
         "@param gamepad The gamepad\n"
         "@param low The strength of the low-frequency motor, 0 to 1\n"
         "@param high The strength of the high-frequency motor, 0 to 1\n"
         "@param seconds How long it rumbles\n"
         "@param error Why it cannot rumble, as when it has no motors; nothing if it rumbles\n"
         "@return True if it rumbles"},
        {"attachVirtual", (ECSFunction)GamepadAttachVirtual, "handle<gamepad.virtual>(string name, out string error)",
         "Plug in a virtual gamepad, whose buttons and axes the plugin sets; it shows as any gamepad does, for tests and for tools that turn other input into a gamepad's\n"
         "@param name Its name\n"
         "@param error Why it cannot attach; nothing if it attaches\n"
         "@return The virtual gamepad; nothing if it cannot attach. One whose handle is gone is unplugged"},
        {"virtualId", (ECSFunction)GamepadVirtualId, "int(handle<gamepad.virtual> pad)",
         "Give the virtual gamepad's id, to open it as a gamepad\n"
         "@param pad The virtual gamepad\n"
         "@return Its id"},
        {"virtualPress", (ECSFunction)GamepadVirtualPress, "bool(handle<gamepad.virtual> pad, string button, bool down, out string error)",
         "Press or release a button of the virtual gamepad\n"
         "@param pad The virtual gamepad\n"
         "@param button The button's name, as button takes it\n"
         "@param down true to press it, false to release it\n"
         "@param error Why it cannot be set; nothing if it is\n"
         "@return True if it is set"},
        {"virtualMove", (ECSFunction)GamepadVirtualMove, "bool(handle<gamepad.virtual> pad, string axis, float value, out string error)",
         "Move a stick or a trigger of the virtual gamepad\n"
         "@param pad The virtual gamepad\n"
         "@param axis The axis's name, as axis takes it\n"
         "@param value Where it goes, -1 to 1\n"
         "@param error Why it cannot be set; nothing if it is\n"
         "@return True if it is set"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a handle, such as pad:button("a"); C calls the functions themselves (OpenECS DESIGN 10.6)
    const char *methods[] = {"id", "name", "connected", "button", "axis", "rumble"};

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, "gamepad", methods[i], methods[i]));
    }

    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "virtual", "id", "virtualId"));
    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "virtual", "press", "virtualPress"));
    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "virtual", "move", "virtualMove"));

    SHU_ReturnResult(ECSEvent_Declare(plugin, "added", "A gamepad was plugged in: { id = id, name = name }"));
    SHU_ReturnResult(ECSEvent_Declare(plugin, "removed", "A gamepad was unplugged: { id = id }"));
    SHU_ReturnResult(ECSEvent_Declare(plugin, "button", "A button of a gamepad went down or up: { id = id, button = name, down = bool }"));
    SHU_ReturnResult(ECSEvent_Declare(plugin, "axis", "A stick or a trigger of a gamepad moved: { id = id, axis = name, value = -1 to 1 }"));
    return SHUResult_Ok;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    GAMEPAD.plugin = plugin;
    const ECSPluginDesc desc = {.Destroy = GamepadFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "gamepad", GamepadDestroy));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "virtual", GamepadVirtualDestroy));
    SHU_ReturnResult(GamepadRegister(plugin));

    const ECSSettingDesc background = {.name = "background",
                                       .type = ECSSettingType_Bool,
                                       .description = "Read the gamepads while another program has the keyboard's focus",
                                       .Changed = GamepadBackgroundChanged};
    SHU_ReturnResult(ECSSetting_Declare(plugin, &background));
    GamepadBackgroundChanged(nullptr);

    // the core starts only SDL's video; the gamepads are this plugin's, and SDL counts each start, so another plugin may start them too
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        ECS_LogError(plugin, "SDL's gamepads cannot start: %s", SDL_GetError());
        return SHUResult_Err;
    }

    GAMEPAD.sdlStarted = true;

    if (!SDL_AddEventWatch(GamepadWatch, nullptr))
    {
        ECS_LogError(plugin, "The gamepads' events cannot be watched: %s", SDL_GetError());
        return SHUResult_Err;
    }

    return SHUResult_Ok;
}
