// The regex standard plugin: regular expressions on UTF-8 text, for searching and filtering text, such as a log's lines (DESIGN 25).
// PCRE2 is compiled into the plugin (DESIGN 1), with SDL's allocator.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#define PCRE2_STATIC
#include "pcre2.h"

#pragma region Source Only

/// @brief A compiled pattern, and the space its matches are written into.
typedef struct RegexPattern
{
    pcre2_code *code;
    pcre2_match_data *match;
    u32 groups; // the number of groups in parentheses
    bool crlf;  // a newline may be \r\n, so an empty match never splits one
} RegexPattern;

static struct
{
    ECSPlugin plugin;
    pcre2_general_context *context; // gives PCRE2 SDL's allocator
    char *result;                   // the last text a function gave, kept until the next one
    usz resultSize;
} REGEX = {0};

static void *RegexAllocate(PCRE2_SIZE size, void *data)
{
    (void)data;
    return SDL_malloc(size);
}

static void RegexRelease(void *memory, void *data)
{
    (void)data;
    SDL_free(memory);
}

/// @brief Fails a call, as ECS_Fail does, with PCRE2's message for a code, and gives why.
static const char *RegexFail(const char *what, int code)
{
    PCRE2_UCHAR message[200];

    if (pcre2_get_error_message(code, message, sizeof(message)) < 0)
    {
        SDL_strlcpy((char *)message, "unknown error", sizeof(message));
    }

    const char *error = nullptr;
    ECS_Fail(REGEX.plugin, &error, "%s: %s", what, (const char *)message);
    return error;
}

static void RegexPatternDestroy(void *data)
{
    RegexPattern *pattern = data;
    pcre2_match_data_free(pattern->match);
    pcre2_code_free(pattern->code);
    SDL_free(pattern);
}

/// @brief Reads the options' letters: i ignores case, m makes ^ and $ match at each line, s makes . match a newline, and x ignores spaces and # comments.
/// @return false if a letter is not one of them.
static bool RegexReadOptions(const ECSValue *options, u32 *retOptions)
{
    *retOptions = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF;
    const char *letters = ECSValue_GetString(options, "");

    for (const char *letter = letters; *letter != '\0'; letter++)
    {
        switch (*letter)
        {
        case 'i':
            *retOptions |= PCRE2_CASELESS;
            break;
        case 'm':
            *retOptions |= PCRE2_MULTILINE;
            break;
        case 's':
            *retOptions |= PCRE2_DOTALL;
            break;
        case 'x':
            *retOptions |= PCRE2_EXTENDED;
            break;
        default:
            return false;
        }
    }

    return true;
}

static RegexPattern *RegexCompile(const char *source, const ECSValue *options, const char **retError)
{
    *retError = nullptr;
    u32 flags = 0;

    if (!ECS_Check(REGEX.plugin, RegexReadOptions(options, &flags), "The options are letters of i, m, s and x."))
    {
        return nullptr;
    }

    pcre2_compile_context *compile = pcre2_compile_context_create(REGEX.context);
    int code = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code *compiled = pcre2_compile((PCRE2_SPTR)source, PCRE2_ZERO_TERMINATED, flags, &code, &offset, compile);
    pcre2_compile_context_free(compile);

    if (compiled == nullptr)
    {
        PCRE2_UCHAR message[200];
        (void)pcre2_get_error_message(code, message, sizeof(message));
        ECS_Fail(REGEX.plugin, retError, "the pattern cannot be read at byte %u: %s", (unsigned)offset + 1U, (const char *)message);
        return nullptr;
    }

    RegexPattern *pattern = SDL_calloc(1, sizeof(RegexPattern));
    pcre2_match_data *match = pcre2_match_data_create_from_pattern(compiled, REGEX.context);

    if (pattern == nullptr || match == nullptr)
    {
        SDL_free(pattern);
        pcre2_match_data_free(match);
        pcre2_code_free(compiled);
        ECS_Fail(REGEX.plugin, retError, "out of memory");
        return nullptr;
    }

    u32 newline = 0;
    (void)pcre2_pattern_info(compiled, PCRE2_INFO_CAPTURECOUNT, &pattern->groups);
    (void)pcre2_pattern_info(compiled, PCRE2_INFO_NEWLINE, &newline);
    pattern->code = compiled;
    pattern->match = match;
    pattern->crlf = newline == PCRE2_NEWLINE_ANY || newline == PCRE2_NEWLINE_CRLF || newline == PCRE2_NEWLINE_ANYCRLF;
    return pattern;
}

/// @brief Finds the next match from a byte, as Perl does: after an empty match, a match must not be empty where that one was, or it starts a character later.
/// @param offset The byte to start at; set after the match, where the next search starts.
/// @param empty The last match was empty at offset; set for the next search.
/// @return The match's count of groups and one, 0 for no match, or a negative PCRE2 error.
static int RegexNext(RegexPattern *pattern, SHUSlice text, usz *offset, bool *empty)
{
    while (*offset <= text.size)
    {
        u32 flags = *empty ? PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED : 0;
        int found = pcre2_match(pattern->code, (PCRE2_SPTR)text.data, text.size, *offset, flags, pattern->match, nullptr);

        if (found > 0)
        {
            const PCRE2_SIZE *bounds = pcre2_get_ovector_pointer(pattern->match);
            *empty = bounds[0] == bounds[1];
            *offset = bounds[1];
            return found;
        }

        if (found != PCRE2_ERROR_NOMATCH || !*empty)
        {
            return found == PCRE2_ERROR_NOMATCH ? 0 : found;
        }

        // the empty match is passed by one character, a whole \r\n, or a whole UTF-8 sequence
        const u8 *bytes = (const u8 *)text.data;
        *empty = false;

        if (*offset >= text.size)
        {
            return 0;
        }

        if (pattern->crlf && bytes[*offset] == '\r' && *offset + 1 < text.size && bytes[*offset + 1] == '\n')
        {
            *offset += 2;
            continue;
        }

        (*offset)++;

        while (*offset < text.size && (bytes[*offset] & 0xC0U) == 0x80U)
        {
            (*offset)++;
        }
    }

    return 0;
}

/// @brief Writes the last match into a table: its positions from 1, its text, its groups and its named groups.
static bool RegexWriteMatch(const RegexPattern *pattern, SHUSlice text, int found, ECSValue *retMatch)
{
    const PCRE2_SIZE *bounds = pcre2_get_ovector_pointer(pattern->match);
    ECSValue *field = nullptr;
    ECSValue_SetTable(retMatch);

    // a lookahead such as (?=a\K) can end a match before it starts
    PCRE2_SIZE start = bounds[0];
    PCRE2_SIZE stop = SDL_max(bounds[1], bounds[0]);

    bool written = !ECSValue_SetTableField(retMatch, "start", &field);
    written = written && (ECSValue_SetInteger(field, (i64)start + 1), !ECSValue_SetTableField(retMatch, "stop", &field));
    written = written && (ECSValue_SetInteger(field, (i64)stop), !ECSValue_SetTableField(retMatch, "text", &field));
    written = written && !ECSValue_SetBuffer(field, (SHUSliceView){(const u8 *)text.data + start, stop - start});

    ECSValue *groups = nullptr;
    written = written && !ECSValue_SetTableField(retMatch, "groups", &groups);

    if (written)
    {
        ECSValue_SetTable(groups);
    }

    for (usz i = 1; written && i <= pattern->groups; i++)
    {
        ECSValue *group = nullptr;
        written = !ECSValue_AddListItem(groups, &group);

        // a group past the count that matched, or one that matched nothing, is unset
        if (written && (int)i < found && bounds[2 * i] != PCRE2_UNSET)
        {
            written = !ECSValue_SetBuffer(group, (SHUSliceView){(const u8 *)text.data + bounds[2 * i], bounds[2 * i + 1] - bounds[2 * i]});
        }
        else if (written)
        {
            ECSValue_SetBool(group, false);
        }
    }

    // the name table: each entry is the group's number in two bytes, then its name ending in a zero byte
    u32 count = 0;
    u32 size = 0;
    PCRE2_SPTR names = nullptr;
    (void)pcre2_pattern_info(pattern->code, PCRE2_INFO_NAMECOUNT, &count);
    (void)pcre2_pattern_info(pattern->code, PCRE2_INFO_NAMEENTRYSIZE, &size);
    (void)pcre2_pattern_info(pattern->code, PCRE2_INFO_NAMETABLE, &names);

    ECSValue *named = nullptr;
    written = written && !ECSValue_SetTableField(retMatch, "named", &named);

    if (written)
    {
        ECSValue_SetTable(named);
    }

    for (u32 i = 0; written && i < count; i++)
    {
        PCRE2_SPTR entry = names + (usz)i * size;
        usz number = ((usz)entry[0] << 8U) | entry[1];

        if ((int)number < found && bounds[2 * number] != PCRE2_UNSET)
        {
            ECSValue *value = nullptr;
            written = !ECSValue_SetTableField(named, (const char *)entry + 2, &value);
            written = written && !ECSValue_SetBuffer(value, (SHUSliceView){(const u8 *)text.data + bounds[2 * number], bounds[2 * number + 1] - bounds[2 * number]});
        }
    }

    return written;
}

static bool RegexTest(RegexPattern *pattern, SHUSlice text)
{
    return pcre2_match(pattern->code, (PCRE2_SPTR)text.data, text.size, 0, 0, pattern->match, nullptr) > 0;
}

static void RegexMatch(RegexPattern *pattern, SHUSlice text, const ECSValue *from, ECSValue *retMatch, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retMatch);
    i64 first = ECSValue_GetInteger(from, 1);

    if (first < 1 || (u64)first > text.size + 1)
    {
        return;
    }

    usz offset = (usz)first - 1;
    bool empty = false;
    int found = RegexNext(pattern, text, &offset, &empty);

    if (found < 0)
    {
        *retError = RegexFail("the match failed", found);
    }
    else if (found > 0 && !RegexWriteMatch(pattern, text, found, retMatch))
    {
        ECS_Fail(REGEX.plugin, retError, "out of memory");
        ECSValue_SetNil(retMatch);
    }
}

static void RegexMatchAll(RegexPattern *pattern, SHUSlice text, ECSValue *retMatches, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetTable(retMatches);
    usz offset = 0;
    bool empty = false;
    int found = 0;

    while ((found = RegexNext(pattern, text, &offset, &empty)) > 0)
    {
        ECSValue *match = nullptr;

        if (ECSValue_AddListItem(retMatches, &match) || !RegexWriteMatch(pattern, text, found, match))
        {
            ECS_Fail(REGEX.plugin, retError, "out of memory");
            ECSValue_SetNil(retMatches);
            return;
        }
    }

    if (found < 0)
    {
        *retError = RegexFail("the match failed", found);
        ECSValue_SetNil(retMatches);
    }
}

static void RegexSplit(RegexPattern *pattern, SHUSlice text, ECSValue *retParts, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetTable(retParts);
    usz offset = 0;
    usz partStart = 0;
    bool empty = false;
    int found = 0;
    ECSValue *part = nullptr;

    while ((found = RegexNext(pattern, text, &offset, &empty)) > 0)
    {
        const PCRE2_SIZE *bounds = pcre2_get_ovector_pointer(pattern->match);

        // an empty match at the start or the end splits nothing off
        if (empty && (bounds[0] == 0 || bounds[0] == text.size))
        {
            continue;
        }

        if (ECSValue_AddListItem(retParts, &part) || ECSValue_SetBuffer(part, (SHUSliceView){(const u8 *)text.data + partStart, bounds[0] - partStart}))
        {
            ECS_Fail(REGEX.plugin, retError, "out of memory");
            ECSValue_SetNil(retParts);
            return;
        }

        partStart = bounds[1];
    }

    if (found < 0)
    {
        *retError = RegexFail("the match failed", found);
        ECSValue_SetNil(retParts);
        return;
    }

    if (ECSValue_AddListItem(retParts, &part) || ECSValue_SetBuffer(part, (SHUSliceView){(const u8 *)text.data + partStart, text.size - partStart}))
    {
        ECS_Fail(REGEX.plugin, retError, "out of memory");
        ECSValue_SetNil(retParts);
    }
}

/// @brief Replaces the first match, or every one, with PCRE2's substitution: $1 or ${name} for a group, and $$ for a dollar.
static SHUSlice RegexSubstitute(RegexPattern *pattern, SHUSlice text, const char *replacement, bool all, i64 *retCount, const char **retError)
{
    *retError = nullptr;
    *retCount = 0;
    u32 flags = PCRE2_SUBSTITUTE_OVERFLOW_LENGTH | PCRE2_SUBSTITUTE_EXTENDED | (all ? PCRE2_SUBSTITUTE_GLOBAL : 0);
    PCRE2_SIZE size = text.size + 1;

    // the first try tells the size that the result needs, when it is larger
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (size > REGEX.resultSize)
        {
            char *grown = SDL_realloc(REGEX.result, size);

            if (grown == nullptr)
            {
                ECS_Fail(REGEX.plugin, retError, "out of memory");
                return (SHUSlice){0};
            }

            REGEX.result = grown;
            REGEX.resultSize = size;
        }

        size = REGEX.resultSize;
        int count = pcre2_substitute(pattern->code, (PCRE2_SPTR)text.data, text.size, 0, flags, pattern->match, nullptr, (PCRE2_SPTR)replacement, PCRE2_ZERO_TERMINATED,
                                     (PCRE2_UCHAR *)REGEX.result, &size);

        if (count >= 0)
        {
            *retCount = count;
            return (SHUSlice){REGEX.result, size};
        }

        if (count != PCRE2_ERROR_NOMEMORY)
        {
            *retError = RegexFail("the replacement failed", count);
            return (SHUSlice){0};
        }
    }

    ECS_Fail(REGEX.plugin, retError, "out of memory");
    return (SHUSlice){0};
}

static SHUSlice RegexReplace(RegexPattern *pattern, SHUSlice text, const char *replacement, i64 *retCount, const char **retError)
{
    return RegexSubstitute(pattern, text, replacement, true, retCount, retError);
}

static SHUSlice RegexReplaceFirst(RegexPattern *pattern, SHUSlice text, const char *replacement, i64 *retCount, const char **retError)
{
    return RegexSubstitute(pattern, text, replacement, false, retCount, retError);
}

static const char *RegexEscape(const char *text)
{
    usz length = SDL_strlen(text);
    usz size = length * 2 + 1;

    if (size > REGEX.resultSize)
    {
        char *grown = SDL_realloc(REGEX.result, size);

        if (grown == nullptr)
        {
            return nullptr;
        }

        REGEX.result = grown;
        REGEX.resultSize = size;
    }

    char *out = REGEX.result;

    for (const char *letter = text; *letter != '\0'; letter++)
    {
        if (SDL_strchr("\\^$.|?*+()[]{}-#/ ", *letter) != nullptr)
        {
            *out++ = '\\';
        }

        *out++ = *letter;
    }

    *out = '\0';
    return REGEX.result;
}

static void RegexFree(void *data)
{
    (void)data;
    pcre2_general_context_free(REGEX.context);
    SDL_free(REGEX.result);
    SDL_zero(REGEX);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    REGEX.plugin = plugin;
    REGEX.context = pcre2_general_context_create(RegexAllocate, RegexRelease, nullptr);

    if (REGEX.context == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    ECSPluginDesc desc = {.Destroy = RegexFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), RegexFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "pattern", RegexPatternDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"compile", (ECSFunction)RegexCompile, "handle<regex.pattern>(string pattern, value<string?> options, out string error)",
         "Read a pattern, in Perl's syntax, for UTF-8 text\n"
         "@param pattern The pattern, such as \"(\\\\d+)-(\\\\d+)\"\n"
         "@param options Letters, or nothing: i ignores case, m makes ^ and $ match at each line, s makes . match a newline, x ignores spaces and # comments\n"
         "@param error Why it cannot be read, with the byte where; nothing if it was read\n"
         "@return The pattern; nothing if it cannot be read"},
        {"test", (ECSFunction)RegexTest, "bool(handle<regex.pattern> pattern, buffer text)",
         "Tell whether the pattern matches anywhere in a text, the fastest way to filter lines\n"
         "@param pattern The pattern\n"
         "@param text The text\n"
         "@return true if it matches"},
        {"match", (ECSFunction)RegexMatch, "void(handle<regex.pattern> pattern, buffer text, value<integer?> start, out value<regex.Match?> match, out string error)",
         "Find the first match in a text\n"
         "@param pattern The pattern\n"
         "@param text The text\n"
         "@param start The byte to start at, from 1; 1 if missing\n"
         "@param match The match; nothing if there is none\n"
         "@param error Why the match failed, such as a limit it reached; nothing if it did not"},
        {"matchAll", (ECSFunction)RegexMatchAll, "void(handle<regex.pattern> pattern, buffer text, out value<regex.Match[]?> matches, out string error)",
         "Find every match in a text, each after the last\n"
         "@param pattern The pattern\n"
         "@param text The text\n"
         "@param matches The matches, in order; empty if there is none\n"
         "@param error Why the match failed; nothing if it did not"},
        {"replace", (ECSFunction)RegexReplace, "buffer(handle<regex.pattern> pattern, buffer text, string replacement, out int count, out string error)",
         "Replace every match in a text\n"
         "@param pattern The pattern\n"
         "@param text The text\n"
         "@param replacement What each match becomes: $1 or ${name} is a group's text, and $$ a dollar\n"
         "@param count How many matches were replaced\n"
         "@param error Why it cannot replace, such as a group the pattern lacks; nothing if it replaced\n"
         "@return The new text; nothing if it cannot replace"},
        {"replaceFirst", (ECSFunction)RegexReplaceFirst, "buffer(handle<regex.pattern> pattern, buffer text, string replacement, out int count, out string error)",
         "Replace the first match in a text\n"
         "@param pattern The pattern\n"
         "@param text The text\n"
         "@param replacement What the match becomes: $1 or ${name} is a group's text, and $$ a dollar\n"
         "@param count 1 if a match was replaced, else 0\n"
         "@param error Why it cannot replace; nothing if it replaced\n"
         "@return The new text; nothing if it cannot replace"},
        {"split", (ECSFunction)RegexSplit, "void(handle<regex.pattern> pattern, buffer text, out value<string[]?> parts, out string error)",
         "Split a text at each match\n"
         "@param pattern The pattern of what separates the parts, such as \",\\\\s*\"\n"
         "@param text The text\n"
         "@param parts The parts between the matches, in order; the whole text if nothing matches\n"
         "@param error Why the match failed; nothing if it did not"},
        {"escape", (ECSFunction)RegexEscape, "string(string text)",
         "Make a text into a pattern that matches it exactly, such as what the user typed in a search field\n"
         "@param text The text\n"
         "@return The pattern"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a pattern, such as pattern:match(text); C calls the functions themselves (OpenECS DESIGN 10.6)
    const char *methods[] = {"test", "match", "matchAll", "replace", "replaceFirst", "split"};

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, "pattern", methods[i], methods[i]));
    }

    return SHUResult_Ok;
}
