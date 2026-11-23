// the standard plugins' glue header for stb_ds: it sets stb_ds to use SDL's allocator and assertions, as SDL is the one library plugins share with
// OpenECS (OpenECS DESIGN 17.2); include it instead of stb_ds.h, and define STB_DS_IMPLEMENTATION in one file of each plugin

#include "SDL3/SDL_assert.h"
#include "SDL3/SDL_stdinc.h"

#define STBDS_REALLOC(context, pointer, size) SDL_realloc(pointer, size)
#define STBDS_FREE(context, pointer) SDL_free(pointer)
#define STBDS_ASSERT(condition) SDL_assert(condition)

#include "stb_ds.h"

// hmdel and shdel give a field's offset, a ptrdiff_t, where a size_t goes; clang warns of the sign with -Wconversion where long long is ptrdiff_t, as on Windows
#undef STBDS_OFFSETOF
#define STBDS_OFFSETOF(var, field) ((size_t)((char *)&(var)->field - (char *)(var)))
