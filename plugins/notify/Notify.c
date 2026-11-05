// The notify standard plugin: desktop notifications and tray icons, which OpenECS gives to plugins (OVERVIEW 11), through SDL's notification
// and tray functions (DESIGN 24). What the user does with them comes back as this plugin's events.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief An entry of a tray's menu, which gives its id to the event of a click.
typedef struct NotifyEntry
{
    struct NotifyTray *tray;
    char *id;
    bool checkbox;
} NotifyEntry;

/// @brief A tray icon with its menu.
typedef struct NotifyTray
{
    SDL_Tray *tray;
    char *name;            // what its events name it by
    NotifyEntry **entries; // the menu's entries, each allocated, as SDL keeps pointers to them
    usz entryCount;
} NotifyTray;

/// @brief A notification's action, which a thread of SDL's may give; it reaches the main thread as a copy.
typedef struct NotifyAction
{
    SDL_NotificationID id;
    char action[];
} NotifyAction;

static struct
{
    ECSPlugin plugin;
    char *error; // why the last call failed, kept until the next one
} NOTIFY = {0};

/// @brief Keeps why a call failed, with SDL's error, and gives it.
static const char *NotifyFail(const char *what)
{
    SDL_free(NOTIFY.error);
    NOTIFY.error = nullptr;

    if (SDL_asprintf(&NOTIFY.error, "%s: %s", what, SDL_GetError()) < 0)
    {
        NOTIFY.error = nullptr;
    }

    return NOTIFY.error == nullptr ? "out of memory" : NOTIFY.error;
}

/// @brief Sets a field of a table to a text; false if there is no memory.
static bool NotifySetText(ECSValue *table, const char *name, const char *text)
{
    ECSValue *field = nullptr;
    return ECSValue_SetTableField(table, name, &field) == SHUResult_Ok && ECSValue_SetString(field, text) == SHUResult_Ok;
}

/// @brief Emits an event with a table of two texts, and a bool unless it is nullptr.
static void NotifyEmit(const char *event, const char *names[2], const char *texts[2], const bool *checked)
{
    ECSValue *value = nullptr;

    if (ECSValue_Create(&value))
    {
        return;
    }

    ECSValue_SetTable(value);
    bool filled = NotifySetText(value, names[0], texts[0]) && (names[1] == nullptr || NotifySetText(value, names[1], texts[1]));
    ECSValue *field = nullptr;

    if (filled && checked != nullptr && (filled = ECSValue_SetTableField(value, "checked", &field) == SHUResult_Ok))
    {
        ECSValue_SetBool(field, *checked);
    }

    if (!filled || ECSEvent_Emit(NOTIFY.plugin, event, value))
    {
        ECS_LogWarn(NOTIFY.plugin, "'%s' cannot be emitted.", event);
    }

    ECSValue_Destroy(&value);
}

/// @brief Emits notify.action on the main thread; after the plugin's Destroy, the action is dropped.
static void NotifyDeliverAction(void *data)
{
    NotifyAction *action = data;

    if (NOTIFY.plugin != nullptr)
    {
        ECSValue *value = nullptr;
        ECSValue *field = nullptr;

        if (ECSValue_Create(&value) == SHUResult_Ok)
        {
            ECSValue_SetTable(value);

            bool filled = ECSValue_SetTableField(value, "id", &field) == SHUResult_Ok;

            if (filled)
            {
                ECSValue_SetInteger(field, action->id);
                filled = NotifySetText(value, "action", action->action);
            }

            if (!filled || ECSEvent_Emit(NOTIFY.plugin, "notify.action", value))
            {
                ECS_LogWarn(NOTIFY.plugin, "'notify.action' cannot be emitted.");
            }

            ECSValue_Destroy(&value);
        }
    }

    SDL_free(action);
}

// on Windows, a notification's action comes on a thread of the system's, so it reaches the main thread as a copy
static bool NotifyWatch(void *data, SDL_Event *event)
{
    (void)data;

    if (event->type != SDL_EVENT_NOTIFICATION_ACTION_INVOKED || event->notification.action_id == nullptr)
    {
        return true;
    }

    usz length = SDL_strlen(event->notification.action_id);
    NotifyAction *action = SDL_malloc(sizeof(NotifyAction) + length + 1);

    if (action == nullptr)
    {
        return true;
    }

    action->id = event->notification.which;
    SDL_memcpy(action->action, event->notification.action_id, length + 1);

    if (SDL_IsMainThread())
    {
        NotifyDeliverAction(action);
    }
    else if (ECS_RunOnMainThread(NotifyDeliverAction, action))
    {
        SDL_free(action);
    }

    return true;
}

/// @brief Loads an image file into a surface; nullptr for an empty path. A relative path starts at the executable's folder.
static SDL_Surface *NotifyLoadImage(const char *path, const char **retError)
{
    if (path == nullptr || path[0] == '\0')
    {
        return nullptr;
    }

    char *fullPath = nullptr;
    SDL_Surface *image = ECSPath_GetFull(&fullPath, path) ? nullptr : SDL_LoadSurface(fullPath);
    ECSPath_Free(fullPath);

    if (image == nullptr)
    {
        *retError = NotifyFail("the image cannot be read");
    }

    return image;
}

#pragma region Notifications

/// @brief Reads a priority's name; normal for any other.
static SDL_NotificationPriority NotifyPriority(const char *name)
{
    if (SDL_strcmp(name, "low") == 0)
    {
        return SDL_NOTIFICATION_PRIORITY_LOW;
    }

    if (SDL_strcmp(name, "high") == 0)
    {
        return SDL_NOTIFICATION_PRIORITY_HIGH;
    }

    return SDL_strcmp(name, "critical") == 0 ? SDL_NOTIFICATION_PRIORITY_CRITICAL : SDL_NOTIFICATION_PRIORITY_NORMAL;
}

/// @brief Makes a notification's buttons from a list of tables of an id and a label; nullptr for none. Their texts are the value's.
static SDL_NotificationAction *NotifyButtons(const ECSValue *buttons, int *retCount)
{
    usz count = ECSValue_GetListCount(buttons);
    SDL_NotificationAction *actions = count == 0 ? nullptr : SDL_calloc(count, sizeof(SDL_NotificationAction));
    *retCount = actions == nullptr ? 0 : (int)count;

    for (usz i = 0; actions != nullptr && i < count; i++)
    {
        const ECSValue *button = ECSValue_GetListItem(buttons, i);
        actions[i].button.type = SDL_NOTIFICATION_ACTION_TYPE_BUTTON;
        actions[i].button.action_id = ECSValue_GetString(ECSValue_GetTableField(button, "id"), "");
        actions[i].button.action_label = ECSValue_GetString(ECSValue_GetTableField(button, "label"), actions[i].button.action_id);
    }

    return actions;
}

static i64 NotifySend(const ECSValue *notification, const char **retError)
{
    *retError = nullptr;
    const char *title = ECSValue_GetString(ECSValue_GetTableField(notification, "title"), nullptr);

    if (title == nullptr)
    {
        *retError = "a notification needs a title";
        return 0;
    }

    SDL_Surface *image = NotifyLoadImage(ECSValue_GetString(ECSValue_GetTableField(notification, "image"), nullptr), retError);

    if (*retError != nullptr)
    {
        return 0;
    }

    int count = 0;
    SDL_NotificationAction *actions = NotifyButtons(ECSValue_GetTableField(notification, "buttons"), &count);

    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetStringProperty(properties, SDL_PROP_NOTIFICATION_TITLE_STRING, title);
    SDL_SetStringProperty(properties, SDL_PROP_NOTIFICATION_MESSAGE_STRING, ECSValue_GetString(ECSValue_GetTableField(notification, "message"), nullptr));
    SDL_SetPointerProperty(properties, SDL_PROP_NOTIFICATION_IMAGE_POINTER, image);
    SDL_SetPointerProperty(properties, SDL_PROP_NOTIFICATION_ACTIONS_POINTER, actions);
    SDL_SetNumberProperty(properties, SDL_PROP_NOTIFICATION_ACTION_COUNT_NUMBER, count);
    SDL_SetNumberProperty(properties, SDL_PROP_NOTIFICATION_PRIORITY_NUMBER, NotifyPriority(ECSValue_GetString(ECSValue_GetTableField(notification, "priority"), "")));
    SDL_SetNumberProperty(properties, SDL_PROP_NOTIFICATION_REPLACES_NUMBER, ECSValue_GetInteger(ECSValue_GetTableField(notification, "replaces"), 0));
    SDL_SetBooleanProperty(properties, SDL_PROP_NOTIFICATION_TRANSIENT_BOOLEAN, ECSValue_GetBool(ECSValue_GetTableField(notification, "transient"), false));

    SDL_NotificationID id = SDL_ShowNotificationWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_DestroySurface(image);
    SDL_free(actions);

    if (id == 0)
    {
        *retError = NotifyFail("the notification cannot be shown");
    }

    return id;
}

static bool NotifyRemove(i64 id)
{
    return id > 0 && SDL_RemoveNotification((SDL_NotificationID)id);
}

#pragma endregion Notifications

#pragma region Trays

/// @brief Emits notify.trayEntry for a click on an entry, with whether a checkbox is checked now.
static void SDLCALL NotifyClickEntry(void *data, SDL_TrayEntry *entry)
{
    NotifyEntry *clicked = data;
    const char *names[2] = {"tray", "entry"};
    const char *texts[2] = {clicked->tray->name, clicked->id};
    bool checked = clicked->checkbox && SDL_GetTrayEntryChecked(entry);
    NotifyEmit("notify.trayEntry", names, texts, clicked->checkbox ? &checked : nullptr);
}

/// @brief Emits notify.trayClicked for a left click on the icon; the menu opens with a right click.
static bool SDLCALL NotifyClickTray(void *data, SDL_Tray *tray)
{
    (void)tray;
    NotifyTray *clicked = data;
    const char *names[2] = {"tray", nullptr};
    const char *texts[2] = {clicked->name, nullptr};
    NotifyEmit("notify.trayClicked", names, texts, nullptr);
    return false;
}

static void NotifyClearEntries(NotifyTray *tray)
{
    SDL_TrayMenu *menu = SDL_GetTrayMenu(tray->tray);
    int count = 0;
    const SDL_TrayEntry **entries = menu == nullptr ? nullptr : SDL_GetTrayEntries(menu, &count);

    for (int i = count - 1; entries != nullptr && i >= 0; i--)
    {
        SDL_RemoveTrayEntry((SDL_TrayEntry *)entries[i]);
    }

    for (usz i = 0; i < tray->entryCount; i++)
    {
        SDL_free(tray->entries[i]->id);
        SDL_free(tray->entries[i]);
    }

    SDL_free((void *)tray->entries);
    tray->entries = nullptr;
    tray->entryCount = 0;
}

/// @brief Fills a tray's menu from a list of tables: each an id and a label, with checked for a checkbox and disabled to grey it; a table without a label is a separator.
static bool NotifySetEntries(NotifyTray *tray, const ECSValue *entries)
{
    NotifyClearEntries(tray);
    usz count = ECSValue_GetListCount(entries);
    SDL_TrayMenu *menu = SDL_GetTrayMenu(tray->tray);

    if (count == 0)
    {
        return true;
    }

    if ((menu == nullptr && (menu = SDL_CreateTrayMenu(tray->tray)) == nullptr) || (tray->entries = SDL_calloc(count, sizeof(NotifyEntry *))) == nullptr)
    {
        return false;
    }

    for (usz i = 0; i < count; i++)
    {
        const ECSValue *item = ECSValue_GetListItem(entries, i);
        const char *label = ECSValue_GetString(ECSValue_GetTableField(item, "label"), nullptr);
        const ECSValue *checked = ECSValue_GetTableField(item, "checked");
        SDL_TrayEntryFlags flags = ECSValue_GetType(checked) == ECSValueType_Bool ? SDL_TRAYENTRY_CHECKBOX : SDL_TRAYENTRY_BUTTON;
        flags |= ECSValue_GetBool(checked, false) ? SDL_TRAYENTRY_CHECKED : 0;
        flags |= ECSValue_GetBool(ECSValue_GetTableField(item, "disabled"), false) ? SDL_TRAYENTRY_DISABLED : 0;

        SDL_TrayEntry *entry = SDL_InsertTrayEntryAt(menu, -1, label, flags);
        NotifyEntry *data = SDL_calloc(1, sizeof(NotifyEntry));

        if (entry == nullptr || data == nullptr || (data->id = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(item, "id"), label == nullptr ? "" : label))) == nullptr)
        {
            SDL_free(data);
            return false;
        }

        data->tray = tray;
        data->checkbox = (flags & SDL_TRAYENTRY_CHECKBOX) != 0;
        tray->entries[tray->entryCount++] = data;

        if (label != nullptr)
        {
            SDL_SetTrayEntryCallback(entry, NotifyClickEntry, data);
        }
    }

    return true;
}

static void NotifyTrayDestroy(void *object)
{
    NotifyTray *tray = object;
    SDL_DestroyTray(tray->tray);

    for (usz i = 0; i < tray->entryCount; i++)
    {
        SDL_free(tray->entries[i]->id);
        SDL_free(tray->entries[i]);
    }

    SDL_free((void *)tray->entries);
    SDL_free(tray->name);
    SDL_free(tray);
}

static NotifyTray *NotifyNewTray(const ECSValue *desc, const char **retError)
{
    *retError = nullptr;
    SDL_Surface *icon = NotifyLoadImage(ECSValue_GetString(ECSValue_GetTableField(desc, "icon"), nullptr), retError);

    if (*retError != nullptr)
    {
        return nullptr;
    }

    NotifyTray *tray = SDL_calloc(1, sizeof(NotifyTray));

    if (tray == nullptr || (tray->name = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(desc, "name"), ""))) == nullptr)
    {
        SDL_free(tray);
        SDL_DestroySurface(icon);
        *retError = "out of memory";
        return nullptr;
    }

    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_TRAY_CREATE_ICON_POINTER, icon);
    SDL_SetStringProperty(properties, SDL_PROP_TRAY_CREATE_TOOLTIP_STRING, ECSValue_GetString(ECSValue_GetTableField(desc, "tooltip"), nullptr));
    SDL_SetPointerProperty(properties, SDL_PROP_TRAY_CREATE_USERDATA_POINTER, tray);
    // SDL takes the callback as an object pointer, which ISO C does not convert from a function pointer
    union
    {
        SDL_TrayClickCallback function;
        void *pointer;
    } click = {.function = NotifyClickTray};
    SDL_SetPointerProperty(properties, SDL_PROP_TRAY_CREATE_LEFTCLICK_CALLBACK_POINTER, click.pointer);
    tray->tray = SDL_CreateTrayWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_DestroySurface(icon);

    if (tray->tray == nullptr)
    {
        *retError = NotifyFail("the tray icon cannot be shown");
        SDL_free(tray->name);
        SDL_free(tray);
        return nullptr;
    }

    if (!NotifySetEntries(tray, ECSValue_GetTableField(desc, "entries")))
    {
        *retError = NotifyFail("the tray's menu cannot be made");
        NotifyTrayDestroy(tray);
        return nullptr;
    }

    return tray;
}

static bool NotifyTraySetEntries(NotifyTray *tray, const ECSValue *entries)
{
    return NotifySetEntries(tray, entries);
}

static void NotifyTraySetTooltip(NotifyTray *tray, const char *tooltip)
{
    SDL_SetTrayTooltip(tray->tray, tooltip);
}

static bool NotifyTraySetIcon(NotifyTray *tray, const char *path)
{
    const char *problem = nullptr;
    SDL_Surface *icon = NotifyLoadImage(path, &problem);

    if (problem != nullptr)
    {
        return false;
    }

    SDL_SetTrayIcon(tray->tray, icon);
    SDL_DestroySurface(icon);
    return true;
}

#pragma endregion Trays

static void NotifyFree(void *data)
{
    (void)data;
    SDL_RemoveEventWatch(NotifyWatch, nullptr);
    SDL_free(NOTIFY.error);
    SDL_zero(NOTIFY);
}

/// @brief Registers the functions, the methods of the tray and the events.
static SHUResult NotifyRegister(ECSPlugin plugin)
{
    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"send", (ECSFunction)NotifySend, "int64(value<notify.Notification> notification, out string error)",
         "Show a desktop notification; the system decides how and whether the user sees it, such as in a do-not-disturb mode\n"
         "@param notification Its title, and optionally its message, image, priority, buttons, the id of one it replaces, and whether it is transient\n"
         "@param error Why it cannot be shown; nothing if it was\n"
         "@return Its id, which notify.action carries; nothing if it cannot be shown"},
        {"remove", (ECSFunction)NotifyRemove, "bool(int64 id)",
         "Take a notification away\n"
         "@param id Its id\n"
         "@return false if it is not shown"},
        {"tray", (ECSFunction)NotifyNewTray, "handle<notify.tray>(value<notify.Tray> tray, out string error)",
         "Show an icon in the system's tray, with a menu; it stays until its handle is gone\n"
         "@param tray Its name, which its events carry, and optionally its icon, tooltip and menu entries\n"
         "@param error Why it cannot be shown; nothing if it was\n"
         "@return The tray icon; nothing if it cannot be shown, such as on a desktop without a tray"},
        {"traySetEntries", (ECSFunction)NotifyTraySetEntries, "bool(handle<notify.tray> tray, value<notify.TrayEntry[]> entries)",
         "Replace the tray icon's menu\n"
         "@param tray The tray icon\n"
         "@param entries The entries\n"
         "@return false if the menu cannot be made"},
        {"traySetTooltip", (ECSFunction)NotifyTraySetTooltip, "void(handle<notify.tray> tray, string tooltip)",
         "Change the text shown over the tray icon\n"
         "@param tray The tray icon\n"
         "@param tooltip The text"},
        {"traySetIcon", (ECSFunction)NotifyTraySetIcon, "bool(handle<notify.tray> tray, string icon)",
         "Change the tray icon's picture\n"
         "@param tray The tray icon\n"
         "@param icon An image file, such as a PNG, from the executable's folder unless absolute\n"
         "@return false if the image cannot be read"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a handle, such as tray:setTooltip(text); C calls the functions themselves (OpenECS DESIGN 10.6)
    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "tray", "setEntries", "traySetEntries"));
    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "tray", "setTooltip", "traySetTooltip"));
    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "tray", "setIcon", "traySetIcon"));

    SHU_ReturnResult(ECSEvent_Declare(plugin, "action", "The user clicked a notification or one of its buttons: { id = id, action = the button's id, or \"default\" }"));
    SHU_ReturnResult(ECSEvent_Declare(plugin, "trayEntry", "The user clicked an entry of a tray icon's menu: { tray = name, entry = id, checked = bool for a checkbox }"));
    SHU_ReturnResult(ECSEvent_Declare(plugin, "trayClicked", "The user clicked a tray icon: { tray = name }"));
    return SHUResult_Ok;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    NOTIFY.plugin = plugin;
    const ECSPluginDesc desc = {.Destroy = NotifyFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "tray", NotifyTrayDestroy));
    SHU_ReturnResult(NotifyRegister(plugin));

    if (!SDL_AddEventWatch(NotifyWatch, nullptr))
    {
        ECS_LogError(plugin, "The notifications' events cannot be watched: %s", SDL_GetError());
        return SHUResult_Err;
    }

    return SHUResult_Ok;
}
