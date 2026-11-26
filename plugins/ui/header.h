// ui in C. Each draw describes the elements in an immediate style, with macros in the manner of Clay's CLAY, and ui builds them with the same element
// functions as for Lua (DESIGN 12). A container's block holds its children:
//
//     UI_SHOW(&ui, panel, surface)
//     {
//         UI_COLUMN(.style = &ui.styles.view)
//         {
//             UI_TEXT("Hello", .style = &ui.styles.title);
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
// An element with an id keeps what its callback gets, and its macro takes it at the next draw: UI_BUTTON gives whether it was clicked, and UI_TOGGLE
// writes the new state into its bool. Where ui needs an answer at once, or a callback reads more naturally, the props take C functions, as Lua's take
// Lua functions: onDrag, onDragOver, onDrop and onMenu, which get the props' user first. The plugin's Init calls Ui_Init, and the panel's Event gives
// each event to Ui_Event.
// The macros cover every element of Lua's ui, with the same props, so a plugin in C builds the same interface as one in Lua.

#include <ctype.h>
#include <string.h>

/// @brief grow = UI_FIXED keeps an element from growing where it would by default, as a scroll or a space does.
#define UI_FIXED (-1.0)

/// @brief A number of the props that is 0, such as padding = UI_ZERO; a number left 0 is left to ui.
#define UI_ZERO (-1.0e300)

/// @brief A flag of the props: left to ui, true or false. true and false set it too, as stretch = true does; UI_FALSE says false.
typedef enum UiFlag
{
    UI_UNSET,
    UI_TRUE,
    UI_FALSE,
} UiFlag;

/// @brief How deep containers may be nested.
#define UI_DEPTH 32

/// @brief The number of items of an array.
#define UI__COUNT(array) (sizeof(array) / sizeof((array)[0]))

/// @brief Where dragged data lands on an item of a list, a table or a tree: before it, after it or into it; UI_BESIDE beside the items, and on any
/// other element, where Lua's where is nil.
typedef enum UiWhere
{
    UI_BESIDE,
    UI_BEFORE,
    UI_AFTER,
    UI_INTO,
} UiWhere;

/// @brief What onDragOver and onDrop get, as Lua's get it.
typedef struct UiDrop
{
    const ECSValue *item;  // a list's or a table's item, from 1, or a tree's node id; nullptr beside the items and on another element
    UiWhere where;         // where on the item it lands
    const char *dataType;  // the data's type, one that the panel accepts
    const ECSValue *value; // the dragged value
    f64 x;                 // the place on an element that is not a list, a table or a tree, in layout units; 0 on them
    f64 y;
} UiDrop;

/// @brief A press on an item and a move of the drag threshold give the item, as onDrag does in Lua.
/// @param user The props' user.
/// @param item The item: a list's or a table's number, from 1, or a tree's node id.
/// @param retType The type of the data to drag, which must live until the function returns.
/// @param retValue The value to drag, which the function fills.
/// @return Whether to drag the data, which ui drags as the core's; false drags nothing.
typedef bool (*UiDragFunction)(void *user, const ECSValue *item, const char **retType, ECSValue *retValue);

/// @brief Says whether dragged data may land where it is, at each DragOver and at the drop, as onDragOver does in Lua.
/// @param user The props' user.
/// @param drop What onDrop would get; over an item, a new where gives the item that place, which the mark shows and onDrop gets.
/// @return True to take the landing, false to refuse it: no mark, and a drop there goes to no onDrop.
typedef bool (*UiDragOverFunction)(void *user, UiDrop *drop);

/// @brief Data dropped on an element, as onDrop gets it in Lua.
/// @param user The props' user.
/// @param drop What was dropped, and where.
/// @return Nothing.
typedef void (*UiDropFunction)(void *user, const UiDrop *drop);

/// @brief A release of the right button on an element, as onMenu gets it in Lua.
/// @param user The props' user.
/// @param item The item under the pointer; nullptr beside the items, and on an element that is not a list, a table or a tree.
/// @param x The place, in layout units, as Ui_Menu takes it.
/// @param y The place's y.
/// @return Nothing.
typedef void (*UiMenuFunction)(void *user, const ECSValue *item, f64 x, f64 y);

/// @brief A click on a menu's entry, or Return on it.
/// @param user The entry's user.
/// @return Nothing.
typedef void (*UiClickFunction)(void *user);

/// @brief The props of an element, in the manner of ui.Props. A number left 0, a flag left unset or a text left nullptr is left to ui. Each element uses the ones it knows.
typedef struct UiProps UiProps;
typedef struct UiSpan UiSpan;

struct UiProps
{
    const char *id;                // names the element; an element whose macro takes what it got needs one, and a button's label is its id if it has none
    const UiProps *style;          // a style: the props of its look that it does not give itself, such as &ui.styles.note
    f64 width;                     // a fixed width, in layout units
    f64 height;                    // a fixed height, in layout units
    f64 grow;                      // a share of the room left along the parent's direction; UI_FIXED for none
    UiFlag stretch;                // takes the parent's whole cross size
    f64 padding;                   // room inside a container's edges
    f64 gap;                       // room between a container's children
    const char *align;             // where a container puts children that do not stretch: "start", "center" or "end"
    const char *background;        // a colour, such as "#202226", or a theme colour, such as "panel"
    const char *hoverBackground;   // the background while the pointer is over it
    const char *border;            // the colour of a line around it
    const char *color;             // the colour of its text
    f64 size;                      // the font size of its text
    const char *font;              // a font name of draw, such as "ui" or "mono"
    const char *tooltip;           // a text that shows when the pointer rests on it
    const char *keys;              // the key combination that does what it does, such as "Ctrl+S", which its tooltip names
    UiFlag selectable;             // UI_FALSE for a list's child that is not an item, such as a heading
    UiFlag clickable;              // a column or row that is clicked like a button; UI_CLICKED takes the click
    const char *title;             // a group's title
    UiFlag wrap;                   // text that breaks into lines at its width, or a row whose children go on more lines when they do not fit
    const char *wrapMark;          // what a text that wraps shows, dimmed, at the start of each line after a break, such as "↪ "; its lines start after it
    i32 maxLines;                  // the most lines a text shows: it wraps, and its last shown line ends with an ellipsis when more follow
    const char *gutter;            // what a text shows, dimmed, before its first line, such as its line's number; its lines start after it, and a wrapMark stands at its end
    UiFlag vertical;               // a separator up a row
    const char *label;             // a toggle's label
    const char *placeholder;       // the text an empty field shows, dimmed, until it is typed in
    const char *look;              // a toggle's look: "check" or "switch"
    f64 min;                       // a slider's or number's least value; min and max count when they differ
    f64 max;                       // a slider's or number's greatest value
    f64 step;                      // what a slider's or number's value is rounded to
    UiFlag valid;                  // UI_FALSE marks a field's text as wrong, and keeps Return from giving it
    UiFlag alpha;                  // a colour element whose picker has an alpha bar, and gives "#RRGGBBAA"
    const UiSpan *spans;           // a button's or a section's label as spans, in place of its text, which stays its id: UI_LABEL(...)
    const char *sort;              // the id of the column a table is sorted by
    UiFlag ascending;              // a table's sort arrow points up
    i32 selected;                  // a list's chosen item, from 1; UI_LIST sets it
    i32 count;                     // a long list's number of items, when it describes only the items that ui's range gives
    i32 first;                     // the number of a long list's first described item, from 1, as ui's range gives it
    bool onActivate;               // a list, table or tree whose items are activated, with a click or Return; UI_ACTIVATED takes the item
    bool onSort;                   // a table whose titles are clicked to sort; UI_SORTED takes the column
    const char *dialog;            // a path's file dialog: "openFile", "saveFile" or "openFolder"
    const char *pattern;           // the extensions a path's file dialog shows, such as "ttf;otf"
    const char *results;           // a search's list that shows what matches, which Up and Down choose in while the search has the keys
    const char *folders;           // a dropdown's separator of folders, such as ".": its choices are paths, shown a folder at a time
    void *user;                    // what onDrag, onDragOver, onDrop and onMenu get first
    UiDragFunction onDrag;         // a list's, a table's or a tree's items are dragged
    UiDragOverFunction onDragOver; // says where dropped data may land; it needs onDrop
    UiDropFunction onDrop;         // data of a type that the panel accepts is dropped on it
    UiMenuFunction onMenu;         // a release of the right button on it, such as to open Ui_Menu there
};

/// @brief ui's own styles, which its views and the first-party views use so they look alike. Ui_Init copies them from ui.styles.
typedef struct UiStyles
{
    UiProps view;    // a view's root: room around it and between its parts, on the panel's colour
    UiProps item;    // a row of a list: room around it and between its parts
    UiProps title;   // a view's title
    UiProps heading; // a heading among items, in the accent
    UiProps note;    // a note, dimmed and small
    UiProps bar;     // a row of tools along a panel's top, a header of its own controls
} UiStyles;

/// @brief How many texts, such as colours, ui's own styles hold, and how long each may be; the Ui keeps copies of them.
#define UI_STYLE_TEXTS 16
#define UI_STYLE_TEXT 32

/// @brief A column of a table: the id names its cells, and the title shows in the header, the id if it is nullptr; a width of 0 is the widest cell's.
typedef struct UiColumn
{
    const char *id;
    const char *title;
    f64 width;
} UiColumn;

/// @brief A mark of a tree's node: a short label at the end of its row, such as an eye or a lock. A list of marks ends with one whose id is nullptr.
/// on points to whether it is on, which UI_TREE writes when the mark is clicked; a click does not choose the node.
typedef struct UiTreeMark
{
    const char *id;
    const char *label;
    bool *on;
    const char *tooltip; // nullptr for none
    const char *keys;    // the key combination that turns it too, which its tooltip names; nullptr for none
    const UiSpan *spans; // the label as spans, in place of label, such as UI_LABEL({.icon = "lock"}); nullptr for the label
} UiTreeMark;

/// @brief A node of a tree. A list of nodes ends with one whose id is nullptr. A node whose children are not nullptr has an arrow, even with none,
/// and open points to whether it is open, which UI_TREE writes when the arrow is clicked.
typedef struct UiTreeNode
{
    const char *id;
    const char *label; // the id if it is nullptr
    const struct UiTreeNode *children;
    bool *open;
    const UiTreeMark *marks; // nullptr for none
    const UiSpan *spans;     // the label as spans, in place of label; nullptr for the label
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
    i32 caller;                                // the number that ui.dispatcher gave, which names the plugin's callbacks in descriptions
    bool failed;                               // memory ran out, or containers are nested too deeply; nothing shows
    UiStyles styles;                           // ui's own styles, for .style
    char texts[UI_STYLE_TEXTS][UI_STYLE_TEXT]; // the texts of the styles, which they point to
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

/// @brief The kinds of C functions that ui calls back, which a token names.
typedef enum UiCallbackKind
{
    UI__DRAG,
    UI__DRAG_OVER,
    UI__DROP,
    UI__MENU,
    UI__CLICK,
} UiCallbackKind;

/// @brief What a description gives ui for a C function: ui gives it back to the plugin's Ui__Call, which calls the function with its user.
typedef struct UiToken
{
    UiCallbackKind kind;
    void (*function)(void);
    void *user;
} UiToken;

/// @brief Sets a field of a table to a C function that ui calls back: the caller's number, and the token, as bytes that ui does not read.
static inline void Ui_SetCallback(ECSValue *table, const char *name, UiCallbackKind kind, void (*function)(void), void *user)
{
    ECSValue *callback = function == nullptr || table == nullptr ? nullptr : Ui_SetTable(table, name);

    if (callback == nullptr)
    {
        return;
    }

    UiToken token;
    memset(&token, 0, sizeof(token));
    token.kind = kind;
    token.function = function;
    token.user = user;
    Ui_SetNumber(callback, "caller", Ui_current->caller);
    ECSValue *bytes = Ui_NewField(callback, "token");

    if (bytes != nullptr && ECSValue_SetBuffer(bytes, (SHUSliceView){.data = &token, .size = sizeof(token)}))
    {
        Ui_current->failed = true;
    }
}

/// @brief Writes the props that are not left to ui into a description's props.
static inline void Ui_WriteProps(ECSValue *table, const UiProps *props, usz depth)
{
    if (table == nullptr)
    {
        return;
    }

    // a style that builds on itself stops here; ui reports the depth as a misuse
    if (props->style != nullptr && depth < UI_DEPTH)
    {
        Ui_WriteProps(Ui_SetTable(table, "style"), props->style, depth + 1);
    }

    Ui_SetString(table, "id", props->id);
    Ui_SetString(table, "align", props->align);
    Ui_SetString(table, "background", props->background);
    Ui_SetString(table, "hoverBackground", props->hoverBackground);
    Ui_SetString(table, "border", props->border);
    Ui_SetString(table, "color", props->color);
    Ui_SetString(table, "font", props->font);
    Ui_SetString(table, "tooltip", props->tooltip);
    Ui_SetString(table, "keys", props->keys);
    Ui_SetString(table, "title", props->title);
    Ui_SetString(table, "label", props->label);
    Ui_SetString(table, "placeholder", props->placeholder);
    Ui_SetString(table, "look", props->look);
    Ui_SetString(table, "sort", props->sort);
    Ui_SetString(table, "dialog", props->dialog);
    Ui_SetString(table, "pattern", props->pattern);
    Ui_SetString(table, "results", props->results);
    Ui_SetString(table, "folders", props->folders);
    Ui_SetString(table, "wrapMark", props->wrapMark);
    Ui_SetString(table, "gutter", props->gutter);

    const struct
    {
        const char *name;
        f64 number;
    } numbers[] = {{"width", props->width}, {"height", props->height}, {"padding", props->padding}, {"gap", props->gap}, {"size", props->size}, {"step", props->step}, {"selected", props->selected}, {"count", props->count}, {"first", props->first}, {"maxLines", props->maxLines}};

    for (usz i = 0; i < UI__COUNT(numbers); i++)
    {
        if (numbers[i].number != 0.0)
        {
            Ui_SetNumber(table, numbers[i].name, numbers[i].number == UI_ZERO ? 0.0 : numbers[i].number);
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
        UiFlag flag;
    } flags[] = {{"stretch", props->stretch}, {"selectable", props->selectable}, {"clickable", props->clickable}, {"wrap", props->wrap}, {"vertical", props->vertical}, {"valid", props->valid}, {"alpha", props->alpha}, {"ascending", props->ascending}, {"onActivate", props->onActivate ? UI_TRUE : UI_UNSET}, {"onSort", props->onSort ? UI_TRUE : UI_UNSET}};

    for (usz i = 0; i < UI__COUNT(flags); i++)
    {
        if (flags[i].flag != UI_UNSET)
        {
            Ui_SetBool(table, flags[i].name, flags[i].flag == UI_TRUE);
        }
    }

    Ui_SetCallback(table, "onDrag", UI__DRAG, (void (*)(void))props->onDrag, props->user);
    Ui_SetCallback(table, "onDragOver", UI__DRAG_OVER, (void (*)(void))props->onDragOver, props->user);
    Ui_SetCallback(table, "onDrop", UI__DROP, (void (*)(void))props->onDrop, props->user);
    Ui_SetCallback(table, "onMenu", UI__MENU, (void (*)(void))props->onMenu, props->user);
}

/// @brief Writes an element's props into its description, its style's among them.
static inline void Ui_SetProps(ECSValue *description, const UiProps *props)
{
    Ui_WriteProps(Ui_SetTable(description, "props"), props, 0);
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

/// @brief The names of UiWhere, as Lua's where gives them; UI_BESIDE is nil.
[[maybe_unused]] static const char *const UI__WHERE[] = {nullptr, "before", "after", "into"};

/// @brief What an onDragOver or an onDrop gets, from what ui gives Ui__Call.
static inline UiDrop Ui__Drop(const ECSValue *args)
{
    const char *where = ECSValue_GetString(ECSValue_GetTableField(args, "where"), "");
    UiDrop drop = {
        .item = ECSValue_GetTableField(args, "item"),
        .dataType = ECSValue_GetString(ECSValue_GetTableField(args, "dataType"), ""),
        .value = ECSValue_GetTableField(args, "value"),
        .x = ECSValue_GetNumber(ECSValue_GetTableField(args, "x"), 0.0),
        .y = ECSValue_GetNumber(ECSValue_GetTableField(args, "y"), 0.0),
    };

    for (usz i = 1; i < UI__COUNT(UI__WHERE); i++)
    {
        drop.where = strcmp(where, UI__WHERE[i]) == 0 ? (UiWhere)i : drop.where;
    }

    return drop;
}

/// @brief Calls the C function of a token that a description gave ui, with the props' user and what ui gives, and fills what ui gets back: onDrag's
/// type and value, onDragOver's false or new where, and nil for the others. ui.dispatcher keeps it, and ui calls it during the panel's events.
static inline void Ui__Call(SHUSlice bytes, const ECSValue *args, ECSValue *retResult)
{
    UiToken token;

    if (bytes.size != sizeof(token))
    {
        return;
    }

    memcpy(&token, bytes.data, sizeof(token));

    switch (token.kind)
    {
    case UI__DRAG:
    {
        const char *type = nullptr;
        ECSValue *value = nullptr;
        ECSValue *typeField = nullptr;
        ECSValue_SetTable(retResult);
        bool dragged = !ECSValue_SetTableField(retResult, "value", &value) && ((UiDragFunction)token.function)(token.user, ECSValue_GetTableField(args, "item"), &type, value);

        if (!dragged || type == nullptr || ECSValue_SetTableField(retResult, "type", &typeField) || ECSValue_SetString(typeField, type))
        {
            ECSValue_SetNil(retResult);
        }

        break;
    }
    case UI__DRAG_OVER:
    {
        UiDrop drop = Ui__Drop(args);
        UiWhere where = drop.where;

        if (!((UiDragOverFunction)token.function)(token.user, &drop))
        {
            ECSValue_SetBool(retResult, false);
        }
        else if (where != UI_BESIDE && drop.where != where && drop.where > UI_BESIDE && drop.where <= UI_INTO && ECSValue_SetString(retResult, UI__WHERE[drop.where]))
        {
            ECSValue_SetNil(retResult);
        }

        break;
    }
    case UI__DROP:
    {
        UiDrop drop = Ui__Drop(args);
        ((UiDropFunction)token.function)(token.user, &drop);
        break;
    }
    case UI__MENU:
        ((UiMenuFunction)token.function)(token.user, ECSValue_GetTableField(args, "item"), ECSValue_GetNumber(ECSValue_GetTableField(args, "x"), 0.0), ECSValue_GetNumber(ECSValue_GetTableField(args, "y"), 0.0));
        break;
    case UI__CLICK:
        ((UiClickFunction)token.function)(token.user);
        break;
    default:
        break;
    }
}

/// @brief Prepares a Ui for a plugin: looks up ui's functions. Call it from the plugin's Init; the plugin's manifest must depend on ui.
/// @return SHUResult_Ok, or the error of the first function that cannot be looked up.
static inline SHUWUR SHUResult Ui_Init(Ui *retUi, ECSPlugin plugin)
{
    *retUi = (Ui){.plugin = plugin};
    SHU_ReturnResult(UiFunctions_Get(plugin, &retUi->functions));
    // ui keeps the function that calls the plugin's C callbacks, and descriptions name it by its number
    retUi->caller = retUi->functions.dispatcher(Ui__Call);

    // ui gives its styles until it is asked again, so the Ui keeps copies, whose texts it holds itself
    const ECSValue *styles = retUi->functions.styles();
    const struct
    {
        const char *name;
        UiProps *props;
    } own[] = {{"view", &retUi->styles.view}, {"item", &retUi->styles.item}, {"title", &retUi->styles.title}, {"heading", &retUi->styles.heading}, {"note", &retUi->styles.note}, {"bar", &retUi->styles.bar}};
    usz texts = 0;

    for (usz i = 0; i < UI__COUNT(own); i++)
    {
        const ECSValue *style = ECSValue_GetTableField(styles, own[i].name);
        UiProps *props = own[i].props;
        const char **fields[] = {&props->align, &props->background, &props->hoverBackground, &props->border, &props->color, &props->font};
        const char *const names[] = {"align", "background", "hoverBackground", "border", "color", "font"};

        for (usz j = 0; j < UI__COUNT(fields); j++)
        {
            const char *text = ECSValue_GetString(ECSValue_GetTableField(style, names[j]), nullptr);

            if (text != nullptr && texts < UI_STYLE_TEXTS && strlen(text) < UI_STYLE_TEXT)
            {
                strcpy(retUi->texts[texts], text);
                *fields[j] = retUi->texts[texts++];
            }
        }

        props->width = ECSValue_GetNumber(ECSValue_GetTableField(style, "width"), 0.0);
        props->height = ECSValue_GetNumber(ECSValue_GetTableField(style, "height"), 0.0);
        props->padding = ECSValue_GetNumber(ECSValue_GetTableField(style, "padding"), 0.0);
        props->gap = ECSValue_GetNumber(ECSValue_GetTableField(style, "gap"), 0.0);
        props->size = ECSValue_GetNumber(ECSValue_GetTableField(style, "size"), 0.0);
        props->stretch = ECSValue_GetBool(ECSValue_GetTableField(style, "stretch"), false) ? UI_TRUE : UI_UNSET;
    }

    return SHUResult_Ok;
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
    static const char *const TYPES[] = {"pointerDown", "pointerUp", "pointerMove", "wheel", "keyDown", "keyUp", "focused", "unfocused", "shown", "hidden", "resized", "drop", "text", "dragOver", "dragLeave"};
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
    case ECSPanelEventType_DragOver:
    case ECSPanelEventType_Drop:
    {
        ECSValue *value = nullptr;
        Ui_SetNumber(table, "x", event->drop.x);
        Ui_SetNumber(table, "y", event->drop.y);
        Ui_SetString(table, "dataType", ECSDropData_GetType(event->drop.data));

        if (ECSValue_SetTableField(table, "value", &value) || ECSValue_Copy(value, ECSDropData_GetValue(event->drop.data)))
        {
            ui->failed = true;
        }

        break;
    }
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

/// @brief An entry of a menu, as ui.MenuEntry is: a label that onClick runs, a submenu of entries, or a line between entries. A list of entries
/// ends with one whose label is nullptr and that is no line.
typedef struct UiMenuEntry
{
    const char *label;
    const char *keys;                  // the keys that run the same, shown at the entry's right, such as "Ctrl+D"; nullptr for none
    UiFlag enabled;                    // UI_FALSE dims the entry, and it does not run
    UiClickFunction onClick;           // runs when the entry is clicked, or chosen with Return; the menu closes first
    void *user;                        // what onClick gets
    const struct UiMenuEntry *entries; // a submenu's entries, which open in place of the menu's; nullptr for none
    bool line;                         // a line between entries, which has nothing else
} UiMenuEntry;

/// @brief Adds a menu's entries to a list, and their submenus' entries to theirs.
static inline void Ui__MenuEntries(ECSValue *list, const UiMenuEntry *entries, usz depth)
{
    for (usz i = 0; list != nullptr && depth < UI_DEPTH && (entries[i].label != nullptr || entries[i].line); i++)
    {
        ECSValue *entry = Ui_AddItem(list);

        if (entry == nullptr)
        {
            return;
        }

        ECSValue_SetTable(entry);

        if (entries[i].line)
        {
            Ui_SetBool(entry, "line", true);
            continue;
        }

        Ui_SetString(entry, "label", entries[i].label);
        Ui_SetString(entry, "keys", entries[i].keys);

        if (entries[i].enabled != UI_UNSET)
        {
            Ui_SetBool(entry, "enabled", entries[i].enabled == UI_TRUE);
        }

        Ui_SetCallback(entry, "onClick", UI__CLICK, (void (*)(void))entries[i].onClick, entries[i].user);

        if (entries[i].entries != nullptr)
        {
            Ui__MenuEntries(Ui_SetTable(entry, "entries"), entries[i].entries, depth + 1);
        }
    }
}

/// @brief Opens a menu of entries in a popup at a point of a panel, in layout units, such as where onMenu's right click was, as ui.menu does.
static inline void Ui_Menu(Ui *ui, ECSPanel panel, f64 x, f64 y, const UiMenuEntry *entries)
{
    ECSValue *list = nullptr;

    if (ECSValue_Create(&list))
    {
        return;
    }

    // Ui_NewField reports to the current Ui
    Ui *current = Ui_current;
    Ui_current = ui;
    ui->failed = false;
    ECSValue_SetTable(list);
    Ui__MenuEntries(list, entries, 0);

    if (!ui->failed)
    {
        ui->functions.menu(panel, (f32)x, (f32)y, list);
    }

    ECSValue_Destroy(&list);
    Ui_current = current;
}

#pragma region Elements

static inline void Ui_Text(const char *text, const UiProps *props)
{
    Ui_SetString(Ui_Add("text", props), "value", text);
}

/// @brief A part of a text in its own style, or a picture a line high, as ui.Span is. A false field is off, and a colour left nullptr is the text's.
/// A list of spans that has no count, as UI_LABEL makes, ends with an empty span, whose text, icon and image are nullptr.
struct UiSpan
{
    const char *text;
    const char *icon;  // an icon in place of the text, as UI_ICON names it: eye, eyeClosed, lock or lockOpen
    const char *image; // the path of an image in place of the text, which fits a square a line high
    bool bold;
    bool italic;
    bool mono; // in the mono font, for code
    bool underline;
    bool strike;
    const char *color;
    const char *link; // underlined, in the accent; UI_SPANS gives it when it is clicked
};

/// @brief Sets a table's field to a list of spans, of a count, or until the empty span when the count is SIZE_MAX.
static inline void Ui_SetSpans(ECSValue *table, const char *name, const UiSpan *spans, usz count)
{
    ECSValue *list = table == nullptr ? nullptr : Ui_SetTable(table, name);

    for (usz i = 0; list != nullptr && i < count && (count != SIZE_MAX || spans[i].text != nullptr || spans[i].icon != nullptr || spans[i].image != nullptr); i++)
    {
        ECSValue *span = nullptr;

        if (ECSValue_AddListItem(list, &span))
        {
            Ui_current->failed = true;
            break;
        }

        ECSValue_SetTable(span);
        Ui_SetString(span, "text", spans[i].text != nullptr ? spans[i].text : "");
        Ui_SetString(span, "icon", spans[i].icon);
        Ui_SetString(span, "image", spans[i].image);
        Ui_SetString(span, "color", spans[i].color);
        Ui_SetString(span, "link", spans[i].link);
        const struct
        {
            const char *name;
            bool on;
        } flags[] = {{"bold", spans[i].bold}, {"italic", spans[i].italic}, {"mono", spans[i].mono}, {"underline", spans[i].underline}, {"strike", spans[i].strike}};

        for (usz j = 0; j < UI__COUNT(flags); j++)
        {
            if (flags[j].on)
            {
                Ui_SetBool(span, flags[j].name, true);
            }
        }
    }
}

/// @brief Sets a table's field to a label: its spans when there are, or else its text.
static inline void Ui_SetLabel(ECSValue *table, const char *name, const char *text, const UiSpan *spans)
{
    if (spans != nullptr)
    {
        Ui_SetSpans(table, name, spans, SIZE_MAX);
    }
    else
    {
        Ui_SetString(table, name, text);
    }
}

/// @brief A text of spans, each in its own style. Gives the link that a click, or Return, gave since the last draw, or nullptr; it is valid until the
/// next take. A text without an id takes the links of every such text.
static inline const char *Ui_Spans(const UiSpan *spans, usz count, const UiProps *props)
{
    const char *link = ECSValue_GetString(ECSValue_GetTableField(Ui_Take(props->id != nullptr ? props->id : "", "onLink"), "value"), nullptr);
    Ui_SetSpans(Ui_Add("text", props), "value", spans, count);
    return link;
}

static inline bool Ui_Button(const char *label, const UiProps *props)
{
    UiProps named = *props;
    named.id = named.id == nullptr ? label : named.id;
    Ui_SetLabel(Ui_Add("button", &named), "value", label, props->spans);
    return Ui_Take(named.id, "onClick") != nullptr;
}

static inline bool Ui_Toggle(bool *on, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onChange");
    *on = taken != nullptr ? ECSValue_GetBool(ECSValue_GetTableField(taken, "value"), *on) : *on;
    Ui_SetBool(Ui_Add("toggle", props), "value", *on);
    return taken != nullptr;
}

/// @brief A slider or a number field: type is "slider" or "number". A number field's value changes at each edit that is a number.
static inline bool Ui_Number(const char *type, f64 *value, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onChange");
    *value = taken != nullptr ? ECSValue_GetNumber(ECSValue_GetTableField(taken, "value"), *value) : *value;
    Ui_SetNumber(Ui_Add(type, props), "value", *value);
    return taken != nullptr;
}

/// @brief Whether Return was pressed in the number field with an id, or a drag of the slider with an id ended, since the last draw.
static inline bool Ui_Submitted(const char *id)
{
    return Ui_Take(id, "onSubmit") != nullptr;
}

/// @brief A selector, tabs or a dropdown: type is "selector", "tabs" or "dropdown". The choices end with nullptr, and selected is the chosen one's
/// number, from 1; 0 for none.
static inline bool Ui_Choice(const char *type, i32 *selected, const char *const *choices, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onSelect");
    *selected = taken != nullptr ? (i32)ECSValue_GetInteger(ECSValue_GetTableField(taken, "value"), *selected) : *selected;
    ECSValue *description = Ui_Add(type, props);
    Ui_SetNumber(description, "selected", *selected);
    ECSValue *list = Ui_SetTable(description, "choices");

    for (i32 i = 0; choices[i] != nullptr; i++)
    {
        Ui_AddString(list, choices[i]);
    }

    return taken != nullptr;
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

/// @brief A path of a file in a buffer of a capacity, which each edit and the file dialog of its button change.
/// @return Whether Return was pressed in it, or a file was chosen.
static inline bool Ui_Path(char *buffer, usz capacity, const UiProps *props)
{
    Ui_CopyText(buffer, capacity, ECSValue_GetString(ECSValue_GetTableField(Ui_Take(props->id, "onChange"), "value"), nullptr));
    const ECSValue *submitted = Ui_Take(props->id, "onSubmit");
    Ui_CopyText(buffer, capacity, ECSValue_GetString(ECSValue_GetTableField(submitted, "value"), nullptr));
    Ui_SetString(Ui_Add("path", props), "value", buffer);
    return submitted != nullptr;
}

/// @brief A search field, whose query is in a buffer of a capacity, which each edit and its clear button change. Ui_Matches tells which texts match.
/// @return Whether the query changed.
static inline bool Ui_Search(char *buffer, usz capacity, const UiProps *props)
{
    const ECSValue *changed = Ui_Take(props->id, "onChange");
    Ui_CopyText(buffer, capacity, ECSValue_GetString(ECSValue_GetTableField(changed, "value"), nullptr));
    Ui_SetString(Ui_Add("search", props), "value", buffer);
    return changed != nullptr;
}

/// @brief Whether a text matches a search, as ui.matches tells: each word of the query, whatever its case, is somewhere in it; an empty query matches
/// every text. Several texts of an item are joined with new lines, so that no word matches across two of them.
static inline bool Ui_Matches(const char *query, const char *text)
{
    for (const char *word = query; word != nullptr && *word != '\0';)
    {
        while (*word == ' ' || *word == '\t' || *word == '\n')
        {
            word++;
        }

        usz length = 0;

        while (word[length] != '\0' && word[length] != ' ' && word[length] != '\t' && word[length] != '\n')
        {
            length++;
        }

        bool found = length == 0;

        for (const char *at = text; !found && at != nullptr && *at != '\0'; at++)
        {
            usz i = 0;

            while (i < length && at[i] != '\0' && tolower((unsigned char)at[i]) == tolower((unsigned char)word[i]))
            {
                i++;
            }

            found = i == length;
        }

        if (!found)
        {
            return false;
        }

        word += length;
    }

    return true;
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

/// @brief Opens a section: a heading of a title with an arrow, over the next descriptions, until Ui_Close. Whether it is open is in open, which a
/// click on the heading changes; ui leaves out its children while it is closed, so the plugin may skip describing them. Its props need an id.
/// @return Whether a click changed open.
static inline bool Ui_OpenSection(bool *open, const char *title, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onToggle");
    *open = taken != nullptr ? ECSValue_GetBool(ECSValue_GetTableField(taken, "value"), *open) : *open;
    ECSValue *description = Ui_Open("section", props);
    Ui_SetLabel(description, "value", title, props->spans);
    Ui_SetBool(description, "open", *open);
    return taken != nullptr;
}

/// @brief A colour, "#RRGGBB" or "#RRGGBBAA", in a buffer of a capacity, which the picker that a click opens changes; 10 bytes hold any.
/// @return Whether it changed.
static inline bool Ui_Color(char *buffer, usz capacity, const UiProps *props)
{
    const ECSValue *taken = Ui_Take(props->id, "onChange");
    Ui_CopyText(buffer, capacity, ECSValue_GetString(ECSValue_GetTableField(taken, "value"), nullptr));
    Ui_SetString(Ui_Add("color", props), "value", buffer);
    return taken != nullptr;
}

/// @brief The item of a list or the row of a table that was activated, with a click or Return, from 1, or 0 if none was. Its props need onActivate = true.
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
/// The table's props need onSort = true.
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
        Ui_SetLabel(item, "label", node->label, node->spans);

        if (node->children != nullptr)
        {
            Ui_AddTreeNodes(Ui_SetTable(item, "children"), open, node->children);
        }

        if (node->marks != nullptr)
        {
            ECSValue *marks = Ui_SetTable(item, "marks");

            for (const UiTreeMark *mark = node->marks; mark->id != nullptr; mark++)
            {
                ECSValue *described = Ui_AddItem(marks);

                if (described == nullptr)
                {
                    break;
                }

                ECSValue_SetTable(described);
                Ui_SetString(described, "id", mark->id);
                Ui_SetLabel(described, "label", mark->label, mark->spans);
                Ui_SetBool(described, "on", mark->on != nullptr && *mark->on);
                Ui_SetString(described, "tooltip", mark->tooltip);
                Ui_SetString(described, "keys", mark->keys);
            }
        }

        if (node->open != nullptr && *node->open)
        {
            Ui_SetBool(open, node->id, true);
        }
    }
}

/// @brief Finds the mark of an id among a node's marks, or nullptr.
static inline const UiTreeMark *Ui_FindTreeMark(const UiTreeNode *node, const char *id)
{
    for (const UiTreeMark *mark = node != nullptr ? node->marks : nullptr; id != nullptr && mark != nullptr && mark->id != nullptr; mark++)
    {
        if (strcmp(mark->id, id) == 0)
        {
            return mark;
        }
    }

    return nullptr;
}

/// @brief A tree of nodes, which end with one whose id is nullptr, which builds only the rows in view and near it. selected points to the chosen node's id, one of the nodes' own, or nullptr; a
/// click on a node's arrow writes its new state into its open, and a click on a node's mark writes its new state into its on.
/// @return Whether a node was chosen, opened or closed, or a mark turned since the last draw.
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

    const ECSValue *marked = Ui_Take(props->id, "onMark");
    const UiTreeMark *mark = Ui_FindTreeMark(Ui_FindTreeNode(nodes, ECSValue_GetString(ECSValue_GetTableField(marked, "value"), nullptr)),
                                             ECSValue_GetString(ECSValue_GetTableField(marked, "mark"), nullptr));

    if (mark != nullptr && mark->on != nullptr)
    {
        *mark->on = ECSValue_GetBool(ECSValue_GetTableField(marked, "on"), false);
    }

    ECSValue *description = Ui_Add("tree", props);
    Ui_SetString(description, "selected", *selected);
    Ui_AddTreeNodes(Ui_SetTable(description, "nodes"), Ui_SetTable(description, "open"), nodes);
    return chosen != nullptr || opened != nullptr || mark != nullptr;
}

/// @brief The id of the node of the tree with an id that was activated, with a click or Return, one of the nodes' own, or nullptr if none was.
/// The tree's props need onActivate = true.
static inline const char *Ui_ActivatedNode(const char *id, const UiTreeNode *nodes)
{
    const UiTreeNode *node = Ui_FindTreeNode(nodes, ECSValue_GetString(ECSValue_GetTableField(Ui_Take(id, "onActivate"), "value"), nullptr));
    return node != nullptr ? node->id : nullptr;
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
#define UI_SECTION(open, title, ...) for (int UI__LATCH = (Ui_OpenSection((open), (title), &(UiProps){__VA_ARGS__}), 0); UI__LATCH < 1; UI__LATCH++, Ui_Close())
#define UI_LIST(selected, ...) for (int UI__LATCH = (Ui_OpenList((selected), &(UiProps){__VA_ARGS__}), 0); UI__LATCH < 1; UI__LATCH++, Ui_Close())

/// @brief Elements without children. Those that take a pointer write what the user changed into it, and give whether it changed.
#define UI_TEXT(text, ...) Ui_Text((text), &(UiProps){__VA_ARGS__})
#define UI_SPANS(spans, count, ...) Ui_Spans((spans), (count), &(UiProps){__VA_ARGS__})
#define UI_BUTTON(label, ...) Ui_Button((label), &(UiProps){__VA_ARGS__})
#define UI_TOGGLE(on, ...) Ui_Toggle((on), &(UiProps){__VA_ARGS__})
#define UI_SLIDER(value, ...) Ui_Number("slider", (value), &(UiProps){__VA_ARGS__})
#define UI_NUMBER(value, ...) Ui_Number("number", (value), &(UiProps){__VA_ARGS__})
#define UI_SELECTOR(selected, choices, ...) Ui_Choice("selector", (selected), (choices), &(UiProps){__VA_ARGS__})
#define UI_TABS(selected, choices, ...) Ui_Choice("tabs", (selected), (choices), &(UiProps){__VA_ARGS__})
#define UI_DROPDOWN(selected, choices, ...) Ui_Choice("dropdown", (selected), (choices), &(UiProps){__VA_ARGS__})
#define UI_FIELD(buffer, capacity, ...) Ui_Field((buffer), (capacity), &(UiProps){__VA_ARGS__})
#define UI_KEY(buffer, capacity, ...) Ui_Key((buffer), (capacity), &(UiProps){__VA_ARGS__})
#define UI_PATH(buffer, capacity, ...) Ui_Path((buffer), (capacity), &(UiProps){__VA_ARGS__})
#define UI_SEARCH(buffer, capacity, ...) Ui_Search((buffer), (capacity), &(UiProps){__VA_ARGS__})
#define UI_COLOR(buffer, capacity, ...) Ui_Color((buffer), (capacity), &(UiProps){__VA_ARGS__})
#define UI_TABLE(selected, columns, cells, rowCount, ...) Ui_Table((selected), (columns), (cells), (rowCount), &(UiProps){__VA_ARGS__})
#define UI_TREE(selected, nodes, ...) Ui_Tree((selected), (nodes), &(UiProps){__VA_ARGS__})
#define UI_SEPARATOR(...) ((void)Ui_Add("separator", &(UiProps){__VA_ARGS__}))
#define UI_SPACE(...) ((void)Ui_Add("space", &(UiProps){__VA_ARGS__}))
#define UI_IMAGE(path, ...) Ui_SetString(Ui_Add("image", &(UiProps){__VA_ARGS__}), "value", (path))
#define UI_LABEL(...) ((const UiSpan[]){__VA_ARGS__, {0}})
#define UI_ICON(name, ...) Ui_SetString(Ui_Add("icon", &(UiProps){__VA_ARGS__}), "value", (name))

/// @brief Whether the clickable column or row with an id was clicked since the last draw.
#define UI_CLICKED(id) (Ui_Take((id), "onClick") != nullptr)

/// @brief The item of the list with an id that was activated since the last draw, from 1, or 0.
#define UI_ACTIVATED(id) Ui_Activated(id)

/// @brief The id of the node of the tree with an id that was activated since the last draw, or nullptr.
#define UI_ACTIVATED_NODE(id, nodes) Ui_ActivatedNode((id), (nodes))

/// @brief Whether Return was pressed in the number field with an id, or a drag of the slider with an id ended, since the last draw.
#define UI_SUBMITTED(id) Ui_Submitted(id)

/// @brief The id of the column whose title of the table with an id was clicked since the last draw, or nullptr.
#define UI_SORTED(id) Ui_Sorted(id)
