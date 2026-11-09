// INI, .env and CSV, which the plugin reads and writes itself, as they are small formats (DESIGN 17).

#include "Parse.h"

#include "SDL3/SDL.h"
#include "stbSDL3.h"

#pragma region Source Only

/// @brief A text being read: where it is, and the number of its line there.
typedef struct ParseReader
{
    const char *text;
    usz length;
    usz position;
    int line; // starts at 1
} ParseReader;

static bool ParseIsBlank(char character)
{
    return character == ' ' || character == '\t';
}

static bool ParseAtEnd(const ParseReader *reader)
{
    return reader->position >= reader->length;
}

static char ParsePeek(const ParseReader *reader)
{
    return ParseAtEnd(reader) ? '\0' : reader->text[reader->position];
}

/// @brief Takes the next character, and counts the lines it passes.
static char ParseTake(ParseReader *reader)
{
    char character = ParsePeek(reader);
    reader->position++;
    reader->line += character == '\n' ? 1 : 0;
    return character;
}

static void ParseSkipBlanks(ParseReader *reader)
{
    while (!ParseAtEnd(reader) && ParseIsBlank(ParsePeek(reader)))
    {
        reader->position++;
    }
}

/// @brief Takes the rest of the line, without its line break, and passes the line break.
/// @return Where the line starts; its length goes to retLength.
static const char *ParseTakeLine(ParseReader *reader, usz *retLength)
{
    const char *start = reader->text + reader->position;
    usz length = 0;

    while (!ParseAtEnd(reader) && ParsePeek(reader) != '\n')
    {
        reader->position++;
        length++;
    }

    ParseTake(reader);

    // a line break of Windows ends with \r\n
    if (length > 0 && start[length - 1] == '\r')
    {
        length--;
    }

    *retLength = length;
    return start;
}

/// @brief Cuts the blanks off both ends of a part of a text.
static const char *ParseTrim(const char *text, usz *length)
{
    while (*length > 0 && ParseIsBlank(text[0]))
    {
        text++;
        (*length)--;
    }

    while (*length > 0 && ParseIsBlank(text[*length - 1]))
    {
        (*length)--;
    }

    return text;
}

/// @brief Sets a table's field to a text, both given by their lengths.
static SHUResult ParseSetTextField(ECSValue *table, const char *name, usz nameLength, const char *text, usz textLength)
{
    char *key = SDL_strndup(name, nameLength);
    ECSValue *field = nullptr;
    SHUResult result = key == nullptr ? SHUResult_ErrAllocation : ECSValue_SetTableField(table, key, &field);
    SDL_free(key);
    return result ? result : ParseSetText(field, text, textLength);
}

/// @brief Adds a text, a number or a boolean as a plain text, as INI, .env and CSV write them.
/// @return false if the value is none of them.
static bool ParseAddPlain(char **text, const ECSValue *value)
{
    usz length = 0;

    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Bool:
        ParseAddString(text, ECSValue_GetBool(value, false) ? "true" : "false");
        return true;
    case ECSValueType_Integer:
        ParseAddFormat(text, "%" SDL_PRIs64, ECSValue_GetInteger(value, 0));
        return true;
    case ECSValueType_Number:
        ParseAddNumber(text, ECSValue_GetNumber(value, 0.0));
        return true;
    case ECSValueType_String:
    case ECSValueType_Buffer:
    {
        const char *string = ParseGetText(value, &length);
        ParseAddText(text, string, length);
        return true;
    }
    default:
        return false;
    }
}

/// @brief Gives the plain text of a field, as ParseAddPlain writes it, in a new stb_ds array without a zero at its end.
static char *ParseMakePlain(const ECSValue *value, bool *retPlain)
{
    char *text = nullptr;
    *retPlain = ParseAddPlain(&text, value);
    return text;
}

#pragma endregion Source Only

#pragma region INI

/// @brief Reads a [section] line, and makes its table the one that the next keys go in.
static bool ParseReadIniSection(const char *line, usz length, int number, ECSValue *data, ECSValue **retSection, const char **retError)
{
    if (line[length - 1] != ']')
    {
        *retError = ParseFail("INI, at line %d: a section's line ends with ]", number);
        return false;
    }

    usz nameLength = length - 2;
    const char *name = ParseTrim(line + 1, &nameLength);
    char *key = SDL_strndup(name, nameLength);

    if (key == nullptr || ECSValue_SetTableField(data, key, retSection) != SHUResult_Ok)
    {
        SDL_free(key);
        *retError = ParseFail("out of memory");
        return false;
    }

    // a section that comes again goes on with its keys
    if (ECSValue_GetType(*retSection) != ECSValueType_Table)
    {
        ECSValue_SetTable(*retSection);
    }

    SDL_free(key);
    return true;
}

/// @brief Reads a key = value line into the table its keys go in. A value in quotes loses them.
static bool ParseReadIniKey(const char *line, usz length, int number, ECSValue *section, const char **retError)
{
    const char *equals = ParseFind(line, length, '=');

    if (equals == nullptr || equals == line)
    {
        *retError = ParseFail("INI, at line %d: a line is a [section], a key = value, or a comment", number);
        return false;
    }

    usz keyLength = (usz)(equals - line);
    usz valueLength = length - keyLength - 1;
    const char *key = ParseTrim(line, &keyLength);
    const char *value = ParseTrim(equals + 1, &valueLength);

    if (valueLength >= 2 && (value[0] == '"' || value[0] == '\'') && value[valueLength - 1] == value[0])
    {
        value++;
        valueLength -= 2;
    }

    if (ParseSetTextField(section, key, keyLength, value, valueLength) != SHUResult_Ok)
    {
        *retError = ParseFail("out of memory");
        return false;
    }

    return true;
}

void ParseReadIni(SHUSlice text, ECSValue *retData, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetTable(retData);
    ECSValue *section = retData;
    ParseReader reader = {.text = text.data, .length = text.size, .line = 1};
    bool read = true;

    while (read && !ParseAtEnd(&reader))
    {
        int number = reader.line;
        usz length = 0;
        const char *line = ParseTakeLine(&reader, &length);
        line = ParseTrim(line, &length);

        if (length == 0 || line[0] == ';' || line[0] == '#')
        {
            continue;
        }

        read = line[0] == '[' ? ParseReadIniSection(line, length, number, retData, &section, retError) : ParseReadIniKey(line, length, number, section, retError);
    }

    if (!read)
    {
        ECSValue_SetNil(retData);
    }
}

/// @brief Adds key = value lines for the fields of a table that are not tables.
static bool ParseAddIniKeys(char **text, const ECSValue *table, const char *section, const char **retError)
{
    const char **names = ParseSortFields(table);
    bool written = true;

    for (usz i = 0; i < arrlenu(names) && written; i++)
    {
        const ECSValue *value = ECSValue_GetTableField(table, names[i]);

        if (ECSValue_GetType(value) == ECSValueType_Table && section == nullptr)
        {
            continue;
        }

        if (names[i][0] == '\0' || SDL_strpbrk(names[i], "=\r\n") != nullptr || SDL_strchr("[;#", names[i][0]) != nullptr || ParseIsBlank(names[i][0]))
        {
            *retError = ParseFail("INI cannot hold the key '%s'", names[i]);
            written = false;
            break;
        }

        bool plain = false;
        char *line = ParseMakePlain(value, &plain);
        usz trimmed = arrlenu(line);
        ParseTrim(line, &trimmed);

        if (!plain || ParseHasZero(line, arrlenu(line)) || ParseFind(line, arrlenu(line), '\n') != nullptr || ParseFind(line, arrlenu(line), '\r') != nullptr)
        {
            *retError = ParseFail("INI cannot hold the value of '%s%s%s': a section holds texts, numbers and booleans on one line", section != nullptr ? section : "",
                                  section != nullptr ? "." : "", names[i]);
            written = false;
        }
        else
        {
            // quotes keep the blanks at a value's ends, and a value that starts and ends with a quote
            bool quote = arrlenu(line) > 0 && (trimmed != arrlenu(line) || line[0] == '"' || line[0] == '\'');
            ParseAddFormat(text, "%s = %s", names[i], quote ? "\"" : "");
            ParseAddText(text, line, arrlenu(line));
            ParseAddString(text, quote ? "\"\n" : "\n");
        }

        arrfree(line);
    }

    arrfree(names);
    return written;
}

const char *ParseWriteIni(const ECSValue *data, const char **retError)
{
    *retError = nullptr;

    if (ECSValue_GetType(data) != ECSValueType_Table || ECSValue_GetListCount(data) > 0)
    {
        *retError = ParseFail("INI holds a table of named fields");
        return nullptr;
    }

    char *text = nullptr;
    bool written = ParseAddIniKeys(&text, data, nullptr, retError);
    const char **names = ParseSortFields(data);

    for (usz i = 0; i < arrlenu(names) && written; i++)
    {
        const ECSValue *section = ECSValue_GetTableField(data, names[i]);

        if (ECSValue_GetType(section) != ECSValueType_Table)
        {
            continue;
        }

        if (ECSValue_GetListCount(section) > 0 || SDL_strpbrk(names[i], "[]\r\n") != nullptr)
        {
            *retError = ParseFail("INI cannot hold the section '%s': a section is a table of named fields, and its name has no brackets", names[i]);
            written = false;
            break;
        }

        ParseAddFormat(&text, "%s[%s]\n", arrlenu(text) > 0 ? "\n" : "", names[i]);
        written = ParseAddIniKeys(&text, section, names[i], retError);
    }

    arrfree(names);

    if (!written)
    {
        arrfree(text);
        return nullptr;
    }

    return ParseKeepText(&text);
}

#pragma endregion INI

#pragma region Env

static bool ParseIsEnvName(char character, bool first)
{
    return SDL_isalpha(character) || character == '_' || (!first && (SDL_isdigit(character) || character == '.' || character == '-'));
}

/// @brief Reads a value in double quotes, which may span lines: \n, \r, \t, \\, \" and \$ are escapes.
static bool ParseReadEnvQuoted(ParseReader *reader, char **retValue)
{
    ParseTake(reader);

    while (!ParseAtEnd(reader) && ParsePeek(reader) != '"')
    {
        char character = ParseTake(reader);

        if (character == '\\' && !ParseAtEnd(reader))
        {
            char escaped = ParseTake(reader);
            character = escaped == 'n' ? '\n' : escaped == 'r' ? '\r'
                                            : escaped == 't'   ? '\t'
                                                               : escaped;

            // an unknown escape keeps its backslash
            if (SDL_strchr("nrt\\\"$", escaped) == nullptr)
            {
                arrput(*retValue, '\\');
            }
        }

        arrput(*retValue, character);
    }

    return ParseTake(reader) == '"';
}

/// @brief Reads a value in single quotes, which may span lines and has no escapes.
static bool ParseReadEnvLiteral(ParseReader *reader, char **retValue)
{
    ParseTake(reader);

    while (!ParseAtEnd(reader) && ParsePeek(reader) != '\'')
    {
        arrput(*retValue, ParseTake(reader));
    }

    return ParseTake(reader) == '\'';
}

/// @brief Reads a value without quotes: the rest of its line, up to a # after a blank.
static void ParseReadEnvPlain(ParseReader *reader, char **retValue)
{
    usz length = 0;
    const char *line = ParseTakeLine(reader, &length);

    for (usz i = 0; i < length; i++)
    {
        if (line[i] == '#' && (i == 0 || ParseIsBlank(line[i - 1])))
        {
            length = i;
        }
    }

    ParseTrim(line, &length);
    ParseAddText(retValue, line, length);
}

/// @brief Reads a value, and the rest of its line, which may hold only a comment.
static bool ParseReadEnvValue(ParseReader *reader, char **retValue, const char **retError)
{
    int number = reader->line;
    char quote = ParsePeek(reader);

    if (quote != '"' && quote != '\'')
    {
        ParseReadEnvPlain(reader, retValue);
        return true;
    }

    if (!(quote == '"' ? ParseReadEnvQuoted(reader, retValue) : ParseReadEnvLiteral(reader, retValue)))
    {
        *retError = ParseFail(".env, at line %d: the value's quote is not closed", number);
        return false;
    }

    ParseSkipBlanks(reader);
    usz length = 0;
    const char *rest = ParseTakeLine(reader, &length);

    if (length > 0 && rest[0] != '#')
    {
        *retError = ParseFail(".env, at line %d: only a comment may follow a value in quotes", reader->line - 1);
        return false;
    }

    return true;
}

/// @brief Reads a KEY=value line, which may start with export.
static bool ParseReadEnvLine(ParseReader *reader, ECSValue *data, const char **retError)
{
    int number = reader->line;

    if (reader->length - reader->position > 7 && SDL_strncmp(reader->text + reader->position, "export", 6) == 0 && ParseIsBlank(reader->text[reader->position + 6]))
    {
        reader->position += 6;
        ParseSkipBlanks(reader);
    }

    usz start = reader->position;

    while (!ParseAtEnd(reader) && ParseIsEnvName(ParsePeek(reader), reader->position == start))
    {
        reader->position++;
    }

    usz nameLength = reader->position - start;
    ParseSkipBlanks(reader);

    if (nameLength == 0 || ParsePeek(reader) != '=')
    {
        *retError = ParseFail(".env, at line %d: a line is KEY=value, or a comment", number);
        return false;
    }

    ParseTake(reader);
    ParseSkipBlanks(reader);
    char *value = nullptr;
    bool read = ParseReadEnvValue(reader, &value, retError);

    if (read && ParseSetTextField(data, reader->text + start, nameLength, value, arrlenu(value)) != SHUResult_Ok)
    {
        *retError = ParseFail("out of memory");
        read = false;
    }

    arrfree(value);
    return read;
}

void ParseReadEnv(SHUSlice text, ECSValue *retData, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetTable(retData);
    ParseReader reader = {.text = text.data, .length = text.size, .line = 1};
    bool read = true;

    while (read && !ParseAtEnd(&reader))
    {
        ParseSkipBlanks(&reader);
        char character = ParsePeek(&reader);

        if (character == '#' || character == '\n' || character == '\r' || ParseAtEnd(&reader))
        {
            usz length = 0;
            ParseTakeLine(&reader, &length);
            continue;
        }

        read = ParseReadEnvLine(&reader, retData, retError);
    }

    if (!read)
    {
        ECSValue_SetNil(retData);
    }
}

/// @brief Gives the escape of a character in a value in double quotes, or nullptr if it needs none.
static const char *ParseEnvEscape(char character)
{
    switch (character)
    {
    case '\n':
        return "\\n";
    case '\r':
        return "\\r";
    case '"':
        return "\\\"";
    case '\\':
        return "\\\\";
    case '$':
        return "\\$";
    default:
        return nullptr;
    }
}

/// @brief Adds a value in double quotes if it holds a blank or a character that a shell would read otherwise, with what reads back escaped.
static void ParseAddEnvValue(char **text, const char *value, usz length)
{
    bool quote = false;

    for (usz i = 0; i < length && !quote; i++)
    {
        quote = SDL_strchr("#\"'\\$\n\r \t", value[i]) != nullptr;
    }

    if (!quote)
    {
        ParseAddText(text, value, length);
        return;
    }

    ParseAddString(text, "\"");

    for (usz i = 0; i < length; i++)
    {
        const char *escape = ParseEnvEscape(value[i]);

        if (escape != nullptr)
        {
            ParseAddString(text, escape);
        }
        else
        {
            ParseAddText(text, &value[i], 1);
        }
    }

    ParseAddString(text, "\"");
}

const char *ParseWriteEnv(const ECSValue *data, const char **retError)
{
    *retError = nullptr;

    if (ECSValue_GetType(data) != ECSValueType_Table || ECSValue_GetListCount(data) > 0)
    {
        *retError = ParseFail(".env holds a table of named fields");
        return nullptr;
    }

    char *text = nullptr;
    const char **names = ParseSortFields(data);
    bool written = true;

    for (usz i = 0; i < arrlenu(names) && written; i++)
    {
        bool name = ParseIsEnvName(names[i][0], true);

        for (const char *character = names[i]; *character != '\0' && name; character++)
        {
            name = ParseIsEnvName(*character, false);
        }

        bool plain = false;
        char *value = ParseMakePlain(ECSValue_GetTableField(data, names[i]), &plain);

        if (!name || !plain || ParseHasZero(value, arrlenu(value)))
        {
            *retError = ParseFail(name ? ".env cannot hold the value of '%s': a key's value is a text, a number or a boolean" : ".env cannot hold the key '%s': a key is a name", names[i]);
            written = false;
        }
        else
        {
            ParseAddFormat(&text, "%s=", names[i]);
            ParseAddEnvValue(&text, value, arrlenu(value));
            ParseAddString(&text, "\n");
        }

        arrfree(value);
    }

    arrfree(names);

    if (!written)
    {
        arrfree(text);
        return nullptr;
    }

    return ParseKeepText(&text);
}

#pragma endregion Env

#pragma region CSV

/// @brief Gives the separator of fields: the separator's one character, or a comma if it is empty.
/// @return false and why if it is longer, or a quote or a line break.
static bool ParseReadSeparator(const char *separator, char *retSeparator, const char **retError)
{
    *retSeparator = separator[0] == '\0' ? ',' : separator[0];

    if (SDL_strlen(separator) > 1 || SDL_strchr("\"\r\n", *retSeparator) != nullptr)
    {
        *retError = ParseFail("CSV's separator is one character, and not a quote or a line break");
        return false;
    }

    return true;
}

/// @brief Reads a field in double quotes: two quotes in it are one.
static bool ParseReadCsvQuoted(ParseReader *reader, char separator, char **retField, const char **retError)
{
    int number = reader->line;
    ParseTake(reader);

    while (true)
    {
        if (ParseAtEnd(reader))
        {
            *retError = ParseFail("CSV, at line %d: the field's quote is not closed", number);
            return false;
        }

        char character = ParseTake(reader);

        if (character == '"' && ParsePeek(reader) != '"')
        {
            break;
        }

        // the second quote of two
        if (character == '"')
        {
            ParseTake(reader);
        }

        arrput(*retField, character);
    }

    char next = ParsePeek(reader);

    if (!ParseAtEnd(reader) && next != separator && next != '\n' && next != '\r')
    {
        *retError = ParseFail("CSV, at line %d: only a separator or a line break may follow a field's closing quote", reader->line);
        return false;
    }

    return true;
}

/// @brief Reads a row's fields, up to its line break.
static bool ParseReadCsvRow(ParseReader *reader, char separator, ECSValue *row, const char **retError)
{
    ECSValue_SetTable(row);

    while (true)
    {
        char *field = nullptr;
        bool read = true;

        if (ParsePeek(reader) == '"')
        {
            read = ParseReadCsvQuoted(reader, separator, &field, retError);
        }
        else
        {
            while (!ParseAtEnd(reader) && ParsePeek(reader) != separator && ParsePeek(reader) != '\n' && ParsePeek(reader) != '\r')
            {
                arrput(field, ParseTake(reader));
            }
        }

        ECSValue *item = nullptr;

        if (read && (ECSValue_AddListItem(row, &item) != SHUResult_Ok || ParseSetText(item, field, arrlenu(field)) != SHUResult_Ok))
        {
            *retError = ParseFail("out of memory");
            read = false;
        }

        arrfree(field);

        if (!read)
        {
            return false;
        }

        char next = ParseTake(reader);

        // a row ends with \n, \r\n or the end of the text
        if (next == '\r' && ParsePeek(reader) == '\n')
        {
            next = ParseTake(reader);
        }

        if (next != separator)
        {
            return true;
        }
    }
}

void ParseReadCsv(SHUSlice text, const char *separator, ECSValue *retData, const char **retError)
{
    *retError = nullptr;
    char character = ',';

    if (!ParseReadSeparator(separator, &character, retError))
    {
        ECSValue_SetNil(retData);
        return;
    }

    ECSValue_SetTable(retData);
    ParseReader reader = {.text = text.data, .length = text.size, .line = 1};
    bool read = true;

    while (read && !ParseAtEnd(&reader))
    {
        ECSValue *row = nullptr;
        read = ECSValue_AddListItem(retData, &row) == SHUResult_Ok && ParseReadCsvRow(&reader, character, row, retError);
        *retError = read || *retError != nullptr ? *retError : ParseFail("out of memory");
    }

    if (!read)
    {
        ECSValue_SetNil(retData);
    }
}

/// @brief Adds a field, in double quotes if it holds the separator, a quote or a line break, with its quotes doubled.
static void ParseAddCsvField(char **text, const char *field, usz length, char separator)
{
    bool quote = false;

    for (usz i = 0; i < length && !quote; i++)
    {
        quote = field[i] == separator || field[i] == '"' || field[i] == '\n' || field[i] == '\r';
    }

    ParseAddString(text, quote ? "\"" : "");

    for (usz i = 0; i < length; i++)
    {
        ParseAddText(text, field[i] == '"' ? "\"\"" : &field[i], field[i] == '"' ? 2 : 1);
    }

    ParseAddString(text, quote ? "\"" : "");
}

const char *ParseWriteCsv(const ECSValue *rows, const char *separator, const char **retError)
{
    *retError = nullptr;
    char character = ',';

    if (!ParseReadSeparator(separator, &character, retError))
    {
        return nullptr;
    }

    char *text = nullptr;
    bool written = ECSValue_GetType(rows) == ECSValueType_Table;

    for (usz i = 0; i < ECSValue_GetListCount(rows) && written; i++)
    {
        const ECSValue *row = ECSValue_GetListItem(rows, i);
        written = ECSValue_GetType(row) == ECSValueType_Table;

        for (usz j = 0; j < ECSValue_GetListCount(row) && written; j++)
        {
            const ECSValue *value = ECSValue_GetListItem(row, j);
            char *field = ParseMakePlain(value, &written);
            written = written || ECSValue_GetType(value) == ECSValueType_Nil;
            ParseAddString(&text, j == 0 ? "" : (char[]){character, '\0'});
            ParseAddCsvField(&text, field, arrlenu(field), character);
            arrfree(field);
        }

        ParseAddString(&text, "\n");
    }

    if (!written)
    {
        *retError = ParseFail("CSV holds a list of rows, each a list of texts, numbers or booleans");
        arrfree(text);
        return nullptr;
    }

    return ParseKeepText(&text);
}

#pragma endregion CSV
