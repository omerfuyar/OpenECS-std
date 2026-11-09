#pragma once

// What the C files of the parse standard plugin share (DESIGN 17): building texts, and keeping the text that a write gives
// and the reason that a call fails until the plugin's next call.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Keeps why a call fails, until the next call, and gives it.
OPENECS_PRINTF(1, 2)
const char *ParseFail(const char *format, ...);

/// @brief Keeps a text that a write built, until the next write, and gives it; frees the builder.
/// @param text An stb_ds array of the text's characters, without a zero at its end.
/// @return The text, or nullptr when there is no memory for it.
const char *ParseKeepText(char **text);

/// @brief Adds characters to an stb_ds array of characters.
void ParseAddText(char **text, const char *add, usz length);

/// @brief Adds a text that ends with a zero, without the zero.
void ParseAddString(char **text, const char *add);

/// @brief Adds a formatted text.
OPENECS_PRINTF(2, 3)
void ParseAddFormat(char **text, const char *format, ...);

/// @brief Adds a number in the shortest form that reads back as the same number; it always has a '.' or an exponent, so it reads back as a number and not an integer.
void ParseAddNumber(char **text, f64 number);

/// @brief Gives the names of a table's named fields, sorted, so the same table always writes the same text.
/// @return An stb_ds array, which the caller frees with arrfree, or nullptr if the table has no named field.
const char **ParseSortFields(const ECSValue *table);

/// @brief Finds a character in a part of a text.
/// @return Where it is first, or nullptr if the part does not hold it.
const char *ParseFind(const char *text, usz length, char character);

/// @brief Tells whether a text holds a zero byte, which only a buffer can hold.
bool ParseHasZero(const char *text, usz length);

/// @brief Makes a value a string, or a buffer if the text holds a zero byte.
SHUResult ParseSetText(ECSValue *value, const char *text, usz length);

/// @brief Gives a value's text: a string's, or a buffer's bytes.
/// @return The text, or nullptr if the value is neither.
const char *ParseGetText(const ECSValue *value, usz *retLength);

void ParseReadJson(SHUSlice text, ECSValue *retData, const char **retError);
const char *ParseWriteJson(const ECSValue *data, bool pretty, const char **retError);
void ParseReadToml(SHUSlice text, ECSValue *retData, const char **retError);
const char *ParseWriteToml(const ECSValue *data, const char **retError);
void ParseReadIni(SHUSlice text, ECSValue *retData, const char **retError);
const char *ParseWriteIni(const ECSValue *data, const char **retError);
void ParseReadEnv(SHUSlice text, ECSValue *retData, const char **retError);
const char *ParseWriteEnv(const ECSValue *data, const char **retError);
void ParseReadCsv(SHUSlice text, const char *separator, ECSValue *retData, const char **retError);
const char *ParseWriteCsv(const ECSValue *rows, const char *separator, const char **retError);

#pragma endregion Declarations
