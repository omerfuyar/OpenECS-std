// JSON, which yyjson reads and writes (DESIGN 17).

#include "Parse.h"

#include "SDL3/SDL.h"
#include "stbSDL3.h"
#include "yyjson.h"

#pragma region Source Only

static void *ParseJsonMalloc(void *context, size_t size)
{
    (void)context;
    return SDL_malloc(size);
}

static void *ParseJsonRealloc(void *context, void *pointer, size_t oldSize, size_t size)
{
    (void)context;
    (void)oldSize;
    return SDL_realloc(pointer, size);
}

static void ParseJsonFree(void *context, void *pointer)
{
    (void)context;
    SDL_free(pointer);
}

/// @brief yyjson takes SDL's allocator, as the plugin's other libraries do (DESIGN 1.3).
static const yyjson_alc PARSE_JSON_ALLOCATOR = {ParseJsonMalloc, ParseJsonRealloc, ParseJsonFree, nullptr};

/// @brief Makes a value what a JSON value holds.
static SHUResult ParseFromJson(yyjson_val *json, ECSValue *retValue)
{
    switch (yyjson_get_type(json))
    {
    case YYJSON_TYPE_BOOL:
        ECSValue_SetBool(retValue, yyjson_get_bool(json));
        return SHUResult_Ok;
    case YYJSON_TYPE_NUM:
        // an integer too large for an int64 becomes a number
        if (yyjson_is_sint(json) || (yyjson_is_uint(json) && yyjson_get_uint(json) <= (u64)INT64_MAX))
        {
            ECSValue_SetInteger(retValue, yyjson_get_sint(json));
        }
        else
        {
            ECSValue_SetNumber(retValue, yyjson_get_num(json));
        }
        return SHUResult_Ok;
    case YYJSON_TYPE_STR:
        return ParseSetText(retValue, yyjson_get_str(json), yyjson_get_len(json));
    case YYJSON_TYPE_ARR:
    {
        ECSValue_SetTable(retValue);
        yyjson_arr_iter items = yyjson_arr_iter_with(json);

        for (yyjson_val *item = yyjson_arr_iter_next(&items); item != nullptr; item = yyjson_arr_iter_next(&items))
        {
            ECSValue *value = nullptr;
            SHU_ReturnResult(ECSValue_AddListItem(retValue, &value));
            SHU_ReturnResult(ParseFromJson(item, value));
        }

        return SHUResult_Ok;
    }
    case YYJSON_TYPE_OBJ:
    {
        ECSValue_SetTable(retValue);
        yyjson_obj_iter fields = yyjson_obj_iter_with(json);

        for (yyjson_val *key = yyjson_obj_iter_next(&fields); key != nullptr; key = yyjson_obj_iter_next(&fields))
        {
            // a null field is a missing one, as in Lua
            yyjson_val *field = yyjson_obj_iter_get_val(key);

            if (!yyjson_is_null(field))
            {
                ECSValue *value = nullptr;
                SHU_ReturnResult(ECSValue_SetTableField(retValue, yyjson_get_str(key), &value));
                SHU_ReturnResult(ParseFromJson(field, value));
            }
        }

        return SHUResult_Ok;
    }
    default:
        ECSValue_SetNil(retValue);
        return SHUResult_Ok;
    }
}

static yyjson_mut_val *ParseToJson(yyjson_mut_doc *document, const ECSValue *value, const char **retError);

/// @brief Makes a JSON array or object of a table.
static yyjson_mut_val *ParseTableToJson(yyjson_mut_doc *document, const ECSValue *table, const char **retError)
{
    usz itemCount = ECSValue_GetListCount(table);

    if (itemCount > 0 && ECSValue_GetTableFieldCount(table) > 0)
    {
        *retError = ParseFail("JSON cannot hold a table that is both a list and named fields");
        return nullptr;
    }

    if (itemCount > 0)
    {
        yyjson_mut_val *array = yyjson_mut_arr(document);

        for (usz i = 0; i < itemCount && array != nullptr; i++)
        {
            yyjson_mut_val *item = ParseToJson(document, ECSValue_GetListItem(table, i), retError);
            array = item != nullptr && yyjson_mut_arr_append(array, item) ? array : nullptr;
        }

        return array;
    }

    yyjson_mut_val *object = yyjson_mut_obj(document);
    const char **names = ParseSortFields(table);

    for (usz i = 0; i < arrlenu(names) && object != nullptr; i++)
    {
        yyjson_mut_val *field = ParseToJson(document, ECSValue_GetTableField(table, names[i]), retError);
        object = field != nullptr && yyjson_mut_obj_add(object, yyjson_mut_strcpy(document, names[i]), field) ? object : nullptr;
    }

    arrfree(names);
    return object;
}

/// @brief Makes a JSON value of a value.
/// @return The JSON value, or nullptr and why if the value cannot be written.
static yyjson_mut_val *ParseToJson(yyjson_mut_doc *document, const ECSValue *value, const char **retError)
{
    usz length = 0;

    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Nil:
        return yyjson_mut_null(document);
    case ECSValueType_Bool:
        return yyjson_mut_bool(document, ECSValue_GetBool(value, false));
    case ECSValueType_Integer:
        return yyjson_mut_sint(document, ECSValue_GetInteger(value, 0));
    case ECSValueType_Number:
        if (!SDL_isinf(ECSValue_GetNumber(value, 0.0)) && !SDL_isnan(ECSValue_GetNumber(value, 0.0)))
        {
            return yyjson_mut_real(document, ECSValue_GetNumber(value, 0.0));
        }

        *retError = ParseFail("JSON cannot hold an infinite number or NaN");
        return nullptr;
    case ECSValueType_String:
    case ECSValueType_Buffer:
    {
        const char *text = ParseGetText(value, &length);
        return yyjson_mut_strncpy(document, text, length);
    }
    case ECSValueType_Table:
        return ParseTableToJson(document, value, retError);
    default:
        *retError = ParseFail("JSON cannot hold a handle");
        return nullptr;
    }
}

#pragma endregion Source Only

void ParseReadJson(SHUSlice text, ECSValue *retData, const char **retError)
{
    *retError = nullptr;
    yyjson_read_err error = {0};
    yyjson_read_flag flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_ALLOW_BOM;

    // yyjson reads the text without changing it, as it is not read in place
    yyjson_doc *document = yyjson_read_opts((char *)text.data, text.size, flags, &PARSE_JSON_ALLOCATOR, &error);

    if (document == nullptr)
    {
        size_t line = 0;
        size_t column = 0;
        size_t character = 0;
        yyjson_locate_pos(text.data, text.size, error.pos, &line, &column, &character);
        *retError = ParseFail("JSON, at line %zu, column %zu: %s", line, column, error.msg);
        ECSValue_SetNil(retData);
        return;
    }

    if (ParseFromJson(yyjson_doc_get_root(document), retData) != SHUResult_Ok)
    {
        *retError = ParseFail("out of memory");
        ECSValue_SetNil(retData);
    }

    yyjson_doc_free(document);
}

const char *ParseWriteJson(const ECSValue *data, bool pretty, const char **retError)
{
    *retError = nullptr;
    yyjson_mut_doc *document = yyjson_mut_doc_new(&PARSE_JSON_ALLOCATOR);
    yyjson_mut_val *root = document == nullptr ? nullptr : ParseToJson(document, data, retError);

    if (root == nullptr)
    {
        *retError = *retError != nullptr ? *retError : ParseFail("out of memory");
        yyjson_mut_doc_free(document);
        return nullptr;
    }

    yyjson_mut_doc_set_root(document, root);
    yyjson_write_err error = {0};
    usz length = 0;
    char *written = yyjson_mut_write_opts(document, pretty ? YYJSON_WRITE_PRETTY : YYJSON_WRITE_NOFLAG, &PARSE_JSON_ALLOCATOR, &length, &error);
    yyjson_mut_doc_free(document);

    if (written == nullptr)
    {
        *retError = ParseFail("JSON: %s", error.msg);
        return nullptr;
    }

    char *text = nullptr;
    ParseAddText(&text, written, length);
    SDL_free(written);
    return ParseKeepText(&text);
}
