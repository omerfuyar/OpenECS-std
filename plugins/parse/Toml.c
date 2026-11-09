// TOML, which tomlc17 reads and the plugin writes (DESIGN 17).

#include "Parse.h"

#include "SDL3/SDL.h"
#include "stbSDL3.h"
#include "tomlc17.h"

#pragma region Source Only

/// @brief Makes a value the text of a date, a time or both, as TOML writes them, such as 1979-05-27T07:32:00Z.
static SHUResult ParseSetTomlTime(const toml_datum_t *datum, ECSValue *retValue)
{
    char *text = nullptr;

    if (datum->type != TOML_TIME)
    {
        ParseAddFormat(&text, "%04d-%02d-%02d", datum->u.ts.year, datum->u.ts.month, datum->u.ts.day);
    }

    if (datum->type != TOML_DATE)
    {
        ParseAddFormat(&text, "%s%02d:%02d:%02d", datum->type == TOML_TIME ? "" : "T", datum->u.ts.hour, datum->u.ts.minute, datum->u.ts.second);

        if (datum->u.ts.usec != 0)
        {
            ParseAddFormat(&text, ".%06d", (int)datum->u.ts.usec);
        }
    }

    if (datum->type == TOML_DATETIMETZ && datum->u.ts.tz == 0)
    {
        ParseAddString(&text, "Z");
    }
    else if (datum->type == TOML_DATETIMETZ)
    {
        int minutes = datum->u.ts.tz < 0 ? -datum->u.ts.tz : datum->u.ts.tz;
        ParseAddFormat(&text, "%c%02d:%02d", datum->u.ts.tz < 0 ? '-' : '+', minutes / 60, minutes % 60);
    }

    SHUResult result = ParseSetText(retValue, text, arrlenu(text));
    arrfree(text);
    return result;
}

/// @brief Makes a value what a TOML value holds.
static SHUResult ParseFromToml(const toml_datum_t *datum, ECSValue *retValue)
{
    switch (datum->type)
    {
    case TOML_STRING:
        return ParseSetText(retValue, datum->u.str.ptr, (usz)datum->u.str.len);
    case TOML_INT64:
        ECSValue_SetInteger(retValue, datum->u.int64);
        return SHUResult_Ok;
    case TOML_FP64:
        ECSValue_SetNumber(retValue, datum->u.fp64);
        return SHUResult_Ok;
    case TOML_BOOLEAN:
        ECSValue_SetBool(retValue, datum->u.boolean);
        return SHUResult_Ok;
    case TOML_DATE:
    case TOML_TIME:
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
        return ParseSetTomlTime(datum, retValue);
    case TOML_ARRAY:
        ECSValue_SetTable(retValue);

        for (int32_t i = 0; i < datum->u.arr.size; i++)
        {
            ECSValue *item = nullptr;
            SHU_ReturnResult(ECSValue_AddListItem(retValue, &item));
            SHU_ReturnResult(ParseFromToml(&datum->u.arr.elem[i], item));
        }

        return SHUResult_Ok;
    case TOML_TABLE:
        ECSValue_SetTable(retValue);

        for (int32_t i = 0; i < datum->u.tab.size; i++)
        {
            char *name = SDL_strndup(datum->u.tab.key[i], (usz)datum->u.tab.len[i]);
            ECSValue *field = nullptr;
            SHUResult result = name == nullptr ? SHUResult_ErrAllocation : ECSValue_SetTableField(retValue, name, &field);
            SDL_free(name);
            SHU_ReturnResult(result);
            SHU_ReturnResult(ParseFromToml(&datum->u.tab.value[i], field));
        }

        return SHUResult_Ok;
    default:
        ECSValue_SetNil(retValue);
        return SHUResult_Ok;
    }
}

/// @brief Tells whether a key can be written bare: letters, digits, '_' and '-'.
static bool ParseIsBareKey(const char *key)
{
    if (key[0] == '\0')
    {
        return false;
    }

    for (const char *character = key; *character != '\0'; character++)
    {
        if (!SDL_isalnum(*character) && *character != '_' && *character != '-')
        {
            return false;
        }
    }

    return true;
}

/// @brief Adds a text in double quotes, with its quotes, backslashes and control characters escaped.
static void ParseAddTomlString(char **text, const char *string, usz length)
{
    ParseAddString(text, "\"");

    for (usz i = 0; i < length; i++)
    {
        unsigned char character = (unsigned char)string[i];

        switch (character)
        {
        case '"':
            ParseAddString(text, "\\\"");
            break;
        case '\\':
            ParseAddString(text, "\\\\");
            break;
        case '\n':
            ParseAddString(text, "\\n");
            break;
        case '\t':
            ParseAddString(text, "\\t");
            break;
        case '\r':
            ParseAddString(text, "\\r");
            break;
        default:
            if (character < 0x20 || character == 0x7F)
            {
                ParseAddFormat(text, "\\u%04X", character);
            }
            else
            {
                ParseAddText(text, (const char *)&string[i], 1);
            }
        }
    }

    ParseAddString(text, "\"");
}

static void ParseAddTomlKey(char **text, const char *key)
{
    if (ParseIsBareKey(key))
    {
        ParseAddString(text, key);
    }
    else
    {
        ParseAddTomlString(text, key, SDL_strlen(key));
    }
}

/// @brief Tells whether a table is a list of tables, which TOML writes as [[sections]].
static bool ParseIsTableList(const ECSValue *value)
{
    usz count = ECSValue_GetListCount(value);

    if (count == 0 || ECSValue_GetTableFieldCount(value) > 0)
    {
        return false;
    }

    for (usz i = 0; i < count; i++)
    {
        const ECSValue *item = ECSValue_GetListItem(value, i);

        if (ECSValue_GetType(item) != ECSValueType_Table || ECSValue_GetListCount(item) > 0)
        {
            return false;
        }
    }

    return true;
}

/// @brief Tells whether a field is written as a section, [name] or [[name]], rather than as name = value.
static bool ParseIsTomlSection(const ECSValue *value)
{
    return ECSValue_GetType(value) == ECSValueType_Table && (ECSValue_GetListCount(value) == 0 || ParseIsTableList(value));
}

static bool ParseAddTomlValue(char **text, const ECSValue *value, const char **retError);

/// @brief Adds a list as [a, b], or a table of named fields as { a = 1, b = 2 }.
static bool ParseAddTomlInline(char **text, const ECSValue *table, const char **retError)
{
    usz count = ECSValue_GetListCount(table);

    if (count > 0 && ECSValue_GetTableFieldCount(table) > 0)
    {
        *retError = ParseFail("TOML cannot hold a table that is both a list and named fields");
        return false;
    }

    bool written = true;
    ParseAddString(text, count > 0 ? "[" : "{");

    for (usz i = 0; i < count && written; i++)
    {
        ParseAddString(text, i == 0 ? "" : ", ");
        written = ParseAddTomlValue(text, ECSValue_GetListItem(table, i), retError);
    }

    const char **names = ParseSortFields(table);

    for (usz i = 0; i < arrlenu(names) && written; i++)
    {
        ParseAddString(text, i == 0 ? " " : ", ");
        ParseAddTomlKey(text, names[i]);
        ParseAddString(text, " = ");
        written = ParseAddTomlValue(text, ECSValue_GetTableField(table, names[i]), retError);
    }

    ParseAddString(text, count > 0 ? "]" : arrlenu(names) > 0 ? " }"
                                                              : "}");
    arrfree(names);
    return written;
}

/// @brief Adds a value as TOML writes it after a key's '='.
/// @return false and why if TOML cannot hold it.
static bool ParseAddTomlValue(char **text, const ECSValue *value, const char **retError)
{
    usz length = 0;
    f64 number = ECSValue_GetNumber(value, 0.0);

    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Bool:
        ParseAddString(text, ECSValue_GetBool(value, false) ? "true" : "false");
        return true;
    case ECSValueType_Integer:
        ParseAddFormat(text, "%" SDL_PRIs64, ECSValue_GetInteger(value, 0));
        return true;
    case ECSValueType_Number:
        if (SDL_isnan(number) || SDL_isinf(number))
        {
            ParseAddString(text, SDL_isnan(number) ? "nan" : number > 0.0 ? "inf"
                                                                          : "-inf");
        }
        else
        {
            ParseAddNumber(text, number);
        }
        return true;
    case ECSValueType_String:
    case ECSValueType_Buffer:
    {
        const char *string = ParseGetText(value, &length);
        ParseAddTomlString(text, string, length);
        return true;
    }
    case ECSValueType_Table:
        return ParseAddTomlInline(text, value, retError);
    case ECSValueType_Nil:
        *retError = ParseFail("TOML cannot hold nil in a list");
        return false;
    default:
        *retError = ParseFail("TOML cannot hold a handle");
        return false;
    }
}

static bool ParseAddTomlTable(char **text, const ECSValue *table, const char *path, const char **retError);

/// @brief Adds a field of a table that is written as a section: [path] with its table, or [[path]] for each table of its list.
static bool ParseAddTomlSection(char **text, const ECSValue *value, const char *path, const char **retError)
{
    usz count = ECSValue_GetListCount(value);
    bool written = true;

    if (count == 0)
    {
        // a table that holds only tables needs no header of its own: theirs name it
        bool header = ECSValue_GetTableFieldCount(value) == 0;

        for (usz i = 0; i < ECSValue_GetTableFieldCount(value) && !header; i++)
        {
            const char *name = nullptr;
            header = !ParseIsTomlSection(ECSValue_GetTableFieldAt(value, i, &name));
        }

        if (header)
        {
            ParseAddFormat(text, "%s[%s]\n", arrlenu(*text) > 0 ? "\n" : "", path);
        }

        return ParseAddTomlTable(text, value, path, retError);
    }

    for (usz i = 0; i < count && written; i++)
    {
        ParseAddFormat(text, "%s[[%s]]\n", arrlenu(*text) > 0 ? "\n" : "", path);
        written = ParseAddTomlTable(text, ECSValue_GetListItem(value, i), path, retError);
    }

    return written;
}

/// @brief Adds a table's fields: its values as key = value lines, then its tables as sections under its path.
/// @param path The table's key path, such as a.b, or an empty text for the top.
static bool ParseAddTomlTable(char **text, const ECSValue *table, const char *path, const char **retError)
{
    const char **names = ParseSortFields(table);
    bool written = true;

    for (usz i = 0; i < arrlenu(names) && written; i++)
    {
        const ECSValue *value = ECSValue_GetTableField(table, names[i]);

        if (!ParseIsTomlSection(value))
        {
            ParseAddTomlKey(text, names[i]);
            ParseAddString(text, " = ");
            written = ParseAddTomlValue(text, value, retError);
            ParseAddString(text, "\n");
        }
    }

    for (usz i = 0; i < arrlenu(names) && written; i++)
    {
        const ECSValue *value = ECSValue_GetTableField(table, names[i]);

        if (ParseIsTomlSection(value))
        {
            char *key = nullptr;
            ParseAddString(&key, path);
            ParseAddString(&key, path[0] == '\0' ? "" : ".");
            ParseAddTomlKey(&key, names[i]);
            arrput(key, '\0');
            written = ParseAddTomlSection(text, value, key, retError);
            arrfree(key);
        }
    }

    arrfree(names);
    return written;
}

#pragma endregion Source Only

void ParseReadToml(SHUSlice text, ECSValue *retData, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retData);

    if (text.size > (usz)INT32_MAX)
    {
        *retError = ParseFail("TOML: the text is too long");
        return;
    }

    toml_result_t result = toml_parse(text.data, (int)text.size);

    if (!result.ok)
    {
        *retError = ParseFail("TOML: %s", result.errmsg);
    }
    else if (ParseFromToml(&result.toptab, retData) != SHUResult_Ok)
    {
        *retError = ParseFail("out of memory");
        ECSValue_SetNil(retData);
    }

    toml_free(result);
}

const char *ParseWriteToml(const ECSValue *data, const char **retError)
{
    *retError = nullptr;

    if (ECSValue_GetType(data) != ECSValueType_Table || ECSValue_GetListCount(data) > 0)
    {
        *retError = ParseFail("TOML holds a table of named fields");
        return nullptr;
    }

    char *text = nullptr;

    if (!ParseAddTomlTable(&text, data, "", retError))
    {
        arrfree(text);
        return nullptr;
    }

    return ParseKeepText(&text);
}
