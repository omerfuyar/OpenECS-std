// The sqlite standard plugin: SQLite databases, for tools that keep many records, such as notes, logs, caches and catalogues (DESIGN 23).
// SQLite is compiled into the plugin as an archive (DESIGN 1), with SDL's allocator. A database is a handle; a query gives its rows as a list of tables.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include "sqlite3.h"

#pragma region Source Only

/// @brief An open database.
typedef struct SqliteDatabase
{
    sqlite3 *connection;
} SqliteDatabase;

static struct
{
    ECSPlugin plugin;
    char *error;        // why the last call failed, kept until the next one
    bool sqliteStarted; // SQLite took SDL's allocator and started
} SQLITE = {0};

/// @brief Keeps why a call failed, and gives it.
static const char *SqliteFail(const char *what, const char *why)
{
    SDL_free(SQLITE.error);
    SQLITE.error = nullptr;

    if (SDL_asprintf(&SQLITE.error, "%s: %s", what, why) < 0)
    {
        SQLITE.error = nullptr;
    }

    return SQLITE.error == nullptr ? "out of memory" : SQLITE.error;
}

#pragma region Allocator

// SQLite asks for an allocation's size, which SDL does not keep, so each allocation starts with its size; 16 bytes keep SQLite's 8-byte alignment
#define SQLITE_HEADER 16

static void *SqliteAllocate(int size)
{
    u8 *memory = SDL_malloc((usz)size + SQLITE_HEADER);

    if (memory == nullptr)
    {
        return nullptr;
    }

    *(int *)memory = size;
    return memory + SQLITE_HEADER;
}

static void SqliteDeallocate(void *memory)
{
    if (memory != nullptr)
    {
        SDL_free((u8 *)memory - SQLITE_HEADER);
    }
}

static void *SqliteReallocate(void *memory, int size)
{
    u8 *moved = SDL_realloc((u8 *)memory - SQLITE_HEADER, (usz)size + SQLITE_HEADER);

    if (moved == nullptr)
    {
        return nullptr;
    }

    *(int *)moved = size;
    return moved + SQLITE_HEADER;
}

static int SqliteSize(void *memory)
{
    return memory == nullptr ? 0 : *(int *)((u8 *)memory - SQLITE_HEADER);
}

static int SqliteRoundUp(int size)
{
    return (size + 7) & ~7;
}

static int SqliteInitMemory(void *data)
{
    (void)data;
    return SQLITE_OK;
}

static void SqliteShutdownMemory(void *data)
{
    (void)data;
}

#pragma endregion Allocator

#pragma region Values

/// @brief Binds a value to a parameter of a statement: nil is NULL, a bool is 0 or 1, and a buffer is a blob.
static int SqliteBind(sqlite3_stmt *statement, int index, const ECSValue *value)
{
    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Bool:
        return sqlite3_bind_int(statement, index, ECSValue_GetBool(value, false) ? 1 : 0);
    case ECSValueType_Integer:
        return sqlite3_bind_int64(statement, index, ECSValue_GetInteger(value, 0));
    case ECSValueType_Number:
        return sqlite3_bind_double(statement, index, ECSValue_GetNumber(value, 0.0));
    case ECSValueType_String:
        return sqlite3_bind_text(statement, index, ECSValue_GetString(value, ""), -1, SQLITE_TRANSIENT);
    case ECSValueType_Buffer:
    {
        SHUSlice buffer = ECSValue_GetBuffer(value, cs(nullptr, 0));
        return sqlite3_bind_blob64(statement, index, buffer.data, buffer.size, SQLITE_TRANSIENT);
    }
    case ECSValueType_Nil:
        return sqlite3_bind_null(statement, index);
    default:
        return SQLITE_MISMATCH;
    }
}

/// @brief Finds a named parameter by its name, written with any of SQLite's prefixes: :name, @name or $name.
static int SqliteFindParameter(sqlite3_stmt *statement, const char *name)
{
    char prefixed[256];
    const char prefixes[] = {':', '@', '$'};

    for (usz i = 0; i < SDL_arraysize(prefixes); i++)
    {
        SDL_snprintf(prefixed, sizeof(prefixed), "%c%s", prefixes[i], name);
        int index = sqlite3_bind_parameter_index(statement, prefixed);

        if (index > 0)
        {
            return index;
        }
    }

    return 0;
}

/// @brief Binds a statement's parameters: a list's items to ?, in order, and named fields to their names.
static const char *SqliteBindAll(sqlite3_stmt *statement, const ECSValue *parameters)
{
    if (parameters == nullptr || ECSValue_GetType(parameters) != ECSValueType_Table)
    {
        return nullptr;
    }

    for (usz i = 0; i < ECSValue_GetListCount(parameters); i++)
    {
        if (SqliteBind(statement, (int)i + 1, ECSValue_GetListItem(parameters, i)) != SQLITE_OK)
        {
            return "a parameter is not nil, a bool, a number, a text or a buffer, or the query has fewer";
        }
    }

    for (usz i = 0; i < ECSValue_GetTableFieldCount(parameters); i++)
    {
        const char *name = nullptr;
        const ECSValue *value = ECSValue_GetTableFieldAt(parameters, i, &name);
        int index = SqliteFindParameter(statement, name);

        if (index == 0)
        {
            return "a named parameter is not in the query";
        }

        if (SqliteBind(statement, index, value) != SQLITE_OK)
        {
            return "a parameter is not nil, a bool, a number, a text or a buffer";
        }
    }

    return nullptr;
}

/// @brief Sets a row's field to a column's value; a NULL leaves the field out.
static SHUResult SqliteSetColumn(ECSValue *row, sqlite3_stmt *statement, int column)
{
    int type = sqlite3_column_type(statement, column);

    if (type == SQLITE_NULL)
    {
        return SHUResult_Ok;
    }

    ECSValue *field = nullptr;
    SHU_ReturnResult(ECSValue_SetTableField(row, sqlite3_column_name(statement, column), &field));

    switch (type)
    {
    case SQLITE_INTEGER:
        ECSValue_SetInteger(field, sqlite3_column_int64(statement, column));
        return SHUResult_Ok;
    case SQLITE_FLOAT:
        ECSValue_SetNumber(field, sqlite3_column_double(statement, column));
        return SHUResult_Ok;
    case SQLITE_BLOB:
    {
        const void *data = sqlite3_column_blob(statement, column);
        return ECSValue_SetBuffer(field, (SHUSliceView){.data = data, .size = (usz)sqlite3_column_bytes(statement, column)});
    }
    default:
    {
        const unsigned char *text = sqlite3_column_text(statement, column);
        return text == nullptr ? SHUResult_ErrAllocation : ECSValue_SetString(field, (const char *)text);
    }
    }
}

/// @brief Steps a statement to its end, and adds each row it gives to a list.
static const char *SqliteStep(sqlite3 *connection, sqlite3_stmt *statement, ECSValue *rows)
{
    int status = SQLITE_ROW;

    while ((status = sqlite3_step(statement)) == SQLITE_ROW)
    {
        ECSValue *row = nullptr;

        if (ECSValue_AddListItem(rows, &row))
        {
            return "out of memory";
        }

        ECSValue_SetTable(row);

        for (int column = 0; column < sqlite3_column_count(statement); column++)
        {
            if (SqliteSetColumn(row, statement, column))
            {
                return "out of memory";
            }
        }
    }

    return status == SQLITE_DONE ? nullptr : sqlite3_errmsg(connection);
}

#pragma endregion Values

static void SqliteDatabaseDestroy(void *object)
{
    SqliteDatabase *database = object;

    // close_v2 waits for statements that are still open; the plugin finalizes each before it returns, so it closes at once
    sqlite3_close_v2(database->connection);
    SDL_free(database);
}

static SqliteDatabase *SqliteOpen(const char *path, const char **retError)
{
    *retError = nullptr;
    char *fullPath = nullptr;
    bool memory = SDL_strcmp(path, ":memory:") == 0;

    if (!memory && ECSPath_GetFull(&fullPath, path))
    {
        *retError = SqliteFail("the database cannot open", "the path is not valid");
        return nullptr;
    }

    SqliteDatabase *database = SDL_calloc(1, sizeof(SqliteDatabase));
    int status = database == nullptr ? SQLITE_NOMEM : sqlite3_open_v2(memory ? path : fullPath, &database->connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    ECSPath_Free(fullPath);

    if (status != SQLITE_OK)
    {
        *retError = SqliteFail("the database cannot open", database == nullptr || database->connection == nullptr ? sqlite3_errstr(status) : sqlite3_errmsg(database->connection));

        if (database != nullptr)
        {
            sqlite3_close_v2(database->connection);
        }

        SDL_free(database);
        return nullptr;
    }

    // another program that writes the database makes this one wait up to a few seconds, not fail at once
    sqlite3_busy_timeout(database->connection, 5000);
    return database;
}

static bool SqliteExec(SqliteDatabase *database, const char *sql, const char **retError)
{
    *retError = nullptr;
    char *message = nullptr;

    if (sqlite3_exec(database->connection, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        *retError = SqliteFail("the SQL failed", message == nullptr ? sqlite3_errmsg(database->connection) : message);
        sqlite3_free(message);
        return false;
    }

    return true;
}

static void SqliteQuery(SqliteDatabase *database, const char *sql, const ECSValue *parameters, ECSValue *retRows, const char **retError)
{
    *retError = nullptr;
    ECSValue_SetNil(retRows);
    sqlite3_stmt *statement = nullptr;
    const char *rest = nullptr;

    if (sqlite3_prepare_v2(database->connection, sql, -1, &statement, &rest) != SQLITE_OK)
    {
        *retError = SqliteFail("the query cannot be read", sqlite3_errmsg(database->connection));
        return;
    }

    if (statement == nullptr)
    {
        *retError = SqliteFail("the query cannot be read", "it holds no statement");
        return;
    }

    const char *problem = SqliteBindAll(statement, parameters);
    ECSValue_SetTable(retRows);

    if (problem == nullptr)
    {
        problem = SqliteStep(database->connection, statement, retRows);
    }

    if (problem != nullptr)
    {
        *retError = SqliteFail("the query failed", problem);
        ECSValue_SetNil(retRows);
    }

    sqlite3_finalize(statement);
}

static i64 SqliteChanges(SqliteDatabase *database)
{
    return sqlite3_changes64(database->connection);
}

static i64 SqliteLastId(SqliteDatabase *database)
{
    return sqlite3_last_insert_rowid(database->connection);
}

static void SqliteClose(SqliteDatabase *database)
{
    ECSHandle_Free(SQLITE.plugin, "database", database);
}

static void SqliteFree(void *data)
{
    (void)data;

    if (SQLITE.sqliteStarted)
    {
        sqlite3_shutdown();
    }

    SDL_free(SQLITE.error);
    SDL_zero(SQLITE);
}

/// @brief Gives SQLite SDL's allocator, which it takes only before it starts, and starts it.
static SHUResult SqliteStart(void)
{
    static const sqlite3_mem_methods ALLOCATOR = {SqliteAllocate, SqliteDeallocate, SqliteReallocate, SqliteSize, SqliteRoundUp, SqliteInitMemory, SqliteShutdownMemory, nullptr};

    if (sqlite3_config(SQLITE_CONFIG_MALLOC, &ALLOCATOR) != SQLITE_OK || sqlite3_initialize() != SQLITE_OK)
    {
        ECS_LogError(SQLITE.plugin, "SQLite cannot start.");
        return SHUResult_Err;
    }

    SQLITE.sqliteStarted = true;
    return SHUResult_Ok;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    SQLITE.plugin = plugin;
    const ECSPluginDesc desc = {.Destroy = SqliteFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(SqliteStart());
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "database", SqliteDatabaseDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"open", (ECSFunction)SqliteOpen, "handle<sqlite.database>(string path, out string error)",
         "Open a database file, and create it if it is missing\n"
         "@param path The file's path, a relative one from the executable's folder; \":memory:\" for a database in memory, which is gone once it closes\n"
         "@param error Why it cannot open; nothing if it opens\n"
         "@return The database; nothing if it cannot open. One whose handle is gone closes"},
        {"exec", (ECSFunction)SqliteExec, "bool(handle<sqlite.database> database, string sql, out string error)",
         "Run SQL statements, separated by semicolons, which give no rows, such as CREATE TABLE\n"
         "@param database The database\n"
         "@param sql The statements\n"
         "@param error Why they failed; nothing if they ran\n"
         "@return false if one failed; the ones before it ran"},
        {"query", (ECSFunction)SqliteQuery, "void(handle<sqlite.database> database, string sql, value parameters, out value<sqlite.Row[]?> rows, out string error)",
         "Run one SQL statement with parameters, and give its rows\n"
         "@param database The database\n"
         "@param sql The statement, with ? for a list's parameters, or :name, @name or $name for a table's named fields\n"
         "@param parameters A list, or a table of named fields, of nils, bools, numbers, texts and buffers; nil for none\n"
         "@param rows Each row a table of its columns by name, which leaves out NULLs; empty for a statement without rows; nothing if it failed\n"
         "@param error Why it failed; nothing if it ran"},
        {"changes", (ECSFunction)SqliteChanges, "int64(handle<sqlite.database> database)",
         "Give how many rows the last INSERT, UPDATE or DELETE changed\n"
         "@param database The database\n"
         "@return The count"},
        {"lastId", (ECSFunction)SqliteLastId, "int64(handle<sqlite.database> database)",
         "Give the rowid of the last row that an INSERT added\n"
         "@param database The database\n"
         "@return The rowid; 0 before any"},
        {"close", (ECSFunction)SqliteClose, "void(handle<sqlite.database> database)",
         "Close the database at once; its handles are invalid from now on\n"
         "@param database The database"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a handle, such as database:query(sql); C calls the functions themselves (OpenECS DESIGN 10.6)
    const char *methods[] = {"exec", "query", "changes", "lastId", "close"};

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, "database", methods[i], methods[i]));
    }

    return SHUResult_Ok;
}
