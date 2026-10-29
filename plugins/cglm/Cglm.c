// The cglm standard plugin: cglm's vectors, 4 by 4 matrices, quaternions, cameras and noise, for Lua and C (DESIGN 16).
// Vectors, quaternions and matrices are struct types (OpenECS DESIGN 10.11): cglm.vec2, cglm.vec3, cglm.vec4 and cglm.quat have the fields x, y, z and w,
// and cglm.mat4 is a list of 16 floats, column by column, as cglm and shaders keep them. A function is named as cglm's, without glm_ and in camelCase:
// glm_mat4_mul is mat4Mul, and it gives what cglm writes into dest. C plugins call the same functions, through cglm.h; cglm itself stays inside the plugin.

#include "OpenECS.h"

#include "SDL3/SDL.h"

// the depth of cglm's projections goes from 0 to 1, as SDL's GPU API takes it
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#include "cglm/cglm.h"

#pragma region Source Only

// the struct types, laid out as their fields say; cglm's own types may need more alignment, so the functions copy them into cglm's types
typedef struct
{
    f32 x, y;
} CglmVec2;

typedef struct
{
    f32 x, y, z;
} CglmVec3;

typedef struct
{
    f32 x, y, z, w;
} CglmVec4;

typedef CglmVec4 CglmQuat;

typedef struct
{
    f32 values[16];
} CglmMat4;

/// @brief The plugin's state: the bytes that the bytes functions gave last, kept until the next call.
static struct
{
    f32 bytes[16];
} CGLM = {0};

// a struct's floats, copied into a local of cglm's type, and a local copied into a struct
#define CGLM_IN(type, name, from) \
    type name;                    \
    SDL_memcpy(name, from, sizeof(name))
#define CGLM_OUT(target, from) SDL_memcpy(target, from, sizeof(*(target)))

/// @brief Gives a struct's floats as bytes, kept until the next call.
static SHUSlice CglmBytes(const void *floats, usz count)
{
    SDL_memcpy(CGLM.bytes, floats, count * sizeof(f32));
    return cs(CGLM.bytes, count * sizeof(f32));
}

// the functions of a vector of N floats, which call cglm's glm_vecN_ functions
#define CGLM_VECTOR(N)                                                                                              \
    static void CglmVec##N##Add(const CglmVec##N *first, const CglmVec##N *second, CglmVec##N *retSum)              \
    {                                                                                                               \
        CGLM_IN(vec##N, one, first);                                                                                \
        CGLM_IN(vec##N, two, second);                                                                               \
        glm_vec##N##_add(one, two, one);                                                                            \
        CGLM_OUT(retSum, one);                                                                                      \
    }                                                                                                               \
    static void CglmVec##N##Sub(const CglmVec##N *first, const CglmVec##N *second, CglmVec##N *retDifference)       \
    {                                                                                                               \
        CGLM_IN(vec##N, one, first);                                                                                \
        CGLM_IN(vec##N, two, second);                                                                               \
        glm_vec##N##_sub(one, two, one);                                                                            \
        CGLM_OUT(retDifference, one);                                                                               \
    }                                                                                                               \
    static void CglmVec##N##Scale(const CglmVec##N *vector, f32 scale, CglmVec##N *retScaled)                       \
    {                                                                                                               \
        CGLM_IN(vec##N, one, vector);                                                                               \
        glm_vec##N##_scale(one, scale, one);                                                                        \
        CGLM_OUT(retScaled, one);                                                                                   \
    }                                                                                                               \
    static f32 CglmVec##N##Dot(const CglmVec##N *first, const CglmVec##N *second)                                   \
    {                                                                                                               \
        CGLM_IN(vec##N, one, first);                                                                                \
        CGLM_IN(vec##N, two, second);                                                                               \
        return glm_vec##N##_dot(one, two);                                                                          \
    }                                                                                                               \
    static f32 CglmVec##N##Norm(const CglmVec##N *vector)                                                           \
    {                                                                                                               \
        CGLM_IN(vec##N, one, vector);                                                                               \
        return glm_vec##N##_norm(one);                                                                              \
    }                                                                                                               \
    static f32 CglmVec##N##Distance(const CglmVec##N *first, const CglmVec##N *second)                              \
    {                                                                                                               \
        CGLM_IN(vec##N, one, first);                                                                                \
        CGLM_IN(vec##N, two, second);                                                                               \
        return glm_vec##N##_distance(one, two);                                                                     \
    }                                                                                                               \
    static void CglmVec##N##Normalize(const CglmVec##N *vector, CglmVec##N *retNormalized)                          \
    {                                                                                                               \
        CGLM_IN(vec##N, one, vector);                                                                               \
        vec##N result;                                                                                              \
        glm_vec##N##_normalize_to(one, result);                                                                     \
        CGLM_OUT(retNormalized, result);                                                                            \
    }                                                                                                               \
    static void CglmVec##N##Lerp(const CglmVec##N *from, const CglmVec##N *target, f32 amount, CglmVec##N *retAt)   \
    {                                                                                                               \
        /* cglm keeps t between 0 and 1 for vec2, but not for vec3 and vec4, so all sizes go as vec3 and vec4 do */ \
        const f32 *one = &from->x;                                                                                  \
        const f32 *two = &target->x;                                                                                \
        f32 *result = &retAt->x;                                                                                    \
        for (int i = 0; i < (N); i++)                                                                               \
        {                                                                                                           \
            result[i] = one[i] + (amount * (two[i] - one[i]));                                                      \
        }                                                                                                           \
    }                                                                                                               \
    static SHUSlice CglmVec##N##Bytes(const CglmVec##N *vector)                                                     \
    {                                                                                                               \
        return CglmBytes(vector, N);                                                                                \
    }

CGLM_VECTOR(2)
CGLM_VECTOR(3)
CGLM_VECTOR(4)

// the registration of a vector's functions, with their signatures
#define CGLM_VECTOR_FUNCTIONS(N)                                                                                                                                 \
    {"vec" #N "Add", (ECSFunction)CglmVec##N##Add, "void(struct<cglm.vec" #N "> a, struct<cglm.vec" #N "> b, out struct<cglm.vec" #N "> sum)",                   \
     "Add two vectors; a + b in Lua\n"                                                                                                                           \
     "@param a A vector\n"                                                                                                                                       \
     "@param b A vector\n"                                                                                                                                       \
     "@param sum The sum"},                                                                                                                                      \
        {"vec" #N "Sub", (ECSFunction)CglmVec##N##Sub, "void(struct<cglm.vec" #N "> a, struct<cglm.vec" #N "> b, out struct<cglm.vec" #N "> difference)",        \
         "Subtract b from a; a - b in Lua\n"                                                                                                                     \
         "@param a The vector to subtract from\n"                                                                                                                \
         "@param b The vector to subtract\n"                                                                                                                     \
         "@param difference The difference, a minus b"},                                                                                                         \
        {"vec" #N "Scale", (ECSFunction)CglmVec##N##Scale, "void(struct<cglm.vec" #N "> v, float s, out struct<cglm.vec" #N "> scaled)",                         \
         "Multiply a vector by a number; v * s in Lua\n"                                                                                                         \
         "@param v A vector\n"                                                                                                                                   \
         "@param s The number each float is multiplied by\n"                                                                                                     \
         "@param scaled The scaled vector"},                                                                                                                     \
        {"vec" #N "Dot", (ECSFunction)CglmVec##N##Dot, "float(struct<cglm.vec" #N "> a, struct<cglm.vec" #N "> b)",                                              \
         "Give the dot product of two vectors\n"                                                                                                                 \
         "@param a A vector\n"                                                                                                                                   \
         "@param b A vector\n"                                                                                                                                   \
         "@return The dot product"},                                                                                                                             \
        {"vec" #N "Norm", (ECSFunction)CglmVec##N##Norm, "float(struct<cglm.vec" #N "> v)",                                                                      \
         "Give the length of a vector\n"                                                                                                                         \
         "@param v A vector\n"                                                                                                                                   \
         "@return The length"},                                                                                                                                  \
        {"vec" #N "Distance", (ECSFunction)CglmVec##N##Distance, "float(struct<cglm.vec" #N "> a, struct<cglm.vec" #N "> b)",                                    \
         "Give the distance between two points\n"                                                                                                                \
         "@param a A point\n"                                                                                                                                    \
         "@param b A point\n"                                                                                                                                    \
         "@return The distance"},                                                                                                                                \
        {"vec" #N "Normalize", (ECSFunction)CglmVec##N##Normalize, "void(struct<cglm.vec" #N "> v, out struct<cglm.vec" #N "> normalized)",                      \
         "Give a vector the length 1, in its direction\n"                                                                                                        \
         "@param v A vector\n"                                                                                                                                   \
         "@param normalized The vector of length 1, all 0 if v's length is about 0"},                                                                            \
        {"vec" #N "Lerp", (ECSFunction)CglmVec##N##Lerp, "void(struct<cglm.vec" #N "> from, struct<cglm.vec" #N "> to, float t, out struct<cglm.vec" #N "> at)", \
         "Give the vector at t between from, at 0, and to, at 1\n"                                                                                               \
         "@param from The vector at 0\n"                                                                                                                         \
         "@param to The vector at 1\n"                                                                                                                           \
         "@param t Where between them: 0 gives from, 1 gives to, and t below 0 or above 1 goes past them\n"                                                      \
         "@param at The vector at t"},                                                                                                                           \
    {                                                                                                                                                            \
        "vec" #N "Bytes", (ECSFunction)CglmVec##N##Bytes, "buffer(struct<cglm.vec" #N "> v)",                                                                    \
            "Give a vector's floats as bytes, such as for a shader's uniforms; kept until the next call\n"                                                       \
            "@param v A vector\n"                                                                                                                                \
            "@return " #N " 32-bit floats in the system's byte order"                                                                                            \
    }

// the methods of a vector of N floats: an operator's name, such as add, is also the operator, such as a + b
#define CGLM_VECTOR_METHODS(N)                                                                                                                        \
    {"vec" #N, "add", "vec" #N "Add"}, {"vec" #N, "sub", "vec" #N "Sub"}, {"vec" #N, "mul", "vec" #N "Scale"}, {"vec" #N, "scale", "vec" #N "Scale"}, \
        {"vec" #N, "dot", "vec" #N "Dot"}, {"vec" #N, "norm", "vec" #N "Norm"}, {"vec" #N, "distance", "vec" #N "Distance"},                          \
        {"vec" #N, "normalize", "vec" #N "Normalize"}, {"vec" #N, "lerp", "vec" #N "Lerp"},                                                           \
    {                                                                                                                                                 \
        "vec" #N, "bytes", "vec" #N "Bytes"                                                                                                           \
    }

static void CglmVec2Make(f32 x, f32 y, CglmVec2 *retVector)
{
    *retVector = (CglmVec2){x, y};
}

static void CglmVec3Make(f32 x, f32 y, f32 z, CglmVec3 *retVector)
{
    *retVector = (CglmVec3){x, y, z};
}

static void CglmVec4Make(f32 x, f32 y, f32 z, f32 w, CglmVec4 *retVector)
{
    *retVector = (CglmVec4){x, y, z, w};
}

static void CglmVec3Cross(const CglmVec3 *first, const CglmVec3 *second, CglmVec3 *retCross)
{
    CGLM_IN(vec3, one, first);
    CGLM_IN(vec3, two, second);
    vec3 result;
    glm_vec3_cross(one, two, result);
    CGLM_OUT(retCross, result);
}

static void CglmMat4Identity(CglmMat4 *retIdentity)
{
    mat4 result;
    glm_mat4_identity(result);
    CGLM_OUT(retIdentity, result);
}

static void CglmMat4Mul(const CglmMat4 *first, const CglmMat4 *second, CglmMat4 *retProduct)
{
    CGLM_IN(mat4, one, first);
    CGLM_IN(mat4, two, second);
    mat4 result;
    glm_mat4_mul(one, two, result);
    CGLM_OUT(retProduct, result);
}

static void CglmMat4Mulv(const CglmMat4 *matrix, const CglmVec4 *vector, CglmVec4 *retProduct)
{
    CGLM_IN(mat4, mat, matrix);
    CGLM_IN(vec4, vec, vector);
    vec4 result;
    glm_mat4_mulv(mat, vec, result);
    CGLM_OUT(retProduct, result);
}

static void CglmMat4Mulv3(const CglmMat4 *matrix, const CglmVec3 *vector, f32 last, CglmVec3 *retProduct)
{
    CGLM_IN(mat4, mat, matrix);
    CGLM_IN(vec3, vec, vector);
    vec3 result;
    glm_mat4_mulv3(mat, vec, last, result);
    CGLM_OUT(retProduct, result);
}

static void CglmMat4Transpose(const CglmMat4 *matrix, CglmMat4 *retTransposed)
{
    CGLM_IN(mat4, mat, matrix);
    mat4 result;
    glm_mat4_transpose_to(mat, result);
    CGLM_OUT(retTransposed, result);
}

static f32 CglmMat4Det(const CglmMat4 *matrix)
{
    CGLM_IN(mat4, mat, matrix);
    return glm_mat4_det(mat);
}

// cglm inverts any matrix; one whose determinant is 0 has no inverse, so it gives zeros
static void CglmMat4Inv(const CglmMat4 *matrix, CglmMat4 *retInverse)
{
    CGLM_IN(mat4, mat, matrix);
    mat4 result = GLM_MAT4_ZERO_INIT;

    if (glm_mat4_det(mat) != 0.0f)
    {
        glm_mat4_inv(mat, result);
    }

    CGLM_OUT(retInverse, result);
}

static SHUSlice CglmMat4Bytes(const CglmMat4 *matrix)
{
    return CglmBytes(matrix->values, 16);
}

static void CglmTranslateMake(const CglmVec3 *vector, CglmMat4 *retMatrix)
{
    CGLM_IN(vec3, vec, vector);
    mat4 result;
    glm_translate_make(result, vec);
    CGLM_OUT(retMatrix, result);
}

static void CglmScaleMake(const CglmVec3 *vector, CglmMat4 *retMatrix)
{
    CGLM_IN(vec3, vec, vector);
    mat4 result;
    glm_scale_make(result, vec);
    CGLM_OUT(retMatrix, result);
}

static void CglmRotateMake(f32 angle, const CglmVec3 *axis, CglmMat4 *retMatrix)
{
    CGLM_IN(vec3, vec, axis);
    mat4 result;
    glm_rotate_make(result, angle, vec);
    CGLM_OUT(retMatrix, result);
}

static void CglmTranslate(const CglmMat4 *matrix, const CglmVec3 *vector, CglmMat4 *retMatrix)
{
    CGLM_IN(mat4, mat, matrix);
    CGLM_IN(vec3, vec, vector);
    glm_translate(mat, vec);
    CGLM_OUT(retMatrix, mat);
}

static void CglmRotate(const CglmMat4 *matrix, f32 angle, const CglmVec3 *axis, CglmMat4 *retMatrix)
{
    CGLM_IN(mat4, mat, matrix);
    CGLM_IN(vec3, vec, axis);
    glm_rotate(mat, angle, vec);
    CGLM_OUT(retMatrix, mat);
}

static void CglmScale(const CglmMat4 *matrix, const CglmVec3 *vector, CglmMat4 *retMatrix)
{
    CGLM_IN(mat4, mat, matrix);
    CGLM_IN(vec3, vec, vector);
    glm_scale(mat, vec);
    CGLM_OUT(retMatrix, mat);
}

static void CglmPerspective(f32 fovy, f32 aspect, f32 nearZ, f32 farZ, CglmMat4 *retProjection)
{
    mat4 result;
    glm_perspective(fovy, aspect, nearZ, farZ, result);
    CGLM_OUT(retProjection, result);
}

static void CglmOrtho(f32 left, f32 right, f32 bottom, f32 top, f32 nearZ, f32 farZ, CglmMat4 *retProjection)
{
    mat4 result;
    glm_ortho(left, right, bottom, top, nearZ, farZ, result);
    CGLM_OUT(retProjection, result);
}

static void CglmLookat(const CglmVec3 *eye, const CglmVec3 *center, const CglmVec3 *upDirection, CglmMat4 *retView)
{
    CGLM_IN(vec3, from, eye);
    CGLM_IN(vec3, target, center);
    CGLM_IN(vec3, upward, upDirection);
    mat4 result;
    glm_lookat(from, target, upward, result);
    CGLM_OUT(retView, result);
}

static void CglmProject(const CglmVec3 *position, const CglmMat4 *matrix, const CglmVec4 *viewport, CglmVec3 *retPoint)
{
    CGLM_IN(vec3, point, position);
    CGLM_IN(mat4, mat, matrix);
    CGLM_IN(vec4, rectangle, viewport);
    vec3 result;
    glm_project(point, mat, rectangle, result);
    CGLM_OUT(retPoint, result);
}

static void CglmQuatIdentity(CglmQuat *retIdentity)
{
    versor result;
    glm_quat_identity(result);
    CGLM_OUT(retIdentity, result);
}

static void CglmQuatMake(f32 angle, f32 x, f32 y, f32 z, CglmQuat *retQuat)
{
    versor result;
    glm_quat(result, angle, x, y, z);
    CGLM_OUT(retQuat, result);
}

static void CglmQuatv(f32 angle, const CglmVec3 *axis, CglmQuat *retQuat)
{
    CGLM_IN(vec3, vec, axis);
    versor result;
    glm_quatv(result, angle, vec);
    CGLM_OUT(retQuat, result);
}

static void CglmQuatMul(const CglmQuat *first, const CglmQuat *second, CglmQuat *retProduct)
{
    CGLM_IN(versor, one, first);
    CGLM_IN(versor, two, second);
    versor result;
    glm_quat_mul(one, two, result);
    CGLM_OUT(retProduct, result);
}

static void CglmQuatNormalize(const CglmQuat *rotation, CglmQuat *retNormalized)
{
    CGLM_IN(versor, rot, rotation);
    versor result;
    glm_quat_normalize_to(rot, result);
    CGLM_OUT(retNormalized, result);
}

static void CglmQuatSlerp(const CglmQuat *from, const CglmQuat *target, f32 amount, CglmQuat *retAt)
{
    CGLM_IN(versor, one, from);
    CGLM_IN(versor, two, target);
    versor result;
    glm_quat_slerp(one, two, amount, result);
    CGLM_OUT(retAt, result);
}

static void CglmQuatMat4(const CglmQuat *rotation, CglmMat4 *retMatrix)
{
    CGLM_IN(versor, rot, rotation);
    mat4 result;
    glm_quat_mat4(rot, result);
    CGLM_OUT(retMatrix, result);
}

static void CglmQuatRotatev(const CglmQuat *rotation, const CglmVec3 *vector, CglmVec3 *retTurned)
{
    CGLM_IN(versor, rot, rotation);
    CGLM_IN(vec3, vec, vector);
    vec3 result;
    glm_quat_rotatev(rot, vec, result);
    CGLM_OUT(retTurned, result);
}

static SHUSlice CglmQuatBytes(const CglmQuat *rotation)
{
    return CglmBytes(rotation, 4);
}

static f32 CglmPerlinVec2(const CglmVec2 *point)
{
    CGLM_IN(vec2, vec, point);
    return glm_perlin_vec2(vec);
}

static f32 CglmPerlinVec3(const CglmVec3 *point)
{
    CGLM_IN(vec3, vec, point);
    return glm_perlin_vec3(vec);
}

static f32 CglmPerlinVec4(const CglmVec4 *point)
{
    CGLM_IN(vec4, vec, point);
    return glm_perlin_vec4(vec);
}

/// @brief Registers the plugin's struct types, before the functions that name them.
static SHUResult CglmRegisterTypes(ECSPlugin plugin)
{
    SHU_ReturnResult(ECSStruct_Register(plugin, "vec2", "float x, float y"));
    SHU_ReturnResult(ECSStruct_Register(plugin, "vec3", "float x, float y, float z"));
    SHU_ReturnResult(ECSStruct_Register(plugin, "vec4", "float x, float y, float z, float w"));
    SHU_ReturnResult(ECSStruct_Register(plugin, "quat", "float x, float y, float z, float w"));
    // a matrix is its list of 16 floats, column by column, so m[13] is its move along y
    SHU_ReturnResult(ECSStruct_Register(plugin, "mat4", "float[16]"));
    return SHUResult_Ok;
}

/// @brief Makes the functions about a struct its methods, so Lua writes a + b, m1 * m2 and v:norm().
static SHUResult CglmAddMethods(ECSPlugin plugin)
{
    const char *methods[][3] = {
        CGLM_VECTOR_METHODS(2),
        CGLM_VECTOR_METHODS(3),
        CGLM_VECTOR_METHODS(4),
        {"vec3", "cross", "vec3Cross"},
        {"mat4", "mul", "mat4Mul"},
        {"mat4", "mulv", "mat4Mulv"},
        {"mat4", "mulv3", "mat4Mulv3"},
        {"mat4", "transpose", "mat4Transpose"},
        {"mat4", "det", "mat4Det"},
        {"mat4", "inv", "mat4Inv"},
        {"mat4", "translate", "translate"},
        {"mat4", "rotate", "rotate"},
        {"mat4", "scale", "scale"},
        {"mat4", "bytes", "mat4Bytes"},
        {"quat", "mul", "quatMul"},
        {"quat", "normalize", "quatNormalize"},
        {"quat", "slerp", "quatSlerp"},
        {"quat", "mat4", "quatMat4"},
        {"quat", "rotatev", "quatRotatev"},
        {"quat", "bytes", "quatBytes"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSStruct_AddMethod(plugin, methods[i][0], methods[i][1], methods[i][2]));
    }

    return SHUResult_Ok;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    const ECSPluginDesc desc = {0};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(CglmRegisterTypes(plugin));

    // a struct is never a result, so each function gives its vector, quaternion or matrix as an out parameter: Lua gets it as the result
    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"vec2", (ECSFunction)CglmVec2Make, "void(float x, float y, out struct<cglm.vec2> vector)",
         "Make a vector of 2 floats\n"
         "@param x The first float\n"
         "@param y The second float\n"
         "@param vector The vector"},
        {"vec3", (ECSFunction)CglmVec3Make, "void(float x, float y, float z, out struct<cglm.vec3> vector)",
         "Make a vector of 3 floats, such as a point or a direction\n"
         "@param x The first float\n"
         "@param y The second float\n"
         "@param z The third float\n"
         "@param vector The vector"},
        {"vec4", (ECSFunction)CglmVec4Make, "void(float x, float y, float z, float w, out struct<cglm.vec4> vector)",
         "Make a vector of 4 floats, such as a colour for a shader\n"
         "@param x The first float\n"
         "@param y The second float\n"
         "@param z The third float\n"
         "@param w The fourth float\n"
         "@param vector The vector"},
        CGLM_VECTOR_FUNCTIONS(2),
        CGLM_VECTOR_FUNCTIONS(3),
        CGLM_VECTOR_FUNCTIONS(4),
        {"vec3Cross", (ECSFunction)CglmVec3Cross, "void(struct<cglm.vec3> a, struct<cglm.vec3> b, out struct<cglm.vec3> cross)",
         "Give the cross product of two vectors\n"
         "@param a A vector\n"
         "@param b A vector\n"
         "@param cross The cross product of a and b"},
        {"mat4Identity", (ECSFunction)CglmMat4Identity, "void(out struct<cglm.mat4> identity)",
         "Give the matrix that changes nothing\n"
         "@param identity The identity matrix"},
        {"mat4Mul", (ECSFunction)CglmMat4Mul, "void(struct<cglm.mat4> m1, struct<cglm.mat4> m2, out struct<cglm.mat4> product)",
         "Multiply matrix m1 by matrix m2: the result does m2, then m1; m1 * m2 in Lua\n"
         "@param m1 A matrix\n"
         "@param m2 A matrix\n"
         "@param product The product"},
        {"mat4Mulv", (ECSFunction)CglmMat4Mulv, "void(struct<cglm.mat4> m, struct<cglm.vec4> v, out struct<cglm.vec4> product)",
         "Multiply a matrix by a vector of 4 floats\n"
         "@param m A matrix\n"
         "@param v A vector\n"
         "@param product The product"},
        {"mat4Mulv3", (ECSFunction)CglmMat4Mulv3, "void(struct<cglm.mat4> m, struct<cglm.vec3> v, float last, out struct<cglm.vec3> product)",
         "Multiply a matrix by a vector of 3 floats whose fourth is last, 1 for a point and 0 for a direction\n"
         "@param m A matrix\n"
         "@param v A vector\n"
         "@param last The vector's fourth float\n"
         "@param product The product's first 3 floats, not divided by its fourth"},
        {"mat4Transpose", (ECSFunction)CglmMat4Transpose, "void(struct<cglm.mat4> m, out struct<cglm.mat4> transposed)",
         "Swap a matrix's rows and columns\n"
         "@param m A matrix\n"
         "@param transposed The transposed matrix"},
        {"mat4Det", (ECSFunction)CglmMat4Det, "float(struct<cglm.mat4> m)",
         "Give a matrix's determinant\n"
         "@param m A matrix\n"
         "@return The determinant"},
        {"mat4Inv", (ECSFunction)CglmMat4Inv, "void(struct<cglm.mat4> m, out struct<cglm.mat4> inverse)",
         "Give the matrix that undoes a matrix\n"
         "@param m A matrix\n"
         "@param inverse The inverse, or all 0 if m's determinant is 0 and it has none"},
        {"mat4Bytes", (ECSFunction)CglmMat4Bytes, "buffer(struct<cglm.mat4> m)",
         "Give a matrix's floats as bytes, column by column, such as for a shader's uniforms; kept until the next call\n"
         "@param m A matrix\n"
         "@return 16 32-bit floats in the system's byte order"},
        {"translateMake", (ECSFunction)CglmTranslateMake, "void(struct<cglm.vec3> v, out struct<cglm.mat4> matrix)",
         "Give the matrix that moves by a vector\n"
         "@param v The move along x, y and z\n"
         "@param matrix The matrix"},
        {"scaleMake", (ECSFunction)CglmScaleMake, "void(struct<cglm.vec3> v, out struct<cglm.mat4> matrix)",
         "Give the matrix that scales by a vector\n"
         "@param v The scale along x, y and z\n"
         "@param matrix The matrix"},
        {"rotateMake", (ECSFunction)CglmRotateMake, "void(float angle, struct<cglm.vec3> axis, out struct<cglm.mat4> matrix)",
         "Give the matrix that turns by an angle in radians around an axis, counterclockwise as the axis points at the viewer\n"
         "@param angle The angle, in radians\n"
         "@param axis The axis, of any length but 0\n"
         "@param matrix The matrix"},
        {"translate", (ECSFunction)CglmTranslate, "void(struct<cglm.mat4> m, struct<cglm.vec3> v, out struct<cglm.mat4> matrix)",
         "Give a matrix that moves by a vector, then does m\n"
         "@param m The matrix to change\n"
         "@param v The move along x, y and z\n"
         "@param matrix The changed matrix"},
        {"rotate", (ECSFunction)CglmRotate, "void(struct<cglm.mat4> m, float angle, struct<cglm.vec3> axis, out struct<cglm.mat4> matrix)",
         "Give a matrix that turns by an angle around an axis, then does m\n"
         "@param m The matrix to change\n"
         "@param angle The angle, in radians, counterclockwise as the axis points at the viewer\n"
         "@param axis The axis, of any length but 0\n"
         "@param matrix The changed matrix"},
        {"scale", (ECSFunction)CglmScale, "void(struct<cglm.mat4> m, struct<cglm.vec3> v, out struct<cglm.mat4> matrix)",
         "Give a matrix that scales by a vector, then does m\n"
         "@param m The matrix to change\n"
         "@param v The scale along x, y and z\n"
         "@param matrix The changed matrix"},
        {"perspective", (ECSFunction)CglmPerspective, "void(float fovy, float aspect, float nearZ, float farZ, out struct<cglm.mat4> projection)",
         "Give a perspective that sees down -z, with a vertical field of view in radians and the width's ratio to the height; the depth goes from 0, at nearZ, to 1, at farZ, as gpu takes it\n"
         "@param fovy The vertical field of view, in radians\n"
         "@param aspect The width divided by the height\n"
         "@param nearZ The distance to the near plane, more than 0\n"
         "@param farZ The distance to the far plane, more than nearZ\n"
         "@param projection The projection matrix"},
        {"ortho", (ECSFunction)CglmOrtho, "void(float left, float right, float bottom, float top, float nearZ, float farZ, out struct<cglm.mat4> projection)",
         "Give a projection without perspective, of a box that sees down -z; the depth goes from 0 to 1\n"
         "@param left The box's left x\n"
         "@param right The box's right x\n"
         "@param bottom The box's bottom y\n"
         "@param top The box's top y\n"
         "@param nearZ The distance along -z to the box's near side, where the depth is 0\n"
         "@param farZ The distance along -z to the box's far side, where the depth is 1\n"
         "@param projection The projection matrix"},
        {"lookat", (ECSFunction)CglmLookat, "void(struct<cglm.vec3> eye, struct<cglm.vec3> center, struct<cglm.vec3> up, out struct<cglm.mat4> view)",
         "Give the matrix of a camera at eye that looks at center, with up as near its up as it can be\n"
         "@param eye The camera's position\n"
         "@param center The point it looks at\n"
         "@param up The direction that is up, not along the line from eye to center\n"
         "@param view The view matrix"},
        {"project", (ECSFunction)CglmProject, "void(struct<cglm.vec3> position, struct<cglm.mat4> m, struct<cglm.vec4> viewport, out struct<cglm.vec3> point)",
         "Give where a point is in a viewport, x, y, width and height, through a matrix such as a projection's: x and y in it, from its bottom left, and the depth\n"
         "@param position The point\n"
         "@param m The matrix, such as projection, view and model multiplied\n"
         "@param viewport The viewport: x, y, width and height\n"
         "@param point The point in the viewport: x, y and the depth, from 0 at near to 1 at far"},
        {"quatIdentity", (ECSFunction)CglmQuatIdentity, "void(out struct<cglm.quat> identity)",
         "Give the quaternion that turns nothing\n"
         "@param identity The quaternion"},
        {"quat", (ECSFunction)CglmQuatMake, "void(float angle, float x, float y, float z, out struct<cglm.quat> rotation)",
         "Give the quaternion that turns by an angle in radians around the axis x, y, z\n"
         "@param angle The angle, in radians\n"
         "@param x The axis's x; the axis can have any length but 0\n"
         "@param y The axis's y\n"
         "@param z The axis's z\n"
         "@param rotation The quaternion"},
        {"quatv", (ECSFunction)CglmQuatv, "void(float angle, struct<cglm.vec3> axis, out struct<cglm.quat> rotation)",
         "Give the quaternion that turns by an angle in radians around an axis\n"
         "@param angle The angle, in radians\n"
         "@param axis The axis, of any length but 0\n"
         "@param rotation The quaternion"},
        {"quatMul", (ECSFunction)CglmQuatMul, "void(struct<cglm.quat> p, struct<cglm.quat> q, out struct<cglm.quat> product)",
         "Multiply quaternion p by quaternion q: the result turns as q, then as p; p * q in Lua\n"
         "@param p A quaternion\n"
         "@param q A quaternion\n"
         "@param product The product"},
        {"quatNormalize", (ECSFunction)CglmQuatNormalize, "void(struct<cglm.quat> q, out struct<cglm.quat> normalized)",
         "Give a quaternion the length 1\n"
         "@param q A quaternion\n"
         "@param normalized The quaternion of length 1; the one that turns nothing if q's length is 0"},
        {"quatSlerp", (ECSFunction)CglmQuatSlerp, "void(struct<cglm.quat> from, struct<cglm.quat> to, float t, out struct<cglm.quat> at)",
         "Give the turn at t between quaternions from, at 0, and to, at 1, at an even speed\n"
         "@param from The quaternion at 0\n"
         "@param to The quaternion at 1\n"
         "@param t Where between them: 0 gives from, 1 gives to\n"
         "@param at The quaternion at t"},
        {"quatMat4", (ECSFunction)CglmQuatMat4, "void(struct<cglm.quat> q, out struct<cglm.mat4> matrix)",
         "Give the matrix that turns as a quaternion does\n"
         "@param q A quaternion of length 1\n"
         "@param matrix The matrix"},
        {"quatRotatev", (ECSFunction)CglmQuatRotatev, "void(struct<cglm.quat> q, struct<cglm.vec3> v, out struct<cglm.vec3> turned)",
         "Turn a vector by a quaternion\n"
         "@param q A quaternion, of any length but 0\n"
         "@param v The vector to turn\n"
         "@param turned The turned vector"},
        {"quatBytes", (ECSFunction)CglmQuatBytes, "buffer(struct<cglm.quat> q)",
         "Give a quaternion's floats as bytes, x, y, z and w, such as for a shader's uniforms; kept until the next call\n"
         "@param q A quaternion\n"
         "@return 4 32-bit floats in the system's byte order"},
        {"perlinVec2", (ECSFunction)CglmPerlinVec2, "float(struct<cglm.vec2> point)",
         "Give Perlin noise at a point, from about -1 to 1\n"
         "@param point The point\n"
         "@return The noise"},
        {"perlinVec3", (ECSFunction)CglmPerlinVec3, "float(struct<cglm.vec3> point)",
         "Give Perlin noise at a point, from about -1 to 1\n"
         "@param point The point\n"
         "@return The noise"},
        {"perlinVec4", (ECSFunction)CglmPerlinVec4, "float(struct<cglm.vec4> point)",
         "Give Perlin noise at a point, from about -1 to 1\n"
         "@param point The point\n"
         "@return The noise"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return CglmAddMethods(plugin);
}
