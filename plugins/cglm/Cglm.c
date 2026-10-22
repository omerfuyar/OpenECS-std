// The cglm standard plugin: cglm's vectors, 4 by 4 matrices, quaternions, cameras and noise, for Lua and C (DESIGN 16).
// Each vector, quaternion and matrix is a buffer of 32-bit floats, a string in Lua: 2, 3 or 4 for a vector, 4 for a quaternion, x, y, z and w,
// and 16 for a matrix, column by column, as cglm and shaders keep them. A function is named as cglm's, without glm_ and in camelCase:
// glm_mat4_mul is mat4Mul, and its result is what cglm writes into dest. C plugins call cglm itself, through header.h.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include "header.h"

#pragma region Source Only

/// @brief The plugin's state. The result of each function is kept here until the next call, as Lua copies it.
static struct
{
    ECSPlugin plugin;
    union
    {
        mat4 matrix;
        vec4 vector; // a vector or a quaternion
    } result; // aligned as cglm needs
} CGLM = {0};

/// @brief Copies a buffer of count floats into floats that are aligned as cglm needs; reports a buffer of another size, and gives false.
static bool CglmRead(f32 *retFloats, SHUSlice buffer, usz count, const char *function)
{
    if (buffer.data == nullptr || buffer.size != count * sizeof(f32))
    {
        ECS_LogWarn(CGLM.plugin, "cglm.%s takes buffers of %zu floats, not of %zu bytes", function, count, buffer.size);
        return false;
    }

    SDL_memcpy(retFloats, buffer.data, count * sizeof(f32));
    return true;
}

/// @brief Gives the first count floats of the result as a buffer.
static SHUSlice CglmResult(usz count)
{
    return cs(&CGLM.result.matrix, count * sizeof(f32));
}

// the functions of a vector of N floats, which call cglm's glm_vecN_ functions
#define CGLM_VECTOR(N)                                                                                                                                  \
    static SHUSlice CglmVec##N##Add(SHUSlice a, SHUSlice b)                                                                                             \
    {                                                                                                                                                   \
        vec##N first, second;                                                                                                                           \
        if (!CglmRead(first, a, N, "vec" #N "Add") || !CglmRead(second, b, N, "vec" #N "Add"))                                                          \
        {                                                                                                                                               \
            return cs(nullptr, 0);                                                                                                                      \
        }                                                                                                                                               \
        glm_vec##N##_add(first, second, CGLM.result.vector);                                                                                            \
        return CglmResult(N);                                                                                                                           \
    }                                                                                                                                                   \
    static SHUSlice CglmVec##N##Sub(SHUSlice a, SHUSlice b)                                                                                             \
    {                                                                                                                                                   \
        vec##N first, second;                                                                                                                           \
        if (!CglmRead(first, a, N, "vec" #N "Sub") || !CglmRead(second, b, N, "vec" #N "Sub"))                                                          \
        {                                                                                                                                               \
            return cs(nullptr, 0);                                                                                                                      \
        }                                                                                                                                               \
        glm_vec##N##_sub(first, second, CGLM.result.vector);                                                                                            \
        return CglmResult(N);                                                                                                                           \
    }                                                                                                                                                   \
    static SHUSlice CglmVec##N##Scale(SHUSlice v, f32 s)                                                                                                \
    {                                                                                                                                                   \
        vec##N vector;                                                                                                                                  \
        if (!CglmRead(vector, v, N, "vec" #N "Scale"))                                                                                                  \
        {                                                                                                                                               \
            return cs(nullptr, 0);                                                                                                                      \
        }                                                                                                                                               \
        glm_vec##N##_scale(vector, s, CGLM.result.vector);                                                                                              \
        return CglmResult(N);                                                                                                                           \
    }                                                                                                                                                   \
    static f32 CglmVec##N##Dot(SHUSlice a, SHUSlice b)                                                                                                  \
    {                                                                                                                                                   \
        vec##N first, second;                                                                                                                           \
        return CglmRead(first, a, N, "vec" #N "Dot") && CglmRead(second, b, N, "vec" #N "Dot") ? glm_vec##N##_dot(first, second) : 0.0f;                \
    }                                                                                                                                                   \
    static f32 CglmVec##N##Norm(SHUSlice v)                                                                                                             \
    {                                                                                                                                                   \
        vec##N vector;                                                                                                                                  \
        return CglmRead(vector, v, N, "vec" #N "Norm") ? glm_vec##N##_norm(vector) : 0.0f;                                                              \
    }                                                                                                                                                   \
    static f32 CglmVec##N##Distance(SHUSlice a, SHUSlice b)                                                                                             \
    {                                                                                                                                                   \
        vec##N first, second;                                                                                                                           \
        return CglmRead(first, a, N, "vec" #N "Distance") && CglmRead(second, b, N, "vec" #N "Distance") ? glm_vec##N##_distance(first, second) : 0.0f; \
    }                                                                                                                                                   \
    static SHUSlice CglmVec##N##Normalize(SHUSlice v)                                                                                                   \
    {                                                                                                                                                   \
        vec##N vector;                                                                                                                                  \
        if (!CglmRead(vector, v, N, "vec" #N "Normalize"))                                                                                              \
        {                                                                                                                                               \
            return cs(nullptr, 0);                                                                                                                      \
        }                                                                                                                                               \
        glm_vec##N##_normalize_to(vector, CGLM.result.vector);                                                                                          \
        return CglmResult(N);                                                                                                                           \
    }                                                                                                                                                   \
    static SHUSlice CglmVec##N##Lerp(SHUSlice from, SHUSlice to, f32 t)                                                                                 \
    {                                                                                                                                                   \
        vec##N first, second;                                                                                                                           \
        if (!CglmRead(first, from, N, "vec" #N "Lerp") || !CglmRead(second, to, N, "vec" #N "Lerp"))                                                    \
        {                                                                                                                                               \
            return cs(nullptr, 0);                                                                                                                      \
        }                                                                                                                                               \
        /* cglm keeps t between 0 and 1 for vec2, but not for vec3 and vec4, so all sizes go as vec3 and vec4 do */                                     \
        for (int i = 0; i < N; i++)                                                                                                                     \
        {                                                                                                                                               \
            CGLM.result.vector[i] = first[i] + t * (second[i] - first[i]);                                                                              \
        }                                                                                                                                               \
        return CglmResult(N);                                                                                                                           \
    }

CGLM_VECTOR(2)
CGLM_VECTOR(3)
CGLM_VECTOR(4)

// the registration of a vector's functions, with their signatures
#define CGLM_VECTOR_FUNCTIONS(N)                                                                          \
    {"vec" #N "Add", (ECSFunction)CglmVec##N##Add, "buffer(buffer a, buffer b)",                          \
     "Add two vectors of " #N " floats\n"                                                                 \
     "@param a A vector: " #N " floats\n"                                                                 \
     "@param b A vector: " #N " floats\n"                                                                 \
     "@return The sum: " #N " floats, or an empty buffer if a buffer has another size"},                  \
        {"vec" #N "Sub", (ECSFunction)CglmVec##N##Sub, "buffer(buffer a, buffer b)",                      \
         "Subtract b from a, vectors of " #N " floats\n"                                                  \
         "@param a The vector to subtract from: " #N " floats\n"                                          \
         "@param b The vector to subtract: " #N " floats\n"                                               \
         "@return The difference, a minus b: " #N " floats, or an empty buffer if a buffer has another size"},             \
        {"vec" #N "Scale", (ECSFunction)CglmVec##N##Scale, "buffer(buffer v, float s)",                   \
         "Multiply a vector of " #N " floats by a number\n"                                               \
         "@param v A vector: " #N " floats\n"                                                             \
         "@param s The number each float is multiplied by\n"                                              \
         "@return The scaled vector: " #N " floats, or an empty buffer if v has another size"},            \
        {"vec" #N "Dot", (ECSFunction)CglmVec##N##Dot, "float(buffer a, buffer b)",                       \
         "Give the dot product of two vectors of " #N " floats\n"                                         \
         "@param a A vector: " #N " floats\n"                                                             \
         "@param b A vector: " #N " floats\n"                                                             \
         "@return The dot product, or 0 if a buffer has another size"},                                    \
        {"vec" #N "Norm", (ECSFunction)CglmVec##N##Norm, "float(buffer v)",                               \
         "Give the length of a vector of " #N " floats\n"                                                 \
         "@param v A vector: " #N " floats\n"                                                             \
         "@return The length, or 0 if v has another size"},                                                \
        {"vec" #N "Distance", (ECSFunction)CglmVec##N##Distance, "float(buffer a, buffer b)",             \
         "Give the distance between two points of " #N " floats\n"                                        \
         "@param a A point: " #N " floats\n"                                                              \
         "@param b A point: " #N " floats\n"                                                              \
         "@return The distance, or 0 if a buffer has another size"},                                       \
        {"vec" #N "Normalize", (ECSFunction)CglmVec##N##Normalize, "buffer(buffer v)",                    \
         "Give a vector of " #N " floats the length 1, in its direction\n"                                \
         "@param v A vector: " #N " floats\n"                                                             \
         "@return The vector: " #N " floats, all 0 if v's length is about 0, or an empty buffer if v has another size"}, \
        {"vec" #N "Lerp", (ECSFunction)CglmVec##N##Lerp, "buffer(buffer from, buffer to, float t)",       \
         "Give the vector at t between from, at 0, and to, at 1\n"                                        \
         "@param from The vector at 0: " #N " floats\n"                                                   \
         "@param to The vector at 1: " #N " floats\n"                                                     \
         "@param t Where between them: 0 gives from, 1 gives to, and t below 0 or above 1 goes past them\n" \
         "@return The vector at t: " #N " floats, or an empty buffer if a buffer has another size"}

static SHUSlice CglmVec2(f32 x, f32 y)
{
    glm_vec2_copy((vec2){x, y}, CGLM.result.vector);
    return CglmResult(2);
}

static SHUSlice CglmVec3(f32 x, f32 y, f32 z)
{
    glm_vec3_copy((vec3){x, y, z}, CGLM.result.vector);
    return CglmResult(3);
}

static SHUSlice CglmVec4(f32 x, f32 y, f32 z, f32 w)
{
    glm_vec4_copy((vec4){x, y, z, w}, CGLM.result.vector);
    return CglmResult(4);
}

static void CglmComponents(SHUSlice v, f32 *retX, f32 *retY, f32 *retZ, f32 *retW)
{
    f32 components[4] = {0};

    if (v.data != nullptr && v.size % sizeof(f32) == 0 && v.size <= sizeof(components))
    {
        SDL_memcpy(components, v.data, v.size);
    }
    else
    {
        ECS_LogWarn(CGLM.plugin, "cglm.components takes a buffer of up to 4 floats, not of %zu bytes", v.size);
    }

    *retX = components[0];
    *retY = components[1];
    *retZ = components[2];
    *retW = components[3];
}

static SHUSlice CglmVec3Cross(SHUSlice a, SHUSlice b)
{
    vec3 first, second;

    if (!CglmRead(first, a, 3, "vec3Cross") || !CglmRead(second, b, 3, "vec3Cross"))
    {
        return cs(nullptr, 0);
    }

    glm_vec3_cross(first, second, CGLM.result.vector);
    return CglmResult(3);
}

static SHUSlice CglmMat4Identity(void)
{
    glm_mat4_identity(CGLM.result.matrix);
    return CglmResult(16);
}

static SHUSlice CglmMat4Mul(SHUSlice a, SHUSlice b)
{
    mat4 first, second;

    if (!CglmRead((f32 *)first, a, 16, "mat4Mul") || !CglmRead((f32 *)second, b, 16, "mat4Mul"))
    {
        return cs(nullptr, 0);
    }

    glm_mat4_mul(first, second, CGLM.result.matrix);
    return CglmResult(16);
}

static SHUSlice CglmMat4Mulv(SHUSlice m, SHUSlice v)
{
    mat4 matrix;
    vec4 vector;

    if (!CglmRead((f32 *)matrix, m, 16, "mat4Mulv") || !CglmRead(vector, v, 4, "mat4Mulv"))
    {
        return cs(nullptr, 0);
    }

    glm_mat4_mulv(matrix, vector, CGLM.result.vector);
    return CglmResult(4);
}

static SHUSlice CglmMat4Mulv3(SHUSlice m, SHUSlice v, f32 last)
{
    mat4 matrix;
    vec3 vector;

    if (!CglmRead((f32 *)matrix, m, 16, "mat4Mulv3") || !CglmRead(vector, v, 3, "mat4Mulv3"))
    {
        return cs(nullptr, 0);
    }

    glm_mat4_mulv3(matrix, vector, last, CGLM.result.vector);
    return CglmResult(3);
}

static SHUSlice CglmMat4Transpose(SHUSlice m)
{
    mat4 matrix;

    if (!CglmRead((f32 *)matrix, m, 16, "mat4Transpose"))
    {
        return cs(nullptr, 0);
    }

    glm_mat4_transpose_to(matrix, CGLM.result.matrix);
    return CglmResult(16);
}

static f32 CglmMat4Det(SHUSlice m)
{
    mat4 matrix;
    return CglmRead((f32 *)matrix, m, 16, "mat4Det") ? glm_mat4_det(matrix) : 0.0f;
}

// cglm inverts any matrix; one whose determinant is 0 has no inverse, so it gives an empty buffer
static SHUSlice CglmMat4Inv(SHUSlice m)
{
    mat4 matrix;

    if (!CglmRead((f32 *)matrix, m, 16, "mat4Inv") || glm_mat4_det(matrix) == 0.0f)
    {
        return cs(nullptr, 0);
    }

    glm_mat4_inv(matrix, CGLM.result.matrix);
    return CglmResult(16);
}

static SHUSlice CglmTranslateMake(SHUSlice v)
{
    vec3 vector;

    if (!CglmRead(vector, v, 3, "translateMake"))
    {
        return cs(nullptr, 0);
    }

    glm_translate_make(CGLM.result.matrix, vector);
    return CglmResult(16);
}

static SHUSlice CglmScaleMake(SHUSlice v)
{
    vec3 vector;

    if (!CglmRead(vector, v, 3, "scaleMake"))
    {
        return cs(nullptr, 0);
    }

    glm_scale_make(CGLM.result.matrix, vector);
    return CglmResult(16);
}

static SHUSlice CglmRotateMake(f32 angle, SHUSlice axis)
{
    vec3 vector;

    if (!CglmRead(vector, axis, 3, "rotateMake"))
    {
        return cs(nullptr, 0);
    }

    glm_rotate_make(CGLM.result.matrix, angle, vector);
    return CglmResult(16);
}

static SHUSlice CglmTranslate(SHUSlice m, SHUSlice v)
{
    vec3 vector;

    if (!CglmRead((f32 *)CGLM.result.matrix, m, 16, "translate") || !CglmRead(vector, v, 3, "translate"))
    {
        return cs(nullptr, 0);
    }

    glm_translate(CGLM.result.matrix, vector);
    return CglmResult(16);
}

static SHUSlice CglmRotate(SHUSlice m, f32 angle, SHUSlice axis)
{
    vec3 vector;

    if (!CglmRead((f32 *)CGLM.result.matrix, m, 16, "rotate") || !CglmRead(vector, axis, 3, "rotate"))
    {
        return cs(nullptr, 0);
    }

    glm_rotate(CGLM.result.matrix, angle, vector);
    return CglmResult(16);
}

static SHUSlice CglmScale(SHUSlice m, SHUSlice v)
{
    vec3 vector;

    if (!CglmRead((f32 *)CGLM.result.matrix, m, 16, "scale") || !CglmRead(vector, v, 3, "scale"))
    {
        return cs(nullptr, 0);
    }

    glm_scale(CGLM.result.matrix, vector);
    return CglmResult(16);
}

static SHUSlice CglmPerspective(f32 fovy, f32 aspect, f32 nearZ, f32 farZ)
{
    glm_perspective(fovy, aspect, nearZ, farZ, CGLM.result.matrix);
    return CglmResult(16);
}

static SHUSlice CglmOrtho(f32 left, f32 right, f32 bottom, f32 top, f32 nearZ, f32 farZ)
{
    glm_ortho(left, right, bottom, top, nearZ, farZ, CGLM.result.matrix);
    return CglmResult(16);
}

static SHUSlice CglmLookat(SHUSlice eye, SHUSlice center, SHUSlice up)
{
    vec3 eyePoint, centerPoint, upDirection;

    if (!CglmRead(eyePoint, eye, 3, "lookat") || !CglmRead(centerPoint, center, 3, "lookat") || !CglmRead(upDirection, up, 3, "lookat"))
    {
        return cs(nullptr, 0);
    }

    glm_lookat(eyePoint, centerPoint, upDirection, CGLM.result.matrix);
    return CglmResult(16);
}

static SHUSlice CglmProject(SHUSlice position, SHUSlice m, SHUSlice viewport)
{
    vec3 point;
    mat4 matrix;
    vec4 rectangle;

    if (!CglmRead(point, position, 3, "project") || !CglmRead((f32 *)matrix, m, 16, "project") || !CglmRead(rectangle, viewport, 4, "project"))
    {
        return cs(nullptr, 0);
    }

    glm_project(point, matrix, rectangle, CGLM.result.vector);
    return CglmResult(3);
}

static SHUSlice CglmQuatIdentity(void)
{
    glm_quat_identity(CGLM.result.vector);
    return CglmResult(4);
}

static SHUSlice CglmQuat(f32 angle, f32 x, f32 y, f32 z)
{
    glm_quat(CGLM.result.vector, angle, x, y, z);
    return CglmResult(4);
}

static SHUSlice CglmQuatv(f32 angle, SHUSlice axis)
{
    vec3 vector;

    if (!CglmRead(vector, axis, 3, "quatv"))
    {
        return cs(nullptr, 0);
    }

    glm_quatv(CGLM.result.vector, angle, vector);
    return CglmResult(4);
}

static SHUSlice CglmQuatMul(SHUSlice p, SHUSlice q)
{
    versor first, second;

    if (!CglmRead(first, p, 4, "quatMul") || !CglmRead(second, q, 4, "quatMul"))
    {
        return cs(nullptr, 0);
    }

    glm_quat_mul(first, second, CGLM.result.vector);
    return CglmResult(4);
}

static SHUSlice CglmQuatNormalize(SHUSlice q)
{
    versor rotation;

    if (!CglmRead(rotation, q, 4, "quatNormalize"))
    {
        return cs(nullptr, 0);
    }

    glm_quat_normalize_to(rotation, CGLM.result.vector);
    return CglmResult(4);
}

static SHUSlice CglmQuatSlerp(SHUSlice from, SHUSlice to, f32 t)
{
    versor first, second;

    if (!CglmRead(first, from, 4, "quatSlerp") || !CglmRead(second, to, 4, "quatSlerp"))
    {
        return cs(nullptr, 0);
    }

    glm_quat_slerp(first, second, t, CGLM.result.vector);
    return CglmResult(4);
}

static SHUSlice CglmQuatMat4(SHUSlice q)
{
    versor rotation;

    if (!CglmRead(rotation, q, 4, "quatMat4"))
    {
        return cs(nullptr, 0);
    }

    glm_quat_mat4(rotation, CGLM.result.matrix);
    return CglmResult(16);
}

static SHUSlice CglmQuatRotatev(SHUSlice q, SHUSlice v)
{
    versor rotation;
    vec3 vector;

    if (!CglmRead(rotation, q, 4, "quatRotatev") || !CglmRead(vector, v, 3, "quatRotatev"))
    {
        return cs(nullptr, 0);
    }

    glm_quat_rotatev(rotation, vector, CGLM.result.vector);
    return CglmResult(3);
}

static f32 CglmPerlinVec2(SHUSlice point)
{
    vec2 vector;
    return CglmRead(vector, point, 2, "perlinVec2") ? glm_perlin_vec2(vector) : 0.0f;
}

static f32 CglmPerlinVec3(SHUSlice point)
{
    vec3 vector;
    return CglmRead(vector, point, 3, "perlinVec3") ? glm_perlin_vec3(vector) : 0.0f;
}

static f32 CglmPerlinVec4(SHUSlice point)
{
    vec4 vector;
    return CglmRead(vector, point, 4, "perlinVec4") ? glm_perlin_vec4(vector) : 0.0f;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    CGLM.plugin = plugin;
    const ECSPluginDesc desc = {0};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));

    // each result is a buffer of floats, kept until the next call; C plugins call cglm's own functions instead
    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"vec2", (ECSFunction)CglmVec2, "buffer(float x, float y)",
         "Make a vector of 2 floats\n"
         "@param x The first float\n"
         "@param y The second float\n"
         "@return The vector: 2 floats"},
        {"vec3", (ECSFunction)CglmVec3, "buffer(float x, float y, float z)",
         "Make a vector of 3 floats, such as a point or a direction\n"
         "@param x The first float\n"
         "@param y The second float\n"
         "@param z The third float\n"
         "@return The vector: 3 floats"},
        {"vec4", (ECSFunction)CglmVec4, "buffer(float x, float y, float z, float w)",
         "Make a vector of 4 floats, such as a colour for a shader\n"
         "@param x The first float\n"
         "@param y The second float\n"
         "@param z The third float\n"
         "@param w The fourth float\n"
         "@return The vector: 4 floats"},
        {"components", (ECSFunction)CglmComponents, "void(buffer v, out float x, out float y, out float z, out float w)",
         "Give the floats of a vector or quaternion; those it lacks are 0\n"
         "@param v A vector or quaternion: up to 4 floats; with more, or not whole floats, all four are 0\n"
         "@param x The first float\n"
         "@param y The second float\n"
         "@param z The third float\n"
         "@param w The fourth float"},
        CGLM_VECTOR_FUNCTIONS(2),
        CGLM_VECTOR_FUNCTIONS(3),
        CGLM_VECTOR_FUNCTIONS(4),
        {"vec3Cross", (ECSFunction)CglmVec3Cross, "buffer(buffer a, buffer b)",
         "Give the cross product of two vectors of 3 floats\n"
         "@param a A vector: 3 floats\n"
         "@param b A vector: 3 floats\n"
         "@return The cross product of a and b: 3 floats, or an empty buffer if a buffer has another size"},
        {"mat4Identity", (ECSFunction)CglmMat4Identity, "buffer()",
         "Give the matrix that changes nothing\n"
         "@return The identity matrix: 16 floats, column by column"},
        {"mat4Mul", (ECSFunction)CglmMat4Mul, "buffer(buffer m1, buffer m2)",
         "Multiply matrix m1 by matrix m2: the result does m2, then m1\n"
         "@param m1 A matrix: 16 floats, column by column\n"
         "@param m2 A matrix: 16 floats, column by column\n"
         "@return The product: 16 floats, column by column, or an empty buffer if a buffer has another size"},
        {"mat4Mulv", (ECSFunction)CglmMat4Mulv, "buffer(buffer m, buffer v)",
         "Multiply a matrix by a vector of 4 floats\n"
         "@param m A matrix: 16 floats, column by column\n"
         "@param v A vector: 4 floats\n"
         "@return The product: 4 floats, or an empty buffer if a buffer has another size"},
        {"mat4Mulv3", (ECSFunction)CglmMat4Mulv3, "buffer(buffer m, buffer v, float last)",
         "Multiply a matrix by a vector of 3 floats whose fourth is last, 1 for a point and 0 for a direction\n"
         "@param m A matrix: 16 floats, column by column\n"
         "@param v A vector: 3 floats\n"
         "@param last The vector's fourth float\n"
         "@return The product's first 3 floats, not divided by its fourth, or an empty buffer if a buffer has another size"},
        {"mat4Transpose", (ECSFunction)CglmMat4Transpose, "buffer(buffer m)",
         "Swap a matrix's rows and columns\n"
         "@param m A matrix: 16 floats, column by column\n"
         "@return The transposed matrix: 16 floats, column by column, or an empty buffer if m has another size"},
        {"mat4Det", (ECSFunction)CglmMat4Det, "float(buffer m)",
         "Give a matrix's determinant\n"
         "@param m A matrix: 16 floats, column by column\n"
         "@return The determinant, or 0 if m has another size"},
        {"mat4Inv", (ECSFunction)CglmMat4Inv, "buffer(buffer m)",
         "Give the matrix that undoes a matrix; an empty buffer if it has none\n"
         "@param m A matrix: 16 floats, column by column\n"
         "@return The inverse: 16 floats, column by column, or an empty buffer if m's determinant is 0 or m has another size"},
        {"translateMake", (ECSFunction)CglmTranslateMake, "buffer(buffer v)",
         "Give the matrix that moves by a vector of 3 floats\n"
         "@param v The move along x, y and z: 3 floats\n"
         "@return The matrix: 16 floats, column by column, or an empty buffer if v has another size"},
        {"scaleMake", (ECSFunction)CglmScaleMake, "buffer(buffer v)",
         "Give the matrix that scales by a vector of 3 floats\n"
         "@param v The scale along x, y and z: 3 floats\n"
         "@return The matrix: 16 floats, column by column, or an empty buffer if v has another size"},
        {"rotateMake", (ECSFunction)CglmRotateMake, "buffer(float angle, buffer axis)",
         "Give the matrix that turns by an angle in radians around an axis, counterclockwise as the axis points at the viewer\n"
         "@param angle The angle, in radians\n"
         "@param axis The axis: 3 floats, of any length but 0\n"
         "@return The matrix: 16 floats, column by column, or an empty buffer if axis has another size"},
        {"translate", (ECSFunction)CglmTranslate, "buffer(buffer m, buffer v)",
         "Give a matrix that moves by a vector of 3 floats, then does m\n"
         "@param m The matrix to change: 16 floats, column by column\n"
         "@param v The move along x, y and z: 3 floats\n"
         "@return The matrix: 16 floats, column by column, or an empty buffer if a buffer has another size"},
        {"rotate", (ECSFunction)CglmRotate, "buffer(buffer m, float angle, buffer axis)",
         "Give a matrix that turns by an angle around an axis, then does m\n"
         "@param m The matrix to change: 16 floats, column by column\n"
         "@param angle The angle, in radians, counterclockwise as the axis points at the viewer\n"
         "@param axis The axis: 3 floats, of any length but 0\n"
         "@return The matrix: 16 floats, column by column, or an empty buffer if a buffer has another size"},
        {"scale", (ECSFunction)CglmScale, "buffer(buffer m, buffer v)",
         "Give a matrix that scales by a vector of 3 floats, then does m\n"
         "@param m The matrix to change: 16 floats, column by column\n"
         "@param v The scale along x, y and z: 3 floats\n"
         "@return The matrix: 16 floats, column by column, or an empty buffer if a buffer has another size"},
        {"perspective", (ECSFunction)CglmPerspective, "buffer(float fovy, float aspect, float nearZ, float farZ)",
         "Give a perspective that sees down -z, with a vertical field of view in radians and the width's ratio to the height; the depth goes from 0, at nearZ, to 1, at farZ, as gpu takes it\n"
         "@param fovy The vertical field of view, in radians\n"
         "@param aspect The width divided by the height\n"
         "@param nearZ The distance to the near plane, more than 0\n"
         "@param farZ The distance to the far plane, more than nearZ\n"
         "@return The projection matrix: 16 floats, column by column"},
        {"ortho", (ECSFunction)CglmOrtho, "buffer(float left, float right, float bottom, float top, float nearZ, float farZ)",
         "Give a projection without perspective, of a box that sees down -z; the depth goes from 0 to 1\n"
         "@param left The box's left x\n"
         "@param right The box's right x\n"
         "@param bottom The box's bottom y\n"
         "@param top The box's top y\n"
         "@param nearZ The distance along -z to the box's near side, where the depth is 0\n"
         "@param farZ The distance along -z to the box's far side, where the depth is 1\n"
         "@return The projection matrix: 16 floats, column by column"},
        {"lookat", (ECSFunction)CglmLookat, "buffer(buffer eye, buffer center, buffer up)",
         "Give the matrix of a camera at eye that looks at center, with up as near its up as it can be\n"
         "@param eye The camera's position: 3 floats\n"
         "@param center The point it looks at: 3 floats\n"
         "@param up The direction that is up: 3 floats, not along the line from eye to center\n"
         "@return The view matrix: 16 floats, column by column, or an empty buffer if a buffer has another size"},
        {"project", (ECSFunction)CglmProject, "buffer(buffer position, buffer m, buffer viewport)",
         "Give where a point is in a viewport, x, y, width and height, through a matrix such as a projection's: x and y in it, from its bottom left, and the depth\n"
         "@param position The point: 3 floats\n"
         "@param m The matrix, such as projection, view and model multiplied: 16 floats, column by column\n"
         "@param viewport The viewport: 4 floats, x, y, width and height\n"
         "@return The point in the viewport: 3 floats, x, y and the depth, from 0 at near to 1 at far; an empty buffer if a buffer has another size"},
        {"quatIdentity", (ECSFunction)CglmQuatIdentity, "buffer()",
         "Give the quaternion that turns nothing\n"
         "@return The quaternion: 4 floats, x, y, z and w"},
        {"quat", (ECSFunction)CglmQuat, "buffer(float angle, float x, float y, float z)",
         "Give the quaternion that turns by an angle in radians around the axis x, y, z\n"
         "@param angle The angle, in radians\n"
         "@param x The axis's x; the axis can have any length but 0\n"
         "@param y The axis's y\n"
         "@param z The axis's z\n"
         "@return The quaternion: 4 floats, x, y, z and w"},
        {"quatv", (ECSFunction)CglmQuatv, "buffer(float angle, buffer axis)",
         "Give the quaternion that turns by an angle in radians around an axis of 3 floats\n"
         "@param angle The angle, in radians\n"
         "@param axis The axis: 3 floats, of any length but 0\n"
         "@return The quaternion: 4 floats, x, y, z and w, or an empty buffer if axis has another size"},
        {"quatMul", (ECSFunction)CglmQuatMul, "buffer(buffer p, buffer q)",
         "Multiply quaternion p by quaternion q: the result turns as q, then as p\n"
         "@param p A quaternion: 4 floats, x, y, z and w\n"
         "@param q A quaternion: 4 floats, x, y, z and w\n"
         "@return The product: 4 floats, x, y, z and w, or an empty buffer if a buffer has another size"},
        {"quatNormalize", (ECSFunction)CglmQuatNormalize, "buffer(buffer q)",
         "Give a quaternion the length 1\n"
         "@param q A quaternion: 4 floats, x, y, z and w\n"
         "@return The quaternion of length 1: 4 floats; the one that turns nothing if q's length is 0; an empty buffer if q has another size"},
        {"quatSlerp", (ECSFunction)CglmQuatSlerp, "buffer(buffer from, buffer to, float t)",
         "Give the turn at t between quaternions from, at 0, and to, at 1, at an even speed\n"
         "@param from The quaternion at 0: 4 floats, x, y, z and w\n"
         "@param to The quaternion at 1: 4 floats, x, y, z and w\n"
         "@param t Where between them: 0 gives from, 1 gives to\n"
         "@return The quaternion at t: 4 floats, x, y, z and w, or an empty buffer if a buffer has another size"},
        {"quatMat4", (ECSFunction)CglmQuatMat4, "buffer(buffer q)",
         "Give the matrix that turns as a quaternion does\n"
         "@param q A quaternion of length 1: 4 floats, x, y, z and w\n"
         "@return The matrix: 16 floats, column by column, or an empty buffer if q has another size"},
        {"quatRotatev", (ECSFunction)CglmQuatRotatev, "buffer(buffer q, buffer v)",
         "Turn a vector of 3 floats by a quaternion\n"
         "@param q A quaternion: 4 floats, x, y, z and w, of any length but 0\n"
         "@param v The vector to turn: 3 floats\n"
         "@return The turned vector: 3 floats, or an empty buffer if a buffer has another size"},
        {"perlinVec2", (ECSFunction)CglmPerlinVec2, "float(buffer point)",
         "Give Perlin noise at a point of 2 floats, from about -1 to 1\n"
         "@param point The point: 2 floats\n"
         "@return The noise, or 0 if point has another size"},
        {"perlinVec3", (ECSFunction)CglmPerlinVec3, "float(buffer point)",
         "Give Perlin noise at a point of 3 floats, from about -1 to 1\n"
         "@param point The point: 3 floats\n"
         "@return The noise, or 0 if point has another size"},
        {"perlinVec4", (ECSFunction)CglmPerlinVec4, "float(buffer point)",
         "Give Perlin noise at a point of 4 floats, from about -1 to 1\n"
         "@param point The point: 4 floats\n"
         "@return The noise, or 0 if point has another size"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
