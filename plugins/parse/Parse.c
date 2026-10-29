// The parse standard plugin: reads JSON, TOML, INI, .env and CSV texts into values, which are Lua tables in Lua, and writes values back into them (DESIGN 17).
// yyjson reads and writes JSON, and tomlc17 reads TOML; both are compiled into this plugin (DESIGN 1). The other formats are read and written here.
// A function that fails gives nothing and why; the reason, and the text that a write gives, are kept until the plugin's next call.

#include "Parse.h"

#include "SDL3/SDL.h"
#include "tomlc17.h"

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

static struct
{
    char *error; // why the last call failed
    char *text;  // what the last write gave
} PARSE = {0};

/// @brief Compares two field names, for SDL_qsort.
static int SDLCALL ParseCompareNames(const void *first, const void *second)
{
    return SDL_strcmp(*(const char *const *)first, *(const char *const *)second);
}

/// @brief Frees what the plugin keeps; the Destroy function of the plugin.
static void ParseFree(void *data)
{
    (void)data;
    SDL_free(PARSE.error);
    SDL_free(PARSE.text);
    SDL_zero(PARSE);
}

#pragma endregion Source Only

const char *ParseFail(const char *format, ...)
{
    SDL_free(PARSE.error);
    PARSE.error = nullptr;

    va_list arguments;
    va_start(arguments, format);
    int length = SDL_vasprintf(&PARSE.error, format, arguments);
    va_end(arguments);

    // a failure always says why, even when there was no memory for the reason
    return length < 0 || PARSE.error == nullptr ? "out of memory" : PARSE.error;
}

const char *ParseKeepText(char **text)
{
    SDL_free(PARSE.text);
    PARSE.text = SDL_malloc(arrlenu(*text) + 1);

    if (PARSE.text != nullptr)
    {
        SDL_memcpy(PARSE.text, *text, arrlenu(*text));
        PARSE.text[arrlenu(*text)] = '\0';
    }

    arrfree(*text);
    return PARSE.text;
}

void ParseAddText(char **text, const char *add, usz length)
{
    if (length > 0)
    {
        SDL_memcpy(arraddnptr(*text, length), add, length);
    }
}

void ParseAddString(char **text, const char *add)
{
    ParseAddText(text, add, SDL_strlen(add));
}

void ParseAddFormat(char **text, const char *format, ...)
{
    char *formatted = nullptr;
    va_list arguments;
    va_start(arguments, format);
    int length = SDL_vasprintf(&formatted, format, arguments);
    va_end(arguments);

    if (length > 0)
    {
        ParseAddText(text, formatted, (usz)length);
    }

    SDL_free(formatted);
}

void ParseAddNumber(char **text, f64 number)
{
    char digits[32] = {0};

    // the fewest digits that read back as the same number
    for (int precision = 1; precision <= 17; precision++)
    {
        SDL_snprintf(digits, sizeof(digits), "%.*g", precision, number);

        if (SDL_strtod(digits, nullptr) == number)
        {
            break;
        }
    }

    ParseAddString(text, digits);

    if (SDL_strpbrk(digits, ".eEn") == nullptr)
    {
        ParseAddString(text, ".0");
    }
}

const char **ParseSortFields(const ECSValue *table)
{
    const char **names = nullptr;

    for (usz i = 0; i < ECSValue_GetTableFieldCount(table); i++)
    {
        const char *name = nullptr;
        ECSValue_GetTableFieldAt(table, i, &name);
        arrput(names, name);
    }

    if (names != nullptr)
    {
        SDL_qsort((void *)names, arrlenu(names), sizeof(*names), ParseCompareNames);
    }

    return names;
}

const char *ParseFind(const char *text, usz length, char character)
{
    for (usz i = 0; i < length; i++)
    {
        if (text[i] == character)
        {
            return &text[i];
        }
    }

    return nullptr;
}

bool ParseHasZero(const char *text, usz length)
{
    return ParseFind(text, length, '\0') != nullptr;
}

SHUResult ParseSetText(ECSValue *value, const char *text, usz length)
{
    if (ParseHasZero(text, length))
    {
        return ECSValue_SetBuffer(value, (SHUSliceView){text, length});
    }

    char *copy = SDL_strndup(text, length);

    if (copy == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    SHUResult result = ECSValue_SetString(value, copy);
    SDL_free(copy);
    return result;
}

const char *ParseGetText(const ECSValue *value, usz *retLength)
{
    switch (ECSValue_GetType(value))
    {
    case ECSValueType_String:
    {
        const char *text = ECSValue_GetString(value, "");
        *retLength = SDL_strlen(text);
        return text;
    }
    case ECSValueType_Buffer:
    {
        SHUSlice buffer = ECSValue_GetBuffer(value, cs(nullptr, 0));
        *retLength = buffer.size;
        return buffer.data;
    }
    default:
        *retLength = 0;
        return nullptr;
    }
}

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    const ECSPluginDesc desc = {.Destroy = ParseFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));

    // tomlc17 takes SDL's allocator, as the plugin's other libraries do (DESIGN 1.3)
    toml_option_t option = toml_default_option();
    option.check_utf8 = true;
    option.mem_realloc = SDL_realloc;
    option.mem_free = SDL_free;
    toml_set_option(option);

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"json.read", (ECSFunction)ParseReadJson, "void(buffer text, out value<any> data, out string error)",
         "Read a JSON text: objects become tables of named fields, arrays lists, and null nil; comments and trailing commas are allowed\n"
         "@param text The JSON text\n"
         "@param data What the text holds; nothing if it cannot be read\n"
         "@param error Why the text cannot be read, with its line and column; nothing if it can"},
        {"json.write", (ECSFunction)ParseWriteJson, "string(value data, bool pretty, out string error)",
         "Write a value as a JSON text: a table of named fields as an object, with its names sorted, and a list as an array\n"
         "@param data The value; a table is either a list or named fields, and an empty table is written as {}\n"
         "@param pretty True to write each item on a line of its own, indented by four spaces\n"
         "@param error Why the value cannot be written, such as a handle or an infinite number in it; nothing if it can\n"
         "@return The text, kept until the plugin's next write; nothing if the value cannot be written"},
        {"toml.read", (ECSFunction)ParseReadToml, "void(buffer text, out value<table?> data, out string error)",
         "Read a TOML text into a table: tables become tables of named fields, and arrays lists; dates and times become texts, as TOML writes them\n"
         "@param text The TOML text\n"
         "@param data What the text holds; nothing if it cannot be read\n"
         "@param error Why the text cannot be read, with its line; nothing if it can"},
        {"toml.write", (ECSFunction)ParseWriteToml, "string(value<table> data, out string error)",
         "Write a table as a TOML text: its values first, then its tables as [sections], with names sorted\n"
         "@param data The table of named fields; its lists hold values of one kind, and a list of tables is written as [[sections]]\n"
         "@param error Why the table cannot be written, such as a handle in it; nothing if it can\n"
         "@return The text, kept until the plugin's next write; nothing if the table cannot be written"},
        {"ini.read", (ECSFunction)ParseReadIni, "void(buffer text, out value<table?> data, out string error)",
         "Read an INI text into a table: each [section] is a table of its keys, and the keys before the first section are fields of the table itself; every value is a text\n"
         "@param text The INI text: key = value lines, and comments on lines that start with ; or #\n"
         "@param data What the text holds; nothing if it cannot be read\n"
         "@param error Why the text cannot be read, with its line; nothing if it can"},
        {"ini.write", (ECSFunction)ParseWriteIni, "string(value<table> data, out string error)",
         "Write a table as an INI text: its texts, numbers and booleans first, then each of its tables as a [section], with names sorted\n"
         "@param data The table of named fields\n"
         "@param error Why the table cannot be written, such as a list or a table inside a section; nothing if it can\n"
         "@return The text, kept until the plugin's next write; nothing if the table cannot be written"},
        {"env.read", (ECSFunction)ParseReadEnv, "void(buffer text, out value<parse.Env?> data, out string error)",
         "Read a .env text into a table of texts: KEY=value lines, which may start with export; values may be in quotes, and # starts a comment\n"
         "@param text The .env text\n"
         "@param data Each key's value, as a text; nothing if the text cannot be read\n"
         "@param error Why the text cannot be read, with its line; nothing if it can"},
        {"env.write", (ECSFunction)ParseWriteEnv, "string(value<table> data, out string error)",
         "Write a table as a .env text: a KEY=value line for each field, with names sorted, and values in double quotes when they need them\n"
         "@param data The table of named fields, each a text, a number or a boolean\n"
         "@param error Why the table cannot be written, such as a key that is not a name; nothing if it can\n"
         "@return The text, kept until the plugin's next write; nothing if the table cannot be written"},
        {"csv.read", (ECSFunction)ParseReadCsv, "void(buffer text, string separator, out value<string[][]?> rows, out string error)",
         "Read a CSV text into a list of rows, each a list of texts; fields may be in double quotes, which hold separators, line breaks and doubled quotes\n"
         "@param text The CSV text\n"
         "@param separator The character between fields; empty for a comma\n"
         "@param rows The rows; nothing if the text cannot be read\n"
         "@param error Why the text cannot be read, with its line; nothing if it can"},
        {"csv.write", (ECSFunction)ParseWriteCsv, "string(value<any[][]> rows, string separator, out string error)",
         "Write a list of rows as a CSV text, each row a list of texts, numbers or booleans; a field goes in double quotes when it holds a separator, a quote or a line break\n"
         "@param rows The rows\n"
         "@param separator The character between fields; empty for a comma\n"
         "@param error Why the rows cannot be written, such as a table in a field; nothing if they can\n"
         "@return The text, each row ending with a line break, kept until the plugin's next write; nothing if the rows cannot be written"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
