// The hash standard plugin: checksums and hashes of data, for checking downloads and files, caches and content addresses (DESIGN 22).
// mbedTLS gives MD5 and the SHAs, zlib CRC-32, and xxHash its fast hashes; each is compiled into the plugin (DESIGN 1).

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "zlib.h"

// xxHash's code is compiled into this file, with its functions static
#define XXH_INLINE_ALL
#ifdef DEBUG
// gcc 16 with -Og fails to inline the functions that xxHash forces inline, so a Debug build lets gcc choose
#define XXH_NO_INLINE_HINTS 1
#endif
#include "xxhash.h"

#pragma region Source Only

/// @brief The algorithms, in the order of their names.
typedef enum HashAlgorithm
{
    HashAlgorithm_Crc32,
    HashAlgorithm_Md5,
    HashAlgorithm_Sha1,
    HashAlgorithm_Sha256,
    HashAlgorithm_Sha512,
    HashAlgorithm_Xxh64,
    HashAlgorithm_Xxh3,
    HashAlgorithm_Xxh128,
    HashAlgorithm_Count,
} HashAlgorithm;

static const char *const HASH_NAMES[] = {"crc32", "md5", "sha1", "sha256", "sha512", "xxh64", "xxh3", "xxh128"};

/// @brief A hash that takes its data in parts. xxHash's states need 64-byte alignment, so the hasher is allocated with it.
typedef struct HashHasher
{
    union
    {
        XXH3_state_t xxh3; // first, for its alignment
        XXH64_state_t xxh64;
        uLong crc32;
        mbedtls_md5_context md5;
        mbedtls_sha1_context sha1;
        mbedtls_sha256_context sha256;
        mbedtls_sha512_context sha512;
    } state;

    HashAlgorithm algorithm;
    bool finished;
} HashHasher;

static struct
{
    char digest[129]; // the last digest, as hexadecimal text, kept until the next one
} HASH = {0};

/// @brief Finds an algorithm by its name; HashAlgorithm_Count if there is none.
static HashAlgorithm HashFindAlgorithm(const char *name)
{
    for (usz i = 0; i < HashAlgorithm_Count; i++)
    {
        if (SDL_strcmp(HASH_NAMES[i], name) == 0)
        {
            return (HashAlgorithm)i;
        }
    }

    return HashAlgorithm_Count;
}

static void HashStart(HashHasher *hasher, HashAlgorithm algorithm)
{
    hasher->algorithm = algorithm;
    hasher->finished = false;

    switch (algorithm)
    {
    case HashAlgorithm_Crc32:
        hasher->state.crc32 = crc32_z(0, nullptr, 0);
        break;
    case HashAlgorithm_Md5:
        mbedtls_md5_init(&hasher->state.md5);
        mbedtls_md5_starts(&hasher->state.md5);
        break;
    case HashAlgorithm_Sha1:
        mbedtls_sha1_init(&hasher->state.sha1);
        mbedtls_sha1_starts(&hasher->state.sha1);
        break;
    case HashAlgorithm_Sha256:
        mbedtls_sha256_init(&hasher->state.sha256);
        mbedtls_sha256_starts(&hasher->state.sha256, 0);
        break;
    case HashAlgorithm_Sha512:
        mbedtls_sha512_init(&hasher->state.sha512);
        mbedtls_sha512_starts(&hasher->state.sha512, 0);
        break;
    case HashAlgorithm_Xxh64:
        XXH64_reset(&hasher->state.xxh64, 0);
        break;
    case HashAlgorithm_Xxh3:
        XXH3_INITSTATE(&hasher->state.xxh3);
        XXH3_64bits_reset(&hasher->state.xxh3);
        break;
    case HashAlgorithm_Xxh128:
        XXH3_INITSTATE(&hasher->state.xxh3);
        XXH3_128bits_reset(&hasher->state.xxh3);
        break;
    default:
        break;
    }
}

static void HashAdd(HashHasher *hasher, SHUSlice data)
{
    switch (hasher->algorithm)
    {
    case HashAlgorithm_Crc32:
        hasher->state.crc32 = crc32_z(hasher->state.crc32, data.data, data.size);
        break;
    case HashAlgorithm_Md5:
        mbedtls_md5_update(&hasher->state.md5, data.data, data.size);
        break;
    case HashAlgorithm_Sha1:
        mbedtls_sha1_update(&hasher->state.sha1, data.data, data.size);
        break;
    case HashAlgorithm_Sha256:
        mbedtls_sha256_update(&hasher->state.sha256, data.data, data.size);
        break;
    case HashAlgorithm_Sha512:
        mbedtls_sha512_update(&hasher->state.sha512, data.data, data.size);
        break;
    case HashAlgorithm_Xxh64:
        XXH64_update(&hasher->state.xxh64, data.data, data.size);
        break;
    case HashAlgorithm_Xxh3:
        XXH3_64bits_update(&hasher->state.xxh3, data.data, data.size);
        break;
    case HashAlgorithm_Xxh128:
        XXH3_128bits_update(&hasher->state.xxh3, data.data, data.size);
        break;
    default:
        break;
    }
}

/// @brief Writes bytes as hexadecimal text into the kept digest.
static const char *HashHex(const u8 *bytes, usz count)
{
    static const char DIGITS[] = "0123456789abcdef";

    for (usz i = 0; i < count; i++)
    {
        HASH.digest[i * 2] = DIGITS[bytes[i] >> 4];
        HASH.digest[(i * 2) + 1] = DIGITS[bytes[i] & 0xF];
    }

    HASH.digest[count * 2] = '\0';
    return HASH.digest;
}

/// @brief Writes a number's bytes, the highest first, as xxHash's canonical form and CRC-32's usual text are.
static usz HashBigEndian(u8 *bytes, u64 value, usz count)
{
    for (usz i = 0; i < count; i++)
    {
        bytes[i] = (u8)(value >> (8 * (count - 1 - i)));
    }

    return count;
}

/// @brief Ends a hash, and frees what its library holds.
static const char *HashFinish(HashHasher *hasher)
{
    u8 bytes[64];
    usz count = 0;

    switch (hasher->algorithm)
    {
    case HashAlgorithm_Crc32:
        count = HashBigEndian(bytes, hasher->state.crc32, 4);
        break;
    case HashAlgorithm_Md5:
        mbedtls_md5_finish(&hasher->state.md5, bytes);
        mbedtls_md5_free(&hasher->state.md5);
        count = 16;
        break;
    case HashAlgorithm_Sha1:
        mbedtls_sha1_finish(&hasher->state.sha1, bytes);
        mbedtls_sha1_free(&hasher->state.sha1);
        count = 20;
        break;
    case HashAlgorithm_Sha256:
        mbedtls_sha256_finish(&hasher->state.sha256, bytes);
        mbedtls_sha256_free(&hasher->state.sha256);
        count = 32;
        break;
    case HashAlgorithm_Sha512:
        mbedtls_sha512_finish(&hasher->state.sha512, bytes);
        mbedtls_sha512_free(&hasher->state.sha512);
        count = 64;
        break;
    case HashAlgorithm_Xxh64:
        count = HashBigEndian(bytes, XXH64_digest(&hasher->state.xxh64), 8);
        break;
    case HashAlgorithm_Xxh3:
        count = HashBigEndian(bytes, XXH3_64bits_digest(&hasher->state.xxh3), 8);
        break;
    case HashAlgorithm_Xxh128:
    {
        XXH128_hash_t hash = XXH3_128bits_digest(&hasher->state.xxh3);
        count = HashBigEndian(bytes, hash.high64, 8);
        count += HashBigEndian(bytes + 8, hash.low64, 8);
        break;
    }
    default:
        break;
    }

    hasher->finished = true;
    return HashHex(bytes, count);
}

static void HashHasherDestroy(void *object)
{
    HashHasher *hasher = object;

    // the SHAs and MD5 clear their state when they are freed
    if (!hasher->finished)
    {
        HashFinish(hasher);
    }

    SDL_aligned_free(hasher);
}

static const char *HashOf(const char *algorithm, SHUSlice data, const char **retError)
{
    *retError = nullptr;
    HashAlgorithm found = HashFindAlgorithm(algorithm);

    if (found == HashAlgorithm_Count)
    {
        *retError = "the algorithm is not one of crc32, md5, sha1, sha256, sha512, xxh64, xxh3 and xxh128";
        return nullptr;
    }

    HashHasher hasher;
    HashStart(&hasher, found);
    HashAdd(&hasher, data);
    return HashFinish(&hasher);
}

static HashHasher *HashNewHasher(const char *algorithm, const char **retError)
{
    *retError = nullptr;
    HashAlgorithm found = HashFindAlgorithm(algorithm);

    if (found == HashAlgorithm_Count)
    {
        *retError = "the algorithm is not one of crc32, md5, sha1, sha256, sha512, xxh64, xxh3 and xxh128";
        return nullptr;
    }

    HashHasher *hasher = SDL_aligned_alloc(64, sizeof(HashHasher));

    if (hasher == nullptr)
    {
        *retError = "out of memory";
        return nullptr;
    }

    HashStart(hasher, found);
    return hasher;
}

static bool HashUpdate(HashHasher *hasher, SHUSlice data)
{
    if (!hasher->finished)
    {
        HashAdd(hasher, data);
    }

    return !hasher->finished;
}

static const char *HashDigest(HashHasher *hasher)
{
    return hasher->finished ? nullptr : HashFinish(hasher);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "hasher", HashHasherDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"of", (ECSFunction)HashOf, "string(string algorithm, buffer data, out string error)",
         "Hash data whole\n"
         "@param algorithm crc32, md5, sha1, sha256, sha512, xxh64, xxh3 or xxh128\n"
         "@param data The data\n"
         "@param error Why it cannot hash; nothing if it hashed\n"
         "@return The hash as hexadecimal text, as tools such as sha256sum write it; nothing if the algorithm is unknown"},
        {"new", (ECSFunction)HashNewHasher, "handle<hash.hasher>(string algorithm, out string error)",
         "Start a hash that takes its data in parts, such as a large file's\n"
         "@param algorithm crc32, md5, sha1, sha256, sha512, xxh64, xxh3 or xxh128\n"
         "@param error Why it cannot start; nothing if it started\n"
         "@return The hasher; nothing if the algorithm is unknown"},
        {"update", (ECSFunction)HashUpdate, "bool(handle<hash.hasher> hasher, buffer data)",
         "Add the next part of the data\n"
         "@param hasher The hasher\n"
         "@param data The part\n"
         "@return false once the hasher gave its digest"},
        {"digest", (ECSFunction)HashDigest, "string(handle<hash.hasher> hasher)",
         "End the hash and give it; the hasher takes no more data\n"
         "@param hasher The hasher\n"
         "@return The hash as hexadecimal text; nothing if the hasher already gave it"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a handle, such as hasher:update(data); C calls the functions themselves (OpenECS DESIGN 10.6)
    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "hasher", "update", "update"));
    SHU_ReturnResult(ECSHandle_AddMethod(plugin, "hasher", "digest", "digest"));
    return SHUResult_Ok;
}
