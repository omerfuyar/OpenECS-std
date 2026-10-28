// ui in C. Each draw describes the elements in an immediate style, with macros in the manner of Clay's CLAY, and ui builds them with the same element
// functions as for Lua (DESIGN 12). A container's block holds its children:
//
//     UI_SHOW(&ui, panel, surface)
//     {
//         UI_COLUMN(.padding = 24, .gap = 12)
//         {
//             UI_TEXT("Hello");
//
//             if (UI_BUTTON("Click me"))
//             {
//                 clicks++;
//             }
//
//             UI_TOGGLE(&sound, .id = "sound", .label = "Sound");
//         }
//     }
//
// C gives ui no functions. An element with an id keeps what its callback gets, and its macro takes it at the next draw: UI_BUTTON gives whether it
// was clicked, and UI_TOGGLE writes the new state into its bool. The plugin's Init calls Ui_Init, and the panel's Event gives each event to Ui_Event.
// The macros cover every element of Lua's ui, with the same props, so a plugin in C builds the same interface as one in Lua.

#include <string.h>

/// @brief grow = UI_FIXED keeps an element from growing where it would by default, as a scroll or a space does.
#define UI_FIXED (-1.0)

/// @brief How deep containers may be nested.
#define UI_DEPTH 32

/// @brief The number of items of an array.
#define UI__COUNT(array) (sizeof(array) / sizeof((array)[0]))

/// @brief The props of an element, in the manner of ui.Props. A number left 0 or a text left nullptr is left to ui. Each element uses the ones it knows.
typedef struct UiProps
{
    const char *id;              // names the element; an element whose macro takes what it got needs one, and a button's label is its id if it has none
    f64 width;                   // a fixed width, in layout units
    f64 height;                  // a fixed height, in layout units
    f64 grow;                    // a share of the room left along the parent's direction; UI_FIXED for none
    bool stretch;                // takes the parent's whole cross size
    f64 padding;                 // room inside a container's edges
    f64 gap;                     // room between a container's children
    const char *align;           // where a container puts children that do not stretch: "start", "center" or "end"
    const char *background;      // a colour, such as "#202226", or a theme colour, such as "tabRow"
    const char *hoverBackground; // the background while the pointer is over it
    const char *border;          // the colour of a line around it
    const char *color;           // the colour of its text
    f64 size;                    // the font size of its text
    const char *font;            // a font name of draw, such as "ui" or "mono"
    const char *tooltip;         // a text that shows when the pointer rests on it
    bool heading;                // a list's child that is not an item
    bool clickable;              // a column or row that is clicked like a button; UI_CLICKED takes the click
    const char *title;           // a group's title
    bool wrap;                   // text that breaks into lines at its width
    bool vertical;               // a separator up a row
    const char *label;           // a toggle's label
    const char *look;            // a toggle's look: "check" or "switch"
    f64 min;                     // a slider's or number's least value; min and max count when they differ
    f64 max;                     // a slider's or number's greatest value
    f64 step;                    // what a slider's or number's value is rounded to
    bool invalid;                // a field's text is wrong
    const char *sort;            // the id of the column a table is sorted by
    bool ascending;              // a table's sort arrow points up
    i32 selected;                // a list's chosen item, from 1; UI_LIST sets it
} UiProps;

/// @brief A column of a table: the id names its cells, and the title shows in the header, the id if it is nullptr; a width of 0 is the widest cell's.
typedef struct UiColumn
{
    const char *id;
    const char *title;
    f64 width;
} UiColumn;

/// @brief A node of a tree. A list of nodes ends with one whose id is nullptr. A node whose children are not nullptr has an arrow, even with none,
/// and open points to whether it is open, which UI_TREE writes when the arrow is clicked.
typedef struct UiTreeNode
{
    const char *id;
    const char *label; // the id if it is nullptr
    const struct UiTreeNode *children;
    bool *open;
} UiTreeNode;

/// @brief What UI_SHOW describes into. Ui_Init prepares it.
typedef struct Ui
{
    ECSPlugin plugin;
    UiFunctions functions;
    ECSPanel panel;
    ECSValue *root;               // a list that holds the element that UI_SHOW shows
    ECSValue *children[UI_DEPTH]; // the children of each open container, the innermost last
    usz depth;
    bool failed; // memory ran out, or containers are nested too deeply; nothing shows
} Ui;

/// @brief The Ui that the current UI_SHOW describes into; the macros use it.
[[maybe_unused]] static Ui *Ui_current = nullptr;

/// @brief Gives a field of a table that is made now, or nullptr if memory runs out.
static inline ECSValue *Ui_NewField(ECSValue *table, const char *name)
{
    ECSValue *field = nullptr;

    if (table == nullptr || ECSValue_SetTableField(table, name, &field))
    {
        Ui_current->failed = true;
        return nullptr;
    }

    return field;
}

static inline void Ui_SetString(ECSValue *table, const char *name, const char *text)
{
    ECSValue *field = text == nullptr ? nullptr : Ui_NewField(table, name);

    if (field != nullptr && ECSValue_SetString(field, text))
    {
        Ui_current->failed = true;
    }
}

static inline void Ui_SetNumber(ECSValue *table, const char *name, f64 number)
{
    ECSValue *field = Ui_NewField(table, name);

    if (field != nullptr)
    {
        ECSValue_SetNumber(field, number);
    }
}

static inline void Ui_SetBool(ECSValue *table, const char *name, bool boolean)
{
    ECSValue *field = Ui_NewField(table, name);

    if (field != nullptr)
    {
        ECSValue_SetBool(field, boolean);
    }
}

/// @brief Sets a field of a table to an empty table, and gives it, or nullptr if memory runs out.
static inline ECSValue *Ui_SetTable(ECSValue *table, const char *name)
{
    ECSValue *field = Ui_NewField(table, name);

    if (field != nullptr)
    {
        ECSValue_SetTable(field);
    }

    return field;
}

/// @brief Adds an item at the end of a list, and gives it, or nullptr if memory runs out.
static inline ECSValue *Ui_AddItem(ECSValue *list)
{
    ECSValue *item = nullptr;

    if (list == nullptr || ECSValue_AddListItem(list, &item))
    {
        Ui_current->failed = true;
        return nullptr;
    }

    return item;
}

static inline void Ui_AddString(ECSValue *list, const char *text)
{
    ECSValue *item = Ui_AddItem(list);

    if (item != nullptr && ECSValue_SetString(item, text))
    {
        Ui_current->failed = true;
    }
}

/// @brief Writes the props that are not left to ui into a description's props.
static inline void Ui_SetProps(ECSValue *description, const UiProps *props)
{
    ECSValue *table = Ui_SetTable(description, "props");

    if (table == nullptr)
    {
        return;
    }

    Ui_SetString(table, "id", props->id);
    Ui_SetString(table, "align", props->align);
    Ui_SetString(table, "background", props->background);
    Ui_SetString(table, "hoverBackground", props->hoverBackground);
    Ui_SetString(table, "border", props->border);
    Ui_SetString(table, "color", props->color);
    Ui_SetString(table, "font", props->font);
    Ui_SetString(table, "tooltip", props->tooltip);
    Ui_SetString(table, "title", props->title);
    Ui_SetString(table, "label", props->label);
    Ui_SetString(table, "look", props->look);
    Ui_SetString(table, "sort", props->sort);

    const struct
    {
        const char *name;
        f64 number;
    } numbers[] = {{"width", props->width}, {"height", props->height}, {"padding", props->padding}, {"gap", props->gap}, {"size", props->size}, {"step", props->step}, {"selected", props->selected}};

    for (usz i = 0; i < UI__COUNT(numbers); i++)
    {
        if (numbers[i].number != 0.0)
        {
            Ui_SetNumber(table, numbers[i].name, numbers[i].number);
        }
    }

    if (props->grow != 0.0)
    {
        Ui_SetNumber(table, "grow", props->grow == UI_FIXED ? 0.0 : props->grow);
    }

    if (props->min != props->max)
    {
        Ui_SetNumber(table, "min", props->min);
        Ui_SetNumber(table, "max", props->max);
    }

    const struct
    {
        const char *name;
        bool set;
        bool value;
    } booleans[] = {{"stretch", props->stretch, true}, {"selectable", props->heading, false}, {"clickable", props->clickable, true}, {"wrap", props->wrap, true}, {"vertical", props->vertical, true}, {"valid", props->invalid, false}, {"ascending", props->ascending, true}};

    for (usz i = 0; i < UI__COUNT(booleans); i++)
    {
        if (booleans[i].set)
        {
            Ui_SetBool(table, booleans[i].name, booleans[i].value);
        }
    }
}

/// @brief Adds the description of an element of a type to the innermost open container, and gives it, or nullptr if memory runs out.
static inline ECSValue *Ui_Add(const char *type, const UiProps *props)
{
    Ui *ui = Ui_current;
    ECSValue *description = nullptr;

    if (ui->failed || ECSValue_AddListItem(ui->children[ui->depth - 1], &description))
    {
        ui->failed = true;
        return nullptr;
    }

    ECSValue_SetTable(description);
    Ui_SetString(description, "type", type);
    Ui_SetProps(description, props);
    return description;
}

/// @brief Opens a container of a type, whose children the next descriptions are, until Ui_Close.
static inline ECSValue *Ui_Open(const char *type, const UiProps *props)
{
    Ui *ui = Ui_current;
    ECSValue *description = Ui_Add(type, props);
    ECSValue *children = Ui_SetTable(description, "children");

    // a container that is too deep, or that memory ran out for, still counts, so Ui_Close stays in step; the root is its parent then
    ui->failed = ui->failed || ui->depth == UI_DEPTH;
    ui->children[ui->depth < UI_DEPTH ? ui->depth : UI_DEPTH - 1] = ui->failed ? ui->root : children;
    ui->depth++;
    return description;
}

static inline void Ui_Close(void)
{
    Ui_current->depth--;
}

/// @brief Prepares a Ui for a plugin: looks up ui's functions. Call it from the plugin's Init; the plugin's manifest must depend on ui.
/// @return SHUResult_Ok, or the error of the first function that cannot be looked up.
static inline SHUWUR SHUResult Ui_Init(Ui *retUi, ECSPlugin plugin)
{
    *retUi = (Ui){.plugin = plugin};
    return UiFunctions_Get(plugin, &retUi->functions);
}

/// @brief Starts describing a panel's elements.
static inline void Ui_Begin(Ui *ui, ECSPanel panel)
{
    Ui_current = ui;
    ui->panel = panel;
    ui->depth = 1;
    ui->failed = ECSValue_Create(&ui->root) != SHUResult_Ok;

    if (!ui->failed)
    {
        ECSValue_SetTable(ui->root);
    }

    ui->children[0] = ui->root;
}

/// @brief Builds the first element described since Ui_Begin, and shows it.
static inline void Ui_End(ECSSurface *surface)
{
    Ui *ui = Ui_current;

    if (!ui->failed && ECSValue_GetListCount(ui->root) > 0)
    {
        ui->functions.showDescription(ui->panel, surface, ECSValue_GetListItem(ui->root, 0));
    }
    else if (ui->failed)
    {
        ECS_LogWarn(ui->plugin, "ui: the elements cannot be described, because memory ran out or containers are nested more than %d deep", UI_DEPTH);
    }

    ECSValue_Destroy(&ui->root);
    Ui_current = nullptr;
}

/// @brief Takes what a callback of the element with an id got since the last draw, or nullptr if it did not run. It is valid until the next take.
static inline const ECSValue *Ui_Take(const char *id, const char *callback)
{
    if (id == nullptr)
    {
        return nullptr;
    }

    const ECSValue *taken = Ui_current->functions.take(Ui_current->panel, id, callback);
    return ECSValue_GetType(taken) == ECSValueType_Table ? taken : nullptr;
}

/// @brief Gives a panel's event to ui, which runs what the elements it reaches do; call it from the panel's Event.
/// @return Whether an element took the event.
static inline bool Ui_Event(Ui *ui, ECSPanel panel, const ECSPanelEvent *event)
{
    static const char *const TYPES[] = {"pointerDown", "pointerUp", "pointerMove", "wheel", "keyDown", "keyUp", "focused", "unfocused", "shown", "hidden", "resized", "drop", "text"};
    ECSValue *table = nullptr;

    if (ECSValue_Create(&table))
    {
        return false;
    }

    // Ui_NewField reports to the current Ui
    Ui *current = Ui_current;
    Ui_current = ui;
    ui->failed = false;
    ECSValue_SetTable(table);
    Ui_SetString(table, "type", (usz)event->type < UI__COUNT(TYPES) ? TYPES[event->type] : "");

    switch (event->type)
    {
    case ECSPanelEventType_PointerDown:
    case ECSPanelEventType_PointerUp:
        Ui_SetNumber(table, "button", event->pointer.button);
        [[fallthrough]];
    case ECSPanelEventType_PointerMove:
        Ui_SetNumber(table, "x", event->pointer.x);
        Ui_SetNumber(table, "y", event->pointer.y);
        break;
    case ECSPanelEventType_Wheel:
        Ui_SetNumber(table, "x", event->wheel.x);
        Ui_SetNumber(table, "y", event->wheel.y);
        Ui_SetNumber(table, "scrollX", event->wheel.scrollX);
        Ui_SetNumber(table, "scrollY", event->wheel.scrollY);
        break;
    case ECSPanelEventType_KeyDown:
    case ECSPanelEventType_KeyUp:
        Ui_SetString(table, "key", event->key.name);
        break;
    case ECSPanelEventType_Text:
        Ui_SetString(table, "text", event->text.text);
        break;
    default:
        break;
    }

    Ui_SetBool(table, "shift", (event->modifiers & ECSModifier_Shift) != 0);
    Ui_SetBool(table, "ctrl", (event->modifiers & ECSModifier_Ctrl) != 0);
    Ui_SetBool(table, "alt", (event->modifiers & ECSModifier_Alt) != 0);
    Ui_SetBool(table, "super", (event->modifiers & ECSModifier_Super) != 0);

    bool taken = !ui->failed && ui->functions.event(panel, table);
    ECSValue_Destroy(&table);
    Ui_current = current;
    return taken;
}

#pragma region Elements

static inline void Ui_Text(const char *text, const UiProps *props)
{
    Ui_SetString(Ui_Add("text", props), "value", text);
}

static inline bool Ui_Button(const char *label, const UiProps *props)
{
    UiProps named = *props;
    named.id = named.id == nullptr ? label : named.id;
    Ui_SetString(Ui_Add("button", &named), "value", label);
    return Ui_Take(named.id, "onClick") != nullptr;
}

static inline bool Ui_Toggle(bool *on, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onChange");
    *on = taken != nullptr ? ECSValue_GetBool(ECSValue_GetTableField(taken, "value"), *on) : *on;
    Ui_SetBool(Ui_Add("toggle", props), "value", *on);
    return taken != nullptr;
}

/// @brief A slider or a number field: type is "slider" or "number".
static inline bool Ui_Number(const char *type, f64 *value, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onChange");
    *value = taken != nullptr ? ECSValue_GetNumber(ECSValue_GetTableField(taken, "value"), *value) : *value;
    Ui_SetNumber(Ui_Add(type, props), "value", *value);
    return taken != nullptr;
}

/// @brief A selector, tabs or a dropdown: type is "selector", "tabs" or "dropdown". The choices end with nullptr, and index is the chosen one's, from 0.
static inline bool Ui_Choice(const char *type, i32 *index, const char *const *choices, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onChange");
    const char *chosen = taken == nullptr ? nullptr : ECSValue_GetString(ECSValue_GetTableField(taken, "value"), nullptr);
    i32 count = 0;

    for (; choices[count] != nullptr; count++)
    {
        if (chosen != nullptr && strcmp(chosen, choices[count]) == 0)
        {
            *index = count;
        }
    }

    ECSValue *description = Ui_Add(type, props);
    Ui_SetString(description, "value", *index >= 0 && *index < count ? choices[*index] : "");
    ECSValue *list = Ui_SetTable(description, "choices");

    for (i32 i = 0; i < count; i++)
    {
        Ui_AddString(list, choices[i]);
    }

    return chosen != nullptr;
}

/// @brief Copies a text into a buffer of a capacity, cut to fit, which always ends with a zero.
static inline void Ui_CopyText(char *buffer, usz capacity, const char *text)
{
    if (text == nullptr || capacity == 0)
    {
        return;
    }

    usz length = strlen(text) < capacity - 1 ? strlen(text) : capacity - 1;
    memmove(buffer, text, length);
    buffer[length] = '\0';
}

/// @brief A text field, whose text is in a buffer of a capacity, which each edit changes.
/// @return Whether Return was pressed in it.
static inline bool Ui_Field(char *buffer, usz capacity, const UiProps *props)
{
    // each take ends the one before, so the text is copied at once
    Ui_CopyText(buffer, capacity, ECSValue_GetString(ECSValue_GetTableField(Ui_Take(props->id, "onChange"), "value"), nullptr));
    const ECSValue *submitted = Ui_Take(props->id, "onSubmit");
    Ui_CopyText(buffer, capacity, ECSValue_GetString(ECSValue_GetTableField(submitted, "value"), nullptr));
    Ui_SetString(Ui_Add("field", props), "value", buffer);
    return submitted != nullptr;
}

/// @brief A key combination, in a buffer of a capacity, which a click and a key press change.
/// @return Whether it changed.
static inline bool Ui_Key(char *buffer, usz capacity, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onChange");
    Ui_CopyText(buffer, capacity, ECSValue_GetString(ECSValue_GetTableField(taken, "value"), nullptr));
    Ui_SetString(Ui_Add("key", props), "value", buffer);
    return taken != nullptr;
}

/// @brief Opens a list, whose chosen item, from 1, is in selected; 0 for none.
static inline void Ui_OpenList(i32 *selected, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onSelect");
    *selected = taken != nullptr ? (i32)ECSValue_GetInteger(ECSValue_GetTableField(taken, "value"), *selected) : *selected;
    UiProps chosen = *props;
    chosen.selected = *selected;
    Ui_Open("list", &chosen);
}

/// @brief The item of a list that was activated, with a click or Return, from 1, or 0 if none was.
static inline i32 Ui_Activated(const char *id)
{
    return (i32)ECSValue_GetInteger(ECSValue_GetTableField(Ui_Take(id, "onActivate"), "value"), 0);
}

/// @brief A table: its columns, which end with one whose id is nullptr, and its rows' cells, a text for each column, row after row. The chosen row,
/// from 1, is in selected; 0 for none. The props' sort and ascending show the arrow of the column the rows are sorted by; the plugin sorts the rows.
static inline void Ui_Table(i32 *selected, const UiColumn *columns, const char *const *cells, usz rowCount, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onSelect");
    *selected = taken != nullptr ? (i32)ECSValue_GetInteger(ECSValue_GetTableField(taken, "value"), *selected) : *selected;
    UiProps chosen = *props;
    chosen.selected = *selected;
    ECSValue *description = Ui_Add("table", &chosen);
    ECSValue *columnList = Ui_SetTable(description, "columns");
    ECSValue *rowList = Ui_SetTable(description, "rows");
    usz columnCount = 0;

    for (; columns[columnCount].id != nullptr; columnCount++)
    {
        ECSValue *column = Ui_AddItem(columnList);

        if (column != nullptr)
        {
            ECSValue_SetTable(column);
            Ui_SetString(column, "id", columns[columnCount].id);
            Ui_SetString(column, "title", columns[columnCount].title);

            if (columns[columnCount].width > 0.0)
            {
                Ui_SetNumber(column, "width", columns[columnCount].width);
            }
        }
    }

    for (usz row = 0; row < rowCount; row++)
    {
        ECSValue *values = Ui_AddItem(rowList);

        if (values != nullptr)
        {
            ECSValue_SetTable(values);
        }

        for (usz column = 0; values != nullptr && column < columnCount; column++)
        {
            Ui_SetString(values, columns[column].id, cells[row * columnCount + column]);
        }
    }
}

/// @brief The id of the column whose title of the table with an id was clicked since the last draw, or nullptr. It is valid until the next take.
static inline const char *Ui_Sorted(const char *id)
{
    return ECSValue_GetString(ECSValue_GetTableField(Ui_Take(id, "onSort"), "value"), nullptr);
}

/// @brief Finds the node of an id among nodes and their children, or nullptr.
static inline const UiTreeNode *Ui_FindTreeNode(const UiTreeNode *nodes, const char *id)
{
    for (const UiTreeNode *node = nodes; id != nullptr && node != nullptr && node->id != nullptr; node++)
    {
        const UiTreeNode *found = strcmp(node->id, id) == 0 ? node : Ui_FindTreeNode(node->children, id);

        if (found != nullptr)
        {
            return found;
        }
    }

    return nullptr;
}

/// @brief Adds the descriptions of tree nodes to a list, and the ids of the open ones to a table of them.
static inline void Ui_AddTreeNodes(ECSValue *list, ECSValue *open, const UiTreeNode *nodes)
{
    for (const UiTreeNode *node = nodes; node->id != nullptr; node++)
    {
        ECSValue *item = Ui_AddItem(list);

        if (item == nullptr)
        {
            return;
        }

        ECSValue_SetTable(item);
        Ui_SetString(item, "id", node->id);
        Ui_SetString(item, "label", node->label);

        if (node->children != nullptr)
        {
            Ui_AddTreeNodes(Ui_SetTable(item, "children"), open, node->children);
        }

        if (node->open != nullptr && *node->open)
        {
            Ui_SetBool(open, node->id, true);
        }
    }
}

/// @brief A tree of nodes, which end with one whose id is nullptr. selected points to the chosen node's id, one of the nodes' own, or nullptr; a
/// click on a node's arrow writes its new state into its open.
/// @return Whether a node was chosen or opened or closed since the last draw.
static inline bool Ui_Tree(const char **selected, const UiTreeNode *nodes, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onSelect");
    const UiTreeNode *chosen = Ui_FindTreeNode(nodes, ECSValue_GetString(ECSValue_GetTableField(taken, "value"), nullptr));
    *selected = chosen != nullptr ? chosen->id : *selected;
    const ECSValue *toggled = Ui_Take(props->id, "onToggle");
    const UiTreeNode *opened = Ui_FindTreeNode(nodes, ECSValue_GetString(ECSValue_GetTableField(toggled, "value"), nullptr));

    if (opened != nullptr && opened->open != nullptr)
    {
        *opened->open = ECSValue_GetBool(ECSValue_GetTableField(toggled, "open"), false);
    }

    ECSValue *description = Ui_Add("tree", props);
    Ui_SetString(description, "selected", *selected);
    Ui_AddTreeNodes(Ui_SetTable(description, "nodes"), Ui_SetTable(description, "open"), nodes);
    return chosen != nullptr || opened != nullptr;
}

#pragma endregion Elements

#define UI__JOIN2(first, second) first##second
#define UI__JOIN(first, second) UI__JOIN2(first, second)
#define UI__LATCH UI__JOIN(ui_latch_, __LINE__)

/// @brief Describes a panel's elements in its block, and shows them after it: UI_SHOW(&ui, panel, surface) { UI_COLUMN() { ... } }. Call it from the panel's Draw.
#define UI_SHOW(ui, panel, surface) for (int UI__LATCH = (Ui_Begin((ui), (panel)), 0); UI__LATCH < 1; UI__LATCH++, Ui_End(surface))

/// @brief Containers: their blocks hold their children. The props are designated initializers of UiProps: UI_COLUMN(.padding = 8, .gap = 4) { ... }.
#define UI_COLUMN(...) for (int UI__LATCH = (Ui_Open("column", &(UiProps){__VA_ARGS__}), 0); UI__LATCH < 1; UI__LATCH++, Ui_Close())
#define UI_ROW(...) for (int UI__LATCH = (Ui_Open("row", &(UiProps){__VA_ARGS__}), 0); UI__LATCH < 1; UI__LATCH++, Ui_Close())
#define UI_SCROLL(...) for (int UI__LATCH = (Ui_Open("scroll", &(UiProps){__VA_ARGS__}), 0); UI__LATCH < 1; UI__LATCH++, Ui_Close())
#define UI_GROUP(...) for (int UI__LATCH = (Ui_Open("group", &(UiProps){__VA_ARGS__}), 0); UI__LATCH < 1; UI__LATCH++, Ui_Close())

/// @brief A list whose children are its items: selected points to the chosen item's number, from 1, which a press changes. UI_ACTIVATED gives a clicked item.
#define UI_LIST(selected, ...) for (int UI__LATCH = (Ui_OpenList((selected), &(UiProps){__VA_ARGS__}), 0); UI__LATCH < 1; UI__LATCH++, Ui_Close())

/// @brief Elements without children. Those that take a pointer write what the user changed into it, and give whether it changed.
#define UI_TEXT(text, ...) Ui_Text((text), &(UiProps){__VA_ARGS__})
#define UI_BUTTON(label, ...) Ui_Button((label), &(UiProps){__VA_ARGS__})
#define UI_TOGGLE(on, ...) Ui_Toggle((on), &(UiProps){__VA_ARGS__})
#define UI_SLIDER(value, ...) Ui_Number("slider", (value), &(UiProps){__VA_ARGS__})
#define UI_NUMBER(value, ...) Ui_Number("number", (value), &(UiProps){__VA_ARGS__})
#define UI_SELECTOR(index, choices, ...) Ui_Choice("selector", (index), (choices), &(UiProps){__VA_ARGS__})
#define UI_TABS(index, choices, ...) Ui_Choice("tabs", (index), (choices), &(UiProps){__VA_ARGS__})
#define UI_DROPDOWN(index, choices, ...) Ui_Choice("dropdown", (index), (choices), &(UiProps){__VA_ARGS__})
#define UI_FIELD(buffer, capacity, ...) Ui_Field((buffer), (capacity), &(UiProps){__VA_ARGS__})
#define UI_KEY(buffer, capacity, ...) Ui_Key((buffer), (capacity), &(UiProps){__VA_ARGS__})
#define UI_TABLE(selected, columns, cells, rowCount, ...) Ui_Table((selected), (columns), (cells), (rowCount), &(UiProps){__VA_ARGS__})
#define UI_TREE(selected, nodes, ...) Ui_Tree((selected), (nodes), &(UiProps){__VA_ARGS__})
#define UI_SEPARATOR(...) ((void)Ui_Add("separator", &(UiProps){__VA_ARGS__}))
#define UI_SPACE(...) ((void)Ui_Add("space", &(UiProps){__VA_ARGS__}))
#define UI_IMAGE(path, ...) Ui_SetString(Ui_Add("image", &(UiProps){__VA_ARGS__}), "value", (path))

/// @brief Whether the clickable column or row with an id was clicked since the last draw.
#define UI_CLICKED(id) (Ui_Take((id), "onClick") != nullptr)

/// @brief The item of the list with an id that was activated since the last draw, from 1, or 0.
#define UI_ACTIVATED(id) Ui_Activated(id)

/// @brief The id of the column whose title of the table with an id was clicked since the last draw, or nullptr.
#define UI_SORTED(id) Ui_Sorted(id)
