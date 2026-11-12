// The syntax standard plugin: syntax highlighting with tree-sitter, for code editors and viewers (DESIGN 27).
// tree-sitter and its grammars of C, Lua and JSON are compiled into the plugin (DESIGN 1); the grammars' highlight queries are in its queries/ folder.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include "tree_sitter/api.h"

#pragma region Source Only

const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_lua(void);
const TSLanguage *tree_sitter_json(void);

/// @brief A language: its grammar, the file names it takes, and its highlight query, read the first time it is used.
typedef struct SyntaxLanguage
{
    const char *name;
    const char *extensions[4]; // ending with nullptr
    const TSLanguage *(*Grammar)(void);
    TSQuery *query;
    bool failed; // its query cannot be read; the reason was logged
} SyntaxLanguage;

/// @brief A kind of highlight, the setting of its colour, and the first words of the captures of the queries that are of it.
typedef struct SyntaxKind
{
    const char *name;
    const char *setting;
    const char *captures[8]; // ending with nullptr
} SyntaxKind;

static SyntaxLanguage SYNTAX_LANGUAGES[] = {
    {"c", {".c", ".h", nullptr}, tree_sitter_c, nullptr, false},
    {"lua", {".lua", nullptr}, tree_sitter_lua, nullptr, false},
    {"json", {".json", nullptr}, tree_sitter_json, nullptr, false},
};

static const SyntaxKind SYNTAX_KINDS[] = {
    {"keyword", "syntax.colorKeyword", {"keyword", "repeat", "conditional", "include", "preproc", "label", nullptr}},
    {"string", "syntax.colorString", {"string", "escape", "character", nullptr}},
    {"comment", "syntax.colorComment", {"comment", nullptr}},
    {"number", "syntax.colorNumber", {"number", "float", "boolean", nullptr}},
    {"function", "syntax.colorFunction", {"function", "method", "constructor", nullptr}},
    {"type", "syntax.colorType", {"type", "attribute", nullptr}},
    {"constant", "syntax.colorConstant", {"constant", nullptr}},
    {"punctuation", "syntax.colorPunctuation", {"punctuation", "operator", "delimiter", nullptr}},
};

static struct
{
    ECSPlugin plugin;
    TSParser *parser;
} SYNTAX = {0};

static SyntaxLanguage *SyntaxFindLanguage(const char *name)
{
    for (usz i = 0; i < SDL_arraysize(SYNTAX_LANGUAGES); i++)
    {
        if (SDL_strcmp(SYNTAX_LANGUAGES[i].name, name) == 0)
        {
            return &SYNTAX_LANGUAGES[i];
        }
    }

    return nullptr;
}

/// @brief The kind of a capture's name, such as "keyword.function"; 0 for none, else the kind's index and one.
static u8 SyntaxKindOf(const char *capture, u32 length)
{
    for (usz i = 0; i < SDL_arraysize(SYNTAX_KINDS); i++)
    {
        for (usz j = 0; SYNTAX_KINDS[i].captures[j] != nullptr; j++)
        {
            usz word = SDL_strlen(SYNTAX_KINDS[i].captures[j]);

            if (word <= length && SDL_strncmp(capture, SYNTAX_KINDS[i].captures[j], word) == 0 && (word == length || capture[word] == '.'))
            {
                return (u8)(i + 1);
            }
        }
    }

    return 0;
}

/// @brief Reads a language's highlight query from the plugin's queries/ folder, the first time.
/// @return The query, or nullptr if it cannot be read; the reason is logged once.
static TSQuery *SyntaxQueryOf(SyntaxLanguage *language)
{
    if (language->query != nullptr || language->failed)
    {
        return language->query;
    }

    char *path = nullptr;
    usz size = 0;
    char *source = SDL_asprintf(&path, "%squeries/%s/highlights.scm", ECSPlugin_GetFolder(SYNTAX.plugin), language->name) < 0 ? nullptr : SDL_LoadFile(path, &size);
    u32 offset = 0;
    TSQueryError error = TSQueryErrorNone;

    if (source != nullptr)
    {
        language->query = ts_query_new(language->Grammar(), source, (u32)size, &offset, &error);
    }

    if (language->query == nullptr)
    {
        ECS_LogError(SYNTAX.plugin, "Cannot read the highlight query '%s' (error %d at byte %u).", path == nullptr ? language->name : path, (int)error, offset);
        language->failed = true;
    }

    SDL_free(source);
    SDL_free(path);
    return language->query;
}

/// @brief Matches a text with a pattern of the few regular expressions that highlight queries use: ^ and $, ., classes such as [A-Z\d_],
/// the escapes \d, \w and \s, and the quantifiers *, + and ?.
static bool SyntaxMatchHere(const char *pattern, const char *text, const char *end);

/// @brief Tells whether a letter is one that an escape names: \d a digit, \w a letter, digit or _, \s a blank, and any other the letter itself.
static bool SyntaxEscapeMatches(char escape, char letter)
{
    switch (escape)
    {
    case 'd':
        return SDL_isdigit((unsigned char)letter);
    case 'w':
        return SDL_isalnum((unsigned char)letter) || letter == '_';
    case 's':
        return SDL_isspace((unsigned char)letter);
    default:
        return letter == escape;
    }
}

/// @brief Reads a class, such as [A-Z\d_] or [^"], whose [ the pattern starts with; tells whether a letter is in it.
/// @return Where the class ends in the pattern.
static const char *SyntaxClass(const char *pattern, char letter, bool *retMatches)
{
    const char *cursor = pattern + 1;
    bool negated = *cursor == '^';
    bool matches = false;
    cursor += negated ? 1 : 0;

    while (*cursor != '\0' && *cursor != ']')
    {
        if (*cursor == '\\' && cursor[1] != '\0')
        {
            matches = matches || SyntaxEscapeMatches(cursor[1], letter);
            cursor += 2;
        }
        else if (cursor[1] == '-' && cursor[2] != ']' && cursor[2] != '\0')
        {
            matches = matches || (letter >= cursor[0] && letter <= cursor[2]);
            cursor += 3;
        }
        else
        {
            matches = matches || letter == *cursor;
            cursor++;
        }
    }

    *retMatches = negated ? !matches : matches;
    return *cursor == ']' ? cursor + 1 : cursor;
}

/// @brief Reads one item of a pattern: a letter, ., an escape or a class; tells whether a letter matches it.
/// @return Where the item ends in the pattern.
static const char *SyntaxItem(const char *pattern, char letter, bool *retMatches)
{
    if (*pattern == '\\' && pattern[1] != '\0')
    {
        *retMatches = SyntaxEscapeMatches(pattern[1], letter);
        return pattern + 2;
    }

    if (*pattern == '[')
    {
        return SyntaxClass(pattern, letter, retMatches);
    }

    *retMatches = *pattern == '.' ? letter != '\n' : letter == *pattern;
    return pattern + 1;
}

/// @brief Matches an item and its quantifier, then the rest of the pattern: the longest run of the item first, then shorter ones down to the least
/// the quantifier takes.
static bool SyntaxMatchRepeat(const char *item, const char *quantifier, const char *text, const char *end)
{
    usz most = *quantifier == '?' ? 1 : (usz)(end - text);
    usz least = *quantifier == '+' ? 1 : 0;
    usz count = 0;
    bool one = text < end;

    while (one && count < most)
    {
        (void)SyntaxItem(item, text[count], &one);
        count += one ? 1 : 0;
        one = one && text + count < end;
    }

    for (; count >= least; count--)
    {
        if (SyntaxMatchHere(quantifier + 1, text + count, end))
        {
            return true;
        }

        if (count == 0)
        {
            break;
        }
    }

    return false;
}

static bool SyntaxMatchHere(const char *pattern, const char *text, const char *end)
{
    if (*pattern == '\0')
    {
        return true;
    }

    if (*pattern == '$' && pattern[1] == '\0')
    {
        return text == end;
    }

    bool matches = false;
    const char *next = SyntaxItem(pattern, text < end ? *text : '\0', &matches);

    if (*next == '*' || *next == '+' || *next == '?')
    {
        return SyntaxMatchRepeat(pattern, next, text, end);
    }

    return matches && text < end && SyntaxMatchHere(next, text + 1, end);
}

static bool SyntaxMatch(const char *pattern, const char *text, usz length)
{
    const char *end = text + length;

    if (*pattern == '^')
    {
        return SyntaxMatchHere(pattern + 1, text, end);
    }

    for (const char *start = text; start <= end; start++)
    {
        if (SyntaxMatchHere(pattern, start, end))
        {
            return true;
        }
    }

    return false;
}

/// @brief The text of a capture in a match, or nullptr if the match does not have it.
static const char *SyntaxCaptureText(const TSQueryMatch *match, u32 capture, const char *text, usz *retLength)
{
    for (u16 i = 0; i < match->capture_count; i++)
    {
        if (match->captures[i].index == capture)
        {
            u32 start = ts_node_start_byte(match->captures[i].node);
            *retLength = ts_node_end_byte(match->captures[i].node) - start;
            return text + start;
        }
    }

    return nullptr;
}

/// @brief Tells whether a match passes one predicate, the steps from first to before last: eq?, any-of? and match?, and their not- forms.
/// Directives, which end in !, and predicates it does not know pass.
static bool SyntaxPredicatePasses(const TSQuery *query, const TSQueryMatch *match, const char *text, const TSQueryPredicateStep *steps, u32 first, u32 last)
{
    u32 length = 0;
    const char *name = steps[first].type == TSQueryPredicateStepTypeString ? ts_query_string_value_for_id(query, steps[first].value_id, &length) : "";
    bool negated = SDL_strncmp(name, "not-", 4) == 0;
    const char *test = negated ? name + 4 : name;
    bool matching = SDL_strcmp(test, "match?") == 0;
    usz captured = 0;
    const char *subject = last > first + 1 && steps[first + 1].type == TSQueryPredicateStepTypeCapture ? SyntaxCaptureText(match, steps[first + 1].value_id, text, &captured) : nullptr;

    if (subject == nullptr || !(matching || SDL_strcmp(test, "eq?") == 0 || SDL_strcmp(test, "any-of?") == 0))
    {
        return true;
    }

    bool passes = false;

    for (u32 i = first + 2; i < last && !passes; i++)
    {
        u32 valueLength = 0;
        usz otherLength = 0;
        const char *value = steps[i].type == TSQueryPredicateStepTypeString ? ts_query_string_value_for_id(query, steps[i].value_id, &valueLength) : SyntaxCaptureText(match, steps[i].value_id, text, &otherLength);
        otherLength = steps[i].type == TSQueryPredicateStepTypeString ? valueLength : otherLength;

        if (value != nullptr)
        {
            passes = matching ? SyntaxMatch(value, subject, captured) : otherLength == captured && SDL_strncmp(value, subject, captured) == 0;
        }
    }

    return passes != negated;
}

/// @brief Tells whether a match passes every predicate of its pattern.
static bool SyntaxPasses(const TSQuery *query, const TSQueryMatch *match, const char *text)
{
    u32 count = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(query, match->pattern_index, &count);

    for (u32 first = 0; first < count;)
    {
        u32 last = first;

        while (last < count && steps[last].type != TSQueryPredicateStepTypeDone)
        {
            last++;
        }

        if (!SyntaxPredicatePasses(query, match, text, steps, first, last))
        {
            return false;
        }

        first = last + 1;
    }

    return true;
}

/// @brief Gives each byte of a text the kind of its highlight, 0 for none: the first capture that covers a byte names it.
/// @return The kinds, one per byte, to free with SDL_free, or nullptr if the language has no query or memory runs out.
static u8 *SyntaxKinds(SyntaxLanguage *language, const char *text, usz length, const char **retError)
{
    TSQuery *query = SyntaxQueryOf(language);
    u8 *kinds = query == nullptr ? nullptr : SDL_calloc(length + 1, 1);

    if (kinds == nullptr)
    {
        *retError = query == nullptr ? "the language's highlight query cannot be read; the log has why" : "out of memory";
        return nullptr;
    }

    TSTree *tree = ts_parser_set_language(SYNTAX.parser, language->Grammar()) ? ts_parser_parse_string(SYNTAX.parser, nullptr, text, (u32)length) : nullptr;
    TSQueryCursor *cursor = tree == nullptr ? nullptr : ts_query_cursor_new();

    if (cursor == nullptr)
    {
        ECS_Fail(SYNTAX.plugin, retError, "the text cannot be parsed");
        ts_tree_delete(tree);
        SDL_free(kinds);
        return nullptr;
    }

    ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
    TSQueryMatch match;
    u32 index = 0;

    while (ts_query_cursor_next_capture(cursor, &match, &index))
    {
        const TSQueryCapture *capture = &match.captures[index];
        u32 nameLength = 0;
        const char *name = ts_query_capture_name_for_id(query, capture->index, &nameLength);
        u8 kind = SyntaxKindOf(name, nameLength);

        if (kind == 0 || !SyntaxPasses(query, &match, text))
        {
            continue;
        }

        for (u32 byte = ts_node_start_byte(capture->node); byte < ts_node_end_byte(capture->node) && byte < length; byte++)
        {
            kinds[byte] = kinds[byte] == 0 ? kind : kinds[byte];
        }
    }

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);
    return kinds;
}

static SyntaxLanguage *SyntaxCheckLanguage(const char *name, const char **retError)
{
    SyntaxLanguage *language = SyntaxFindLanguage(name);
    *retError = language == nullptr ? "the language is not one of c, lua and json" : nullptr;
    return language;
}

/// @brief Sets a table's field to a text; false without memory.
static bool SyntaxSetText(ECSValue *table, const char *name, const char *text, usz length)
{
    ECSValue *field = nullptr;
    char *copy = SDL_strndup(text, length);
    bool written = copy != nullptr && !ECSValue_SetTableField(table, name, &field) && !ECSValue_SetString(field, copy);
    SDL_free(copy);
    return written;
}

static void SyntaxHighlight(const char *name, SHUSlice text, ECSValue *retRanges, const char **retError)
{
    ECSValue_SetNil(retRanges);
    SyntaxLanguage *language = SyntaxCheckLanguage(name, retError);
    u8 *kinds = language == nullptr ? nullptr : SyntaxKinds(language, text.data == nullptr ? "" : text.data, text.size, retError);

    if (kinds == nullptr)
    {
        return;
    }

    ECSValue_SetTable(retRanges);
    bool written = true;

    for (usz byte = 0; byte < text.size && written;)
    {
        usz end = byte;

        while (end < text.size && kinds[end] == kinds[byte])
        {
            end++;
        }

        if (kinds[byte] != 0)
        {
            ECSValue *range = nullptr;
            ECSValue *field = nullptr;
            const char *kind = SYNTAX_KINDS[kinds[byte] - 1].name;
            written = !ECSValue_AddListItem(retRanges, &range) && !ECSValue_SetTableField(range, "start", &field);
            written = written && (ECSValue_SetInteger(field, (i64)byte + 1), !ECSValue_SetTableField(range, "stop", &field));
            written = written && (ECSValue_SetInteger(field, (i64)end), SyntaxSetText(range, "kind", kind, SDL_strlen(kind)));
        }

        byte = end;
    }

    if (!written)
    {
        ECS_Fail(SYNTAX.plugin, retError, "out of memory");
        ECSValue_SetNil(retRanges);
    }

    SDL_free(kinds);
}

static void SyntaxSpans(const char *name, SHUSlice text, ECSValue *retLines, const char **retError)
{
    ECSValue_SetNil(retLines);
    SyntaxLanguage *language = SyntaxCheckLanguage(name, retError);
    const char *source = text.data == nullptr ? "" : text.data;
    u8 *kinds = language == nullptr ? nullptr : SyntaxKinds(language, source, text.size, retError);

    if (kinds == nullptr)
    {
        return;
    }

    ECSValue_SetTable(retLines);
    ECSValue *line = nullptr;
    bool written = !ECSValue_AddListItem(retLines, &line);

    if (written)
    {
        ECSValue_SetTable(line);
    }

    // a span is a run of one kind within a line; a new line starts the next line's list
    for (usz byte = 0; byte < text.size && written;)
    {
        if (source[byte] == '\n')
        {
            written = !ECSValue_AddListItem(retLines, &line);
            ECSValue_SetTable(line);
            byte++;
            continue;
        }

        usz end = byte;

        while (end < text.size && kinds[end] == kinds[byte] && source[end] != '\n')
        {
            end++;
        }

        ECSValue *span = nullptr;
        written = !ECSValue_AddListItem(line, &span);
        ECSValue_SetTable(span);
        written = written && SyntaxSetText(span, "text", source + byte, end - byte);

        if (written && kinds[byte] != 0)
        {
            const char *color = ECSValue_GetString(ECSSetting_Get(SYNTAX_KINDS[kinds[byte] - 1].setting), "text");
            written = SyntaxSetText(span, "color", color, SDL_strlen(color));
        }

        byte = end;
    }

    if (!written)
    {
        ECS_Fail(SYNTAX.plugin, retError, "out of memory");
        ECSValue_SetNil(retLines);
    }

    SDL_free(kinds);
}

static void SyntaxLanguages(ECSValue *retNames)
{
    ECSValue_SetTable(retNames);

    for (usz i = 0; i < SDL_arraysize(SYNTAX_LANGUAGES); i++)
    {
        ECSValue *name = nullptr;

        if (ECSValue_AddListItem(retNames, &name) || ECSValue_SetString(name, SYNTAX_LANGUAGES[i].name))
        {
            ECSValue_SetNil(retNames);
            return;
        }
    }
}

static const char *SyntaxLanguageOf(const char *path)
{
    const char *dot = SDL_strrchr(path, '.');

    for (usz i = 0; dot != nullptr && i < SDL_arraysize(SYNTAX_LANGUAGES); i++)
    {
        for (usz j = 0; SYNTAX_LANGUAGES[i].extensions[j] != nullptr; j++)
        {
            if (SDL_strcasecmp(dot, SYNTAX_LANGUAGES[i].extensions[j]) == 0)
            {
                return SYNTAX_LANGUAGES[i].name;
            }
        }
    }

    return nullptr;
}

static void SyntaxFree(void *data)
{
    (void)data;

    for (usz i = 0; i < SDL_arraysize(SYNTAX_LANGUAGES); i++)
    {
        ts_query_delete(SYNTAX_LANGUAGES[i].query);
        SYNTAX_LANGUAGES[i].query = nullptr;
        SYNTAX_LANGUAGES[i].failed = false;
    }

    ts_parser_delete(SYNTAX.parser);
    SDL_zero(SYNTAX);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    SYNTAX.plugin = plugin;
    ts_set_allocator(SDL_malloc, SDL_calloc, SDL_realloc, SDL_free);
    SYNTAX.parser = ts_parser_new();

    if (SYNTAX.parser == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    const ECSPluginDesc desc = {.Destroy = SyntaxFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), SyntaxFree(nullptr););

    // each kind's colour; a theme or the user's settings file may set them, and they read on dark and light backgrounds
    const struct
    {
        const char *name;
        const char *description;
        const char *color;
    } colors[] = {
        {"colorKeyword", "Colour of keywords", "#4C8BF5"},
        {"colorString", "Colour of strings", "#5E9E5E"},
        {"colorComment", "Colour of comments", "#8A8F98"},
        {"colorNumber", "Colour of numbers and booleans", "#C27B30"},
        {"colorFunction", "Colour of functions", "#3E8FB0"},
        {"colorType", "Colour of types", "#B0679A"},
        {"colorConstant", "Colour of constants", "#C27B30"},
        {"colorPunctuation", "Colour of operators and punctuation", "#8A8F98"},
    };

    for (usz i = 0; i < SDL_arraysize(colors); i++)
    {
        const ECSSettingDesc setting = {.name = colors[i].name, .type = ECSSettingType_Color, .description = colors[i].description, .defaultString = colors[i].color};
        SHU_ReturnResult(ECSSetting_Declare(plugin, &setting));
    }

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"languages", (ECSFunction)SyntaxLanguages, "void(out value<string[]> names)",
         "Give the languages it highlights\n"
         "@param names Their names: c, lua and json"},
        {"languageOf", (ECSFunction)SyntaxLanguageOf, "string(string path)",
         "Give the language of a file, by its extension\n"
         "@param path The file's path or name, such as \"main.c\"\n"
         "@return The language's name; nothing for a file of no language it knows"},
        {"highlight", (ECSFunction)SyntaxHighlight, "void(string language, buffer text, out value<syntax.Range[]?> ranges, out string error)",
         "Give the highlighted ranges of a text: keywords, strings, comments, numbers, functions, types, constants and punctuation\n"
         "@param language c, lua or json\n"
         "@param text The text, in UTF-8\n"
         "@param ranges The ranges, in order, each with its first and last byte from 1 and its kind; text of no kind has none\n"
         "@param error Why it cannot highlight, such as a language it does not know; nothing if it highlighted"},
        {"spans", (ECSFunction)SyntaxSpans, "void(string language, buffer text, out value<ui.Span[][]?> lines, out string error)",
         "Give a text's lines as spans of ui's, each in the colour of its kind, for ui.text in the mono font\n"
         "@param language c, lua or json\n"
         "@param text The text, in UTF-8\n"
         "@param lines A list of spans for each line of the text\n"
         "@param error Why it cannot highlight; nothing if it highlighted"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
