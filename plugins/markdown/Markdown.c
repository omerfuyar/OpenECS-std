// The markdown standard plugin: reads Markdown into a tree of tables, and into ui's descriptions of elements, for notes and help panels (DESIGN 26).
// md4c's code is compiled into this file (DESIGN 1).

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include "md4c.c"

#pragma region Source Only

/// @brief A node that the reader is in: its table, its children, and the text that waits to be added to it.
typedef struct MarkdownFrame
{
    ECSValue *node;
    ECSValue *children; // nullptr for a node whose text is a field, such as a code block's
    bool literal;       // its text goes into its own text field
    bool skipped;       // a block of md4c's that the tree leaves out, such as a table's body; its children go to the node above
} MarkdownFrame;

/// @brief What the reader keeps while md4c reads.
typedef struct MarkdownReader
{
    MarkdownFrame frames[64]; // deeper nodes go to the deepest one
    int depth;                // the index of the current frame
    int ignored;              // frames past the last, which are not kept
    char *text;               // the text that waits
    usz length;
    usz capacity;
    bool head;   // in a table's head
    bool failed; // out of memory
} MarkdownReader;

static void MarkdownAddText(MarkdownReader *reader, const char *text, usz size)
{
    if (reader->length + size + 1 > reader->capacity)
    {
        usz capacity = SDL_max(reader->capacity * 2, reader->length + size + 64);
        char *grown = SDL_realloc(reader->text, capacity);

        if (grown == nullptr)
        {
            reader->failed = true;
            return;
        }

        reader->text = grown;
        reader->capacity = capacity;
    }

    SDL_memcpy(reader->text + reader->length, text, size);
    reader->length += size;
    reader->text[reader->length] = '\0';
}

/// @brief Sets a field of a table to a text; nothing happens without memory, which the reader then remembers.
static void MarkdownSetText(MarkdownReader *reader, ECSValue *table, const char *name, const char *text, usz size)
{
    if (table == nullptr)
    {
        return;
    }

    ECSValue *field = nullptr;
    char *copy = SDL_strndup(text, size);

    if (copy == nullptr || ECSValue_SetTableField(table, name, &field) || ECSValue_SetString(field, copy))
    {
        reader->failed = true;
    }

    SDL_free(copy);
}

/// @brief Adds a child node of a type to the current node.
/// @return The child, or nullptr without memory.
static ECSValue *MarkdownAddNode(MarkdownReader *reader, const char *type)
{
    MarkdownFrame *frame = &reader->frames[reader->depth];
    ECSValue *child = nullptr;

    if (frame->children == nullptr || ECSValue_AddListItem(frame->children, &child))
    {
        reader->failed = reader->failed || frame->children != nullptr;
        return nullptr;
    }

    MarkdownSetText(reader, child, "type", type, SDL_strlen(type));
    return child;
}

/// @brief Puts the waiting text into the current node: into its text field, or as a text node after its last child.
static void MarkdownFlush(MarkdownReader *reader)
{
    if (reader->length == 0)
    {
        return;
    }

    MarkdownFrame *frame = &reader->frames[reader->depth];

    if (frame->literal)
    {
        MarkdownSetText(reader, frame->node, "text", reader->text, reader->length);
    }
    else
    {
        ECSValue *node = MarkdownAddNode(reader, "text");

        if (node != nullptr)
        {
            MarkdownSetText(reader, node, "text", reader->text, reader->length);
        }
    }

    reader->length = 0;
}

/// @brief Enters a node: a new child of the current node, or, with a nullptr type, md4c's block that the tree leaves out.
/// @param literal Its text goes into its text field, as a code block's does.
static ECSValue *MarkdownEnter(MarkdownReader *reader, const char *type, bool literal)
{
    MarkdownFlush(reader);

    if (reader->ignored > 0 || reader->depth + 1 >= (int)SDL_arraysize(reader->frames))
    {
        reader->ignored++;
        return nullptr;
    }

    MarkdownFrame *parent = &reader->frames[reader->depth];
    MarkdownFrame frame = {.node = parent->node, .children = parent->children, .skipped = true};

    if (type != nullptr)
    {
        ECSValue *node = MarkdownAddNode(reader, type);
        ECSValue *children = nullptr;

        if (node != nullptr && !literal && ECSValue_SetTableField(node, "children", &children))
        {
            reader->failed = true;
        }

        if (children != nullptr)
        {
            ECSValue_SetTable(children);
        }

        frame = (MarkdownFrame){.node = node, .children = children, .literal = literal};
    }

    reader->frames[++reader->depth] = frame;
    return type == nullptr ? nullptr : frame.node;
}

static void MarkdownLeave(MarkdownReader *reader)
{
    if (reader->ignored > 0)
    {
        reader->ignored--;
        return;
    }

    MarkdownFlush(reader);

    if (reader->depth > 0)
    {
        reader->depth--;
    }
}

/// @brief Sets a field of a node to an attribute of md4c, such as a link's address.
static void MarkdownSetAttribute(MarkdownReader *reader, ECSValue *node, const char *name, const MD_ATTRIBUTE *attribute)
{
    if (node != nullptr && attribute->text != nullptr && attribute->size > 0)
    {
        MarkdownSetText(reader, node, name, attribute->text, attribute->size);
    }
}

static void MarkdownSetInteger(MarkdownReader *reader, ECSValue *node, const char *name, i64 integer)
{
    ECSValue *field = nullptr;

    if (node != nullptr && ECSValue_SetTableField(node, name, &field))
    {
        reader->failed = true;
    }
    else if (field != nullptr)
    {
        ECSValue_SetInteger(field, integer);
    }
}

static void MarkdownSetBool(MarkdownReader *reader, ECSValue *node, const char *name, bool boolean)
{
    ECSValue *field = nullptr;

    if (node != nullptr && ECSValue_SetTableField(node, name, &field))
    {
        reader->failed = true;
    }
    else if (field != nullptr)
    {
        ECSValue_SetBool(field, boolean);
    }
}

static int MarkdownEnterBlock(MD_BLOCKTYPE type, void *detail, void *data)
{
    MarkdownReader *reader = data;
    static const char *const ALIGNS[] = {"default", "left", "center", "right"};

    switch (type)
    {
    case MD_BLOCK_QUOTE:
        MarkdownEnter(reader, "quote", false);
        break;
    case MD_BLOCK_UL:
    {
        ECSValue *node = MarkdownEnter(reader, "list", false);
        MarkdownSetBool(reader, node, "ordered", false);
        MarkdownSetBool(reader, node, "tight", ((MD_BLOCK_UL_DETAIL *)detail)->is_tight != 0);
        break;
    }
    case MD_BLOCK_OL:
    {
        ECSValue *node = MarkdownEnter(reader, "list", false);
        MarkdownSetBool(reader, node, "ordered", true);
        MarkdownSetBool(reader, node, "tight", ((MD_BLOCK_OL_DETAIL *)detail)->is_tight != 0);
        MarkdownSetInteger(reader, node, "start", ((MD_BLOCK_OL_DETAIL *)detail)->start);
        break;
    }
    case MD_BLOCK_LI:
    {
        const MD_BLOCK_LI_DETAIL *item = detail;
        ECSValue *node = MarkdownEnter(reader, "item", false);

        if (item->is_task)
        {
            MarkdownSetBool(reader, node, "checked", item->task_mark != ' ');
        }

        break;
    }
    case MD_BLOCK_HR:
        MarkdownEnter(reader, "rule", true);
        break;
    case MD_BLOCK_H:
        MarkdownSetInteger(reader, MarkdownEnter(reader, "heading", false), "level", ((MD_BLOCK_H_DETAIL *)detail)->level);
        break;
    case MD_BLOCK_CODE:
        MarkdownSetAttribute(reader, MarkdownEnter(reader, "codeBlock", true), "language", &((MD_BLOCK_CODE_DETAIL *)detail)->lang);
        break;
    case MD_BLOCK_HTML:
        MarkdownEnter(reader, "html", true);
        break;
    case MD_BLOCK_P:
        MarkdownEnter(reader, "paragraph", false);
        break;
    case MD_BLOCK_TABLE:
        MarkdownEnter(reader, "table", false);
        break;
    case MD_BLOCK_THEAD:
        reader->head = true;
        MarkdownEnter(reader, nullptr, false);
        break;
    case MD_BLOCK_TR:
        MarkdownSetBool(reader, MarkdownEnter(reader, "row", false), "head", reader->head);
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
    {
        MD_ALIGN align = ((MD_BLOCK_TD_DETAIL *)detail)->align;
        ECSValue *node = MarkdownEnter(reader, "cell", false);

        if (node != nullptr && (usz)align < SDL_arraysize(ALIGNS))
        {
            MarkdownSetText(reader, node, "align", ALIGNS[align], SDL_strlen(ALIGNS[align]));
        }

        break;
    }
    default:
        // the document is the root, which is entered already, and the others give their children to the node above
        if (type != MD_BLOCK_DOC)
        {
            MarkdownEnter(reader, nullptr, false);
        }

        break;
    }

    return reader->failed ? -1 : 0;
}

static int MarkdownLeaveBlock(MD_BLOCKTYPE type, void *detail, void *data)
{
    (void)detail;
    MarkdownReader *reader = data;

    if (type == MD_BLOCK_THEAD)
    {
        reader->head = false;
    }

    if (type != MD_BLOCK_DOC)
    {
        MarkdownLeave(reader);
    }

    return reader->failed ? -1 : 0;
}

static int MarkdownEnterSpan(MD_SPANTYPE type, void *detail, void *data)
{
    MarkdownReader *reader = data;

    switch (type)
    {
    case MD_SPAN_EM:
        MarkdownEnter(reader, "emphasis", false);
        break;
    case MD_SPAN_STRONG:
        MarkdownEnter(reader, "strong", false);
        break;
    case MD_SPAN_DEL:
        MarkdownEnter(reader, "strike", false);
        break;
    case MD_SPAN_CODE:
        MarkdownEnter(reader, "code", true);
        break;
    case MD_SPAN_A:
    {
        ECSValue *node = MarkdownEnter(reader, "link", false);
        MarkdownSetAttribute(reader, node, "url", &((MD_SPAN_A_DETAIL *)detail)->href);
        MarkdownSetAttribute(reader, node, "title", &((MD_SPAN_A_DETAIL *)detail)->title);
        break;
    }
    case MD_SPAN_IMG:
    {
        ECSValue *node = MarkdownEnter(reader, "image", false);
        MarkdownSetAttribute(reader, node, "url", &((MD_SPAN_IMG_DETAIL *)detail)->src);
        MarkdownSetAttribute(reader, node, "title", &((MD_SPAN_IMG_DETAIL *)detail)->title);
        break;
    }
    default:
        MarkdownEnter(reader, nullptr, false);
        break;
    }

    return reader->failed ? -1 : 0;
}

static int MarkdownLeaveSpan(MD_SPANTYPE type, void *detail, void *data)
{
    (void)type;
    (void)detail;
    MarkdownLeave(data);
    return ((MarkdownReader *)data)->failed ? -1 : 0;
}

/// @brief Adds an entity's character, such as &amp; or &#233;, to the waiting text; an entity it does not know stays as it is.
static void MarkdownAddEntity(MarkdownReader *reader, const char *text, usz size)
{
    static const struct
    {
        const char *entity;
        const char *character;
    } NAMED[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", "\xC2\xA0"}, {"&copy;", "\xC2\xA9"}, {"&mdash;", "\xE2\x80\x94"}, {"&ndash;", "\xE2\x80\x93"}, {"&hellip;", "\xE2\x80\xA6"}};

    for (usz i = 0; i < SDL_arraysize(NAMED); i++)
    {
        if (SDL_strlen(NAMED[i].entity) == size && SDL_strncmp(NAMED[i].entity, text, size) == 0)
        {
            MarkdownAddText(reader, NAMED[i].character, SDL_strlen(NAMED[i].character));
            return;
        }
    }

    if (size > 3 && text[1] == '#')
    {
        bool hex = text[2] == 'x' || text[2] == 'X';
        u32 code = (u32)SDL_strtoul(text + (hex ? 3 : 2), nullptr, hex ? 16 : 10);
        char utf8[5] = {0};
        char *end = SDL_UCS4ToUTF8(code == 0 || code > 0x10FFFF ? 0xFFFD : code, utf8);
        MarkdownAddText(reader, utf8, (usz)(end - utf8));
        return;
    }

    MarkdownAddText(reader, text, size);
}

static int MarkdownText(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *data)
{
    MarkdownReader *reader = data;

    if (reader->ignored > 0)
    {
        return 0;
    }

    switch (type)
    {
    case MD_TEXT_BR:
        MarkdownFlush(reader);
        MarkdownAddNode(reader, "break");
        break;
    case MD_TEXT_SOFTBR:
        MarkdownAddText(reader, " ", 1);
        break;
    case MD_TEXT_NULLCHAR:
        MarkdownAddText(reader, "\xEF\xBF\xBD", 3);
        break;
    case MD_TEXT_ENTITY:
        MarkdownAddEntity(reader, text, size);
        break;
    default:
        MarkdownAddText(reader, text, size);
        break;
    }

    return reader->failed ? -1 : 0;
}

/// @brief Reads Markdown into a document node.
/// @return false without memory; the document is nil then.
static bool MarkdownRead(const char *text, usz size, ECSValue *document)
{
    MarkdownReader reader = {0};
    ECSValue *children = nullptr;
    ECSValue_SetTable(document);
    MarkdownSetText(&reader, document, "type", "document", 8);

    if (reader.failed || ECSValue_SetTableField(document, "children", &children))
    {
        ECSValue_SetNil(document);
        return false;
    }

    ECSValue_SetTable(children);
    reader.frames[0] = (MarkdownFrame){.node = document, .children = children};

    const MD_PARSER parser = {
        .flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS,
        .enter_block = MarkdownEnterBlock,
        .leave_block = MarkdownLeaveBlock,
        .enter_span = MarkdownEnterSpan,
        .leave_span = MarkdownLeaveSpan,
        .text = MarkdownText,
    };

    int failed = md_parse(text, (MD_SIZE)size, &parser, &reader);
    MarkdownFlush(&reader);
    SDL_free(reader.text);

    if (failed != 0 || reader.failed)
    {
        ECSValue_SetNil(document);
        return false;
    }

    return true;
}

static const char *MarkdownTypeOf(const ECSValue *node)
{
    return ECSValue_GetString(ECSValue_GetTableField(node, "type"), "");
}

/// @brief Adds a node's text, and its children's, to a buffer, as a text element shows it: a hard break is a new line, and an image its description.
static void MarkdownAddPlainText(MarkdownReader *buffer, const ECSValue *node)
{
    const char *type = MarkdownTypeOf(node);
    const char *text = ECSValue_GetString(ECSValue_GetTableField(node, "text"), nullptr);

    if (SDL_strcmp(type, "break") == 0)
    {
        MarkdownAddText(buffer, "\n", 1);
    }
    else if (text != nullptr)
    {
        MarkdownAddText(buffer, text, SDL_strlen(text));
    }

    const ECSValue *children = ECSValue_GetTableField(node, "children");

    for (usz i = 0; i < ECSValue_GetListCount(children); i++)
    {
        MarkdownAddPlainText(buffer, ECSValue_GetListItem(children, i));
    }
}

/// @brief Makes a description of ui's: its type, and its props as a table.
/// @return The props, to be filled, or nullptr without memory.
static ECSValue *MarkdownDescribe(MarkdownReader *reader, ECSValue *description, const char *type)
{
    ECSValue *props = nullptr;
    ECSValue_SetTable(description);
    MarkdownSetText(reader, description, "type", type, SDL_strlen(type));

    if (ECSValue_SetTableField(description, "props", &props))
    {
        reader->failed = true;
        return nullptr;
    }

    ECSValue_SetTable(props);
    return props;
}

/// @brief Adds a description to a container's children.
static ECSValue *MarkdownAddChild(MarkdownReader *reader, ECSValue *container)
{
    ECSValue *children = nullptr;
    ECSValue *child = nullptr;

    if (ECSValue_GetTableField(container, "children") == nullptr && ECSValue_SetTableField(container, "children", &children) == SHUResult_Ok)
    {
        ECSValue_SetTable(children);
    }

    children = (ECSValue *)ECSValue_GetTableField(container, "children");

    if (children == nullptr || ECSValue_AddListItem(children, &child))
    {
        reader->failed = true;
        return nullptr;
    }

    return child;
}

/// @brief Describes a text element in a size, 0 for the default one, and a colour; it wraps, unless it has a font, as code keeps its lines.
static void MarkdownDescribeText(MarkdownReader *reader, ECSValue *description, const char *text, usz length, i64 size, const char *color, const char *font)
{
    ECSValue *props = MarkdownDescribe(reader, description, "text");
    MarkdownSetText(reader, description, "value", text == nullptr ? "" : text, length);

    if (props == nullptr)
    {
        return;
    }

    MarkdownSetBool(reader, props, "wrap", font == nullptr);
    MarkdownSetText(reader, props, "color", color, SDL_strlen(color));

    if (size > 0)
    {
        MarkdownSetInteger(reader, props, "size", size);
    }

    if (font != nullptr)
    {
        MarkdownSetText(reader, props, "font", font, SDL_strlen(font));
    }
}

static void MarkdownDescribeBlock(MarkdownReader *reader, ECSValue *description, const ECSValue *node, const char *color);

/// @brief The style that the nodes around a part of a line give it, as ui's spans name it.
typedef struct MarkdownStyle
{
    bool bold;
    bool italic;
    bool strike;
    const char *link; // the address of the link around it, or nullptr
} MarkdownStyle;

/// @brief Adds a span of ui's: a text in a style, with mono for code.
static void MarkdownAddSpan(MarkdownReader *reader, ECSValue *spans, const char *text, MarkdownStyle style, bool mono)
{
    ECSValue *span = nullptr;

    if (ECSValue_AddListItem(spans, &span))
    {
        reader->failed = true;
        return;
    }

    ECSValue_SetTable(span);
    MarkdownSetText(reader, span, "text", text, SDL_strlen(text));
    const struct
    {
        const char *name;
        bool on;
    } FLAGS[] = {{"bold", style.bold}, {"italic", style.italic}, {"strike", style.strike}, {"mono", mono}};

    for (usz i = 0; i < SDL_arraysize(FLAGS); i++)
    {
        if (FLAGS[i].on)
        {
            MarkdownSetBool(reader, span, FLAGS[i].name, true);
        }
    }

    if (style.link != nullptr)
    {
        MarkdownSetText(reader, span, "link", style.link, SDL_strlen(style.link));
    }
}

/// @brief Adds the spans of a part of a line, and of the parts inside it, in the style of the nodes around them.
static void MarkdownAddSpans(MarkdownReader *reader, ECSValue *spans, const ECSValue *node, MarkdownStyle style)
{
    const char *type = MarkdownTypeOf(node);
    const char *text = ECSValue_GetString(ECSValue_GetTableField(node, "text"), nullptr);

    if (SDL_strcmp(type, "break") == 0)
    {
        MarkdownAddSpan(reader, spans, "\n", style, false);
        return;
    }

    if (text != nullptr)
    {
        MarkdownAddSpan(reader, spans, text, style, SDL_strcmp(type, "code") == 0);
        return;
    }

    style.bold = style.bold || SDL_strcmp(type, "strong") == 0;
    style.italic = style.italic || SDL_strcmp(type, "emphasis") == 0;
    style.strike = style.strike || SDL_strcmp(type, "strike") == 0;

    if (SDL_strcmp(type, "link") == 0)
    {
        style.link = ECSValue_GetString(ECSValue_GetTableField(node, "url"), "");
    }

    const ECSValue *children = ECSValue_GetTableField(node, "children");

    for (usz i = 0; i < ECSValue_GetListCount(children); i++)
    {
        MarkdownAddSpans(reader, spans, ECSValue_GetListItem(children, i), style);
    }
}

/// @brief Describes a wrapped text element of spans: the parts of a line from first to before end, in a size, 0 for the default one, and a colour.
static void MarkdownDescribeSpans(MarkdownReader *reader, ECSValue *description, const ECSValue *parts, usz first, usz end, i64 size, const char *color)
{
    ECSValue *props = MarkdownDescribe(reader, description, "text");
    ECSValue *spans = nullptr;

    if (props == nullptr || ECSValue_SetTableField(description, "value", &spans))
    {
        reader->failed = true;
        return;
    }

    ECSValue_SetTable(spans);

    for (usz i = first; i < end; i++)
    {
        MarkdownAddSpans(reader, spans, ECSValue_GetListItem(parts, i), (MarkdownStyle){0});
    }

    MarkdownSetBool(reader, props, "wrap", true);
    MarkdownSetText(reader, props, "color", color, SDL_strlen(color));

    if (size > 0)
    {
        MarkdownSetInteger(reader, props, "size", size);
    }
}

/// @brief Describes a text element of a node's parts of a line, in their styles.
static void MarkdownDescribeNode(MarkdownReader *reader, ECSValue *description, const ECSValue *node, i64 size, const char *color)
{
    const ECSValue *children = ECSValue_GetTableField(node, "children");
    MarkdownDescribeSpans(reader, description, children, 0, ECSValue_GetListCount(children), size, color);
}

/// @brief Describes a text element of a node's plain text, such as an HTML block's, in a font, which keeps its lines.
static void MarkdownDescribePlain(MarkdownReader *reader, ECSValue *description, const ECSValue *node, const char *color, const char *font)
{
    MarkdownReader buffer = {0};
    MarkdownAddPlainText(&buffer, node);
    MarkdownDescribeText(reader, description, buffer.text, buffer.length, 0, color, font);
    reader->failed = reader->failed || buffer.failed;
    SDL_free(buffer.text);
}

/// @brief Tells whether a node is a block, which gets an element of its own, and not a part of a line, such as a link.
static bool MarkdownIsBlock(const char *type)
{
    static const char *const BLOCKS[] = {"paragraph", "heading", "list", "quote", "codeBlock", "rule", "table", "html"};

    for (usz i = 0; i < SDL_arraysize(BLOCKS); i++)
    {
        if (SDL_strcmp(type, BLOCKS[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

/// @brief Describes a run of a node's parts of a line that are not in a block, as in a tight list's item, as one paragraph.
static void MarkdownDescribeRun(MarkdownReader *reader, ECSValue *column, const ECSValue *parts, usz first, usz end, const char *color)
{
    ECSValue *described = first < end ? MarkdownAddChild(reader, column) : nullptr;

    if (described != nullptr)
    {
        MarkdownDescribeSpans(reader, described, parts, first, end, 0, color);
    }
}

/// @brief Describes a column of a node's blocks; runs of text that are not in a block, as in a tight list's item, become a paragraph.
static void MarkdownDescribeBlocks(MarkdownReader *reader, ECSValue *column, const ECSValue *node, const char *color, i64 gap)
{
    ECSValue *props = MarkdownDescribe(reader, column, "column");
    MarkdownSetInteger(reader, props, "gap", gap);
    MarkdownSetBool(reader, props, "stretch", true);

    const ECSValue *children = ECSValue_GetTableField(node, "children");
    usz runStart = 0; // the first of the parts of a line in a row, which become one paragraph

    for (usz i = 0; i < ECSValue_GetListCount(children) && !reader->failed; i++)
    {
        const ECSValue *child = ECSValue_GetListItem(children, i);

        if (!MarkdownIsBlock(MarkdownTypeOf(child)))
        {
            continue;
        }

        MarkdownDescribeRun(reader, column, children, runStart, i, color);
        runStart = i + 1;
        ECSValue *described = MarkdownAddChild(reader, column);

        if (described != nullptr)
        {
            MarkdownDescribeBlock(reader, described, child, color);
        }
    }

    MarkdownDescribeRun(reader, column, children, runStart, ECSValue_GetListCount(children), color);
}

/// @brief Describes a list: a row for each item, of its mark and a column of its blocks.
static void MarkdownDescribeList(MarkdownReader *reader, ECSValue *description, const ECSValue *node, const char *color)
{
    ECSValue *props = MarkdownDescribe(reader, description, "column");
    bool tight = ECSValue_GetBool(ECSValue_GetTableField(node, "tight"), true);
    MarkdownSetInteger(reader, props, "gap", tight ? 2 : 8);
    MarkdownSetBool(reader, props, "stretch", true);

    bool ordered = ECSValue_GetBool(ECSValue_GetTableField(node, "ordered"), false);
    i64 number = ECSValue_GetInteger(ECSValue_GetTableField(node, "start"), 1);
    const ECSValue *items = ECSValue_GetTableField(node, "children");

    for (usz i = 0; i < ECSValue_GetListCount(items) && !reader->failed; i++, number++)
    {
        const ECSValue *item = ECSValue_GetListItem(items, i);
        const ECSValue *checked = ECSValue_GetTableField(item, "checked");
        char mark[32];

        if (checked != nullptr)
        {
            SDL_strlcpy(mark, ECSValue_GetBool(checked, false) ? "[x]" : "[ ]", sizeof(mark));
        }
        else if (ordered)
        {
            SDL_snprintf(mark, sizeof(mark), "%" SDL_PRIs64 ".", number);
        }
        else
        {
            SDL_strlcpy(mark, "\xE2\x80\xA2", sizeof(mark));
        }

        ECSValue *row = MarkdownAddChild(reader, description);
        ECSValue *rowProps = row == nullptr ? nullptr : MarkdownDescribe(reader, row, "row");
        MarkdownSetInteger(reader, rowProps, "gap", 6);
        MarkdownSetBool(reader, rowProps, "stretch", true);

        ECSValue *markText = rowProps == nullptr ? nullptr : MarkdownAddChild(reader, row);
        ECSValue *markProps = markText == nullptr ? nullptr : MarkdownDescribe(reader, markText, "text");
        MarkdownSetText(reader, markText, "value", mark, SDL_strlen(mark));
        MarkdownSetText(reader, markProps, "color", color, SDL_strlen(color));

        ECSValue *blocks = markText == nullptr ? nullptr : MarkdownAddChild(reader, row);

        if (blocks != nullptr)
        {
            MarkdownDescribeBlocks(reader, blocks, item, color, tight ? 2 : 8);
            MarkdownSetInteger(reader, (ECSValue *)ECSValue_GetTableField(blocks, "props"), "grow", 1);
        }
    }
}

/// @brief Describes a table: a column of rows of cells of equal widths, whose head is dimmed on the colour of the tab row, so every row shows at once.
static void MarkdownDescribeTable(MarkdownReader *reader, ECSValue *description, const ECSValue *node)
{
    ECSValue *props = MarkdownDescribe(reader, description, "column");
    MarkdownSetBool(reader, props, "stretch", true);
    const ECSValue *rows = ECSValue_GetTableField(node, "children");

    for (usz i = 0; i < ECSValue_GetListCount(rows) && !reader->failed; i++)
    {
        const ECSValue *row = ECSValue_GetListItem(rows, i);
        bool head = ECSValue_GetBool(ECSValue_GetTableField(row, "head"), false);
        const ECSValue *cells = ECSValue_GetTableField(row, "children");
        ECSValue *described = MarkdownAddChild(reader, description);
        ECSValue *rowProps = described == nullptr ? nullptr : MarkdownDescribe(reader, described, "row");
        MarkdownSetInteger(reader, rowProps, "gap", 8);
        MarkdownSetInteger(reader, rowProps, "padding", 4);
        MarkdownSetBool(reader, rowProps, "stretch", true);

        if (head)
        {
            MarkdownSetText(reader, rowProps, "background", "tabRow", 6);
        }

        for (usz j = 0; j < ECSValue_GetListCount(cells) && rowProps != nullptr && !reader->failed; j++)
        {
            ECSValue *cell = MarkdownAddChild(reader, described);

            if (cell != nullptr)
            {
                // a cell grows from no width, so the columns share the row equally and line up; a text too long is cut
                MarkdownDescribeNode(reader, cell, ECSValue_GetListItem(cells, j), 0, head ? "textDim" : "text");
                ECSValue *cellProps = (ECSValue *)ECSValue_GetTableField(cell, "props");
                MarkdownSetInteger(reader, cellProps, "grow", 1);
                MarkdownSetInteger(reader, cellProps, "width", 1);
                MarkdownSetBool(reader, cellProps, "wrap", false);
            }
        }
    }
}

static void MarkdownDescribeBlock(MarkdownReader *reader, ECSValue *description, const ECSValue *node, const char *color)
{
    static const i64 HEADING_SIZES[] = {26, 21, 18, 16, 15, 14};
    const char *type = MarkdownTypeOf(node);

    if (SDL_strcmp(type, "heading") == 0)
    {
        i64 level = SDL_clamp(ECSValue_GetInteger(ECSValue_GetTableField(node, "level"), 1), 1, 6);
        MarkdownDescribeNode(reader, description, node, HEADING_SIZES[level - 1], "text");
    }
    else if (SDL_strcmp(type, "list") == 0)
    {
        MarkdownDescribeList(reader, description, node, color);
    }
    else if (SDL_strcmp(type, "quote") == 0)
    {
        // a quote is its blocks, dimmed and indented
        MarkdownDescribeBlocks(reader, description, node, "textDim", 8);
        MarkdownSetInteger(reader, (ECSValue *)ECSValue_GetTableField(description, "props"), "padding", 12);
    }
    else if (SDL_strcmp(type, "codeBlock") == 0)
    {
        // a code block keeps its lines, in the mono font on the colour of tabs; its last new line ends it
        ECSValue *props = MarkdownDescribe(reader, description, "column");
        MarkdownSetInteger(reader, props, "padding", 8);
        MarkdownSetText(reader, props, "background", "tab", 3);
        MarkdownSetBool(reader, props, "stretch", true);

        const char *text = ECSValue_GetString(ECSValue_GetTableField(node, "text"), "");
        usz length = SDL_strlen(text);
        ECSValue *child = MarkdownAddChild(reader, description);

        if (child != nullptr)
        {
            MarkdownDescribeText(reader, child, text, length > 0 && text[length - 1] == '\n' ? length - 1 : length, 0, "text", "mono");
        }
    }
    else if (SDL_strcmp(type, "rule") == 0)
    {
        MarkdownDescribe(reader, description, "separator");
    }
    else if (SDL_strcmp(type, "table") == 0)
    {
        MarkdownDescribeTable(reader, description, node);
    }
    else if (SDL_strcmp(type, "html") == 0)
    {
        // HTML shows as it is written, dimmed
        MarkdownDescribePlain(reader, description, node, "textDim", "mono");
    }
    else
    {
        MarkdownDescribeNode(reader, description, node, 0, color);
    }
}

static void MarkdownParse(SHUSlice text, ECSValue *retDocument)
{
    (void)MarkdownRead(text.data == nullptr ? "" : text.data, text.size, retDocument);
}

static void MarkdownElements(SHUSlice text, ECSValue *retDescription)
{
    ECSValue_SetNil(retDescription);
    ECSValue *document = nullptr;
    MarkdownReader reader = {0};

    if (ECSValue_Create(&document) || !MarkdownRead(text.data == nullptr ? "" : text.data, text.size, document))
    {
        ECSValue_Destroy(&document);
        return;
    }

    MarkdownDescribeBlocks(&reader, retDescription, document, "text", 10);

    if (reader.failed)
    {
        ECSValue_SetNil(retDescription);
    }

    ECSValue_Destroy(&document);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"parse", (ECSFunction)MarkdownParse, "void(buffer text, out value<markdown.Node?> document)",
         "Read Markdown, as GitHub writes it, into a tree of nodes\n"
         "@param text The Markdown text\n"
         "@param document The document node, whose children are its blocks; nothing without memory"},
        {"elements", (ECSFunction)MarkdownElements, "void(buffer text, out value<markdown.Description?> description)",
         "Read Markdown into a description of ui's elements, which ui.showDescription shows: a column of wrapped texts, lists, quotes, code, tables and lines\n"
         "@param text The Markdown text\n"
         "@param description The description of the column; nothing without memory"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
