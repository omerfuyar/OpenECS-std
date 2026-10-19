// The linear standard plugin: vectors, 4 by 4 matrices and quaternions of floats (DESIGN 16).
// Each is a buffer of 32-bit floats: a vector has 2, 3 or 4, a quaternion 4 and a matrix 16, column by column, so a matrix goes into a shader's
// uniforms as it is. The math is in header.h, whose inline functions C plugins call directly; these functions give it to Lua.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include "header.h"

#pragma region Source Only

/// @brief The plugin's state. The result of each function is kept here until the next call, as Lua copies it.
static struct
{
    ECSPlugin plugin;
    f32 result[16];
} LINEAR = {0};

/// @brief How many floats a buffer holds, if it is of floats and holds from least to most of them; 0, with a warning, if not.
static usz LinearCount(SHUSlice buffer, usz least, usz most, const char *function)
{
    usz count = buffer.size / sizeof(f32);

    if (buffer.data == nullptr || buffer.size % sizeof(f32) != 0 || count < least || count > most)
    {
        ECS_LogWarn(LINEAR.plugin, "linear.%s takes buffers of %zu to %zu floats, not of %zu bytes", function, least, most, buffer.size);
        return 0;
    }

    return count;
}

/// @brief Copies a buffer's floats, as a buffer from C may not be aligned for them.
static void LinearRead(f32 *out, SHUSlice buffer, usz count)
{
    if (count > 0)
    {
        SDL_memcpy(out, buffer.data, count * sizeof(f32));
    }
}

/// @brief Gives the first count floats of the result as a buffer.
static SHUSlice LinearResult(usz count)
{
    return cs(LINEAR.result, count * sizeof(f32));
}

/// @brief Gives an empty buffer, for misuse that was reported.
static SHUSlice LinearNone(void)
{
    return cs(nullptr, 0);
}

/// @brief Reads two buffers of the same count of floats, from least to most, for a function; gives the count, or 0 if they do not fit.
static usz LinearReadPair(f32 *a, f32 *b, SHUSlice first, SHUSlice second, usz least, usz most, const char *function)
{
    usz count = LinearCount(first, least, most, function);

    if (count == 0 || LinearCount(second, least, most, function) != count)
    {
        if (count != 0)
        {
            ECS_LogWarn(LINEAR.plugin, "linear.%s takes two buffers of as many floats", function);
        }

        return 0;
    }

    LinearRead(a, first, count);
    LinearRead(b, second, count);
    return count;
}

static SHUSlice LinearVec2(f32 x, f32 y)
{
    LINEAR.result[0] = x;
    LINEAR.result[1] = y;
    return LinearResult(2);
}

static SHUSlice LinearVec3(f32 x, f32 y, f32 z)
{
    LINEAR.result[0] = x;
    LINEAR.result[1] = y;
    LINEAR.result[2] = z;
    return LinearResult(3);
}

static SHUSlice LinearVec4(f32 x, f32 y, f32 z, f32 w)
{
    LINEAR.result[0] = x;
    LINEAR.result[1] = y;
    LINEAR.result[2] = z;
    LINEAR.result[3] = w;
    return LinearResult(4);
}

static void LinearComponents(SHUSlice v, f32 *x, f32 *y, f32 *z, f32 *w)
{
    f32 in[4] = {0};
    usz count = LinearCount(v, 1, 4, "components");
    LinearRead(in, v, count);
    *x = in[0];
    *y = in[1];
    *z = in[2];
    *w = in[3];
}

static SHUSlice LinearAdd(SHUSlice a, SHUSlice b)
{
    f32 x[16], y[16];
    usz count = LinearReadPair(x, y, a, b, 1, 16, "add");

    if (count == 0)
    {
        return LinearNone();
    }

    Linear_Add(LINEAR.result, x, y, count);
    return LinearResult(count);
}

static SHUSlice LinearSubtract(SHUSlice a, SHUSlice b)
{
    f32 x[16], y[16];
    usz count = LinearReadPair(x, y, a, b, 1, 16, "subtract");

    if (count == 0)
    {
        return LinearNone();
    }

    Linear_Subtract(LINEAR.result, x, y, count);
    return LinearResult(count);
}

static SHUSlice LinearScale(SHUSlice v, f32 factor)
{
    f32 x[16];
    usz count = LinearCount(v, 1, 16, "scale");

    if (count == 0)
    {
        return LinearNone();
    }

    LinearRead(x, v, count);
    Linear_Scale(LINEAR.result, x, factor, count);
    return LinearResult(count);
}

static f32 LinearDot(SHUSlice a, SHUSlice b)
{
    f32 x[16], y[16];
    usz count = LinearReadPair(x, y, a, b, 1, 16, "dot");
    return count == 0 ? 0.0f : Linear_Dot(x, y, count);
}

static f32 LinearLength(SHUSlice v)
{
    f32 x[16];
    usz count = LinearCount(v, 1, 16, "length");
    LinearRead(x, v, count);
    return count == 0 ? 0.0f : Linear_Length(x, count);
}

static SHUSlice LinearNormalize(SHUSlice v)
{
    f32 x[16];
    usz count = LinearCount(v, 1, 16, "normalize");

    if (count == 0)
    {
        return LinearNone();
    }

    LinearRead(x, v, count);
    Linear_Normalize(LINEAR.result, x, count);
    return LinearResult(count);
}

static SHUSlice LinearLerp(SHUSlice a, SHUSlice b, f32 t)
{
    f32 x[16], y[16];
    usz count = LinearReadPair(x, y, a, b, 1, 16, "lerp");

    if (count == 0)
    {
        return LinearNone();
    }

    Linear_Lerp(LINEAR.result, x, y, t, count);
    return LinearResult(count);
}

static SHUSlice LinearCross(SHUSlice a, SHUSlice b)
{
    f32 x[3], y[3];

    if (LinearReadPair(x, y, a, b, 3, 3, "cross") == 0)
    {
        return LinearNone();
    }

    Linear_Cross(LINEAR.result, x, y);
    return LinearResult(3);
}

static SHUSlice LinearIdentity(void)
{
    Linear_Identity(LINEAR.result);
    return LinearResult(16);
}

static SHUSlice LinearMultiply(SHUSlice a, SHUSlice b)
{
    f32 x[16], y[16];

    if (LinearReadPair(x, y, a, b, 16, 16, "multiply") == 0)
    {
        return LinearNone();
    }

    Linear_Multiply(LINEAR.result, x, y);
    return LinearResult(16);
}

static SHUSlice LinearTransform(SHUSlice matrix, SHUSlice vector)
{
    f32 m[16], v[4];
    usz count = LinearCount(vector, 3, 4, "transform");

    if (LinearCount(matrix, 16, 16, "transform") == 0 || count == 0)
    {
        return LinearNone();
    }

    LinearRead(m, matrix, 16);
    LinearRead(v, vector, count);
    Linear_Transform(LINEAR.result, m, v, count);
    return LinearResult(count);
}

static SHUSlice LinearTranslation(f32 x, f32 y, f32 z)
{
    Linear_Translation(LINEAR.result, x, y, z);
    return LinearResult(16);
}

static SHUSlice LinearScaling(f32 x, f32 y, f32 z)
{
    Linear_Scaling(LINEAR.result, x, y, z);
    return LinearResult(16);
}

static SHUSlice LinearRotation(SHUSlice axis, f32 angle)
{
    f32 a[3];

    if (LinearCount(axis, 3, 3, "rotation") == 0)
    {
        return LinearNone();
    }

    LinearRead(a, axis, 3);
    Linear_Rotation(LINEAR.result, a, angle);
    return LinearResult(16);
}

static SHUSlice LinearPerspective(f32 fov, f32 aspect, f32 zNear, f32 zFar)
{
    Linear_Perspective(LINEAR.result, fov, aspect, zNear, zFar);
    return LinearResult(16);
}

static SHUSlice LinearOrthographic(f32 left, f32 right, f32 bottom, f32 top, f32 zNear, f32 zFar)
{
    Linear_Orthographic(LINEAR.result, left, right, bottom, top, zNear, zFar);
    return LinearResult(16);
}

static SHUSlice LinearLookAt(SHUSlice eye, SHUSlice target, SHUSlice up)
{
    f32 e[3], t[3], u[3];

    if (LinearCount(eye, 3, 3, "lookAt") == 0 || LinearCount(target, 3, 3, "lookAt") == 0 || LinearCount(up, 3, 3, "lookAt") == 0)
    {
        return LinearNone();
    }

    LinearRead(e, eye, 3);
    LinearRead(t, target, 3);
    LinearRead(u, up, 3);
    Linear_LookAt(LINEAR.result, e, t, u);
    return LinearResult(16);
}

static SHUSlice LinearTranspose(SHUSlice matrix)
{
    f32 m[16];

    if (LinearCount(matrix, 16, 16, "transpose") == 0)
    {
        return LinearNone();
    }

    LinearRead(m, matrix, 16);
    Linear_Transpose(LINEAR.result, m);
    return LinearResult(16);
}

static SHUSlice LinearInverse(SHUSlice matrix)
{
    f32 m[16];

    if (LinearCount(matrix, 16, 16, "inverse") == 0)
    {
        return LinearNone();
    }

    LinearRead(m, matrix, 16);
    return Linear_Inverse(LINEAR.result, m) ? LinearResult(16) : LinearNone();
}

static SHUSlice LinearQuaternion(SHUSlice axis, f32 angle)
{
    f32 a[3];

    if (LinearCount(axis, 3, 3, "quaternion") == 0)
    {
        return LinearNone();
    }

    LinearRead(a, axis, 3);
    Linear_Quaternion(LINEAR.result, a, angle);
    return LinearResult(4);
}

static SHUSlice LinearQuaternionMultiply(SHUSlice a, SHUSlice b)
{
    f32 x[4], y[4];

    if (LinearReadPair(x, y, a, b, 4, 4, "quaternionMultiply") == 0)
    {
        return LinearNone();
    }

    Linear_QuaternionMultiply(LINEAR.result, x, y);
    return LinearResult(4);
}

static SHUSlice LinearSlerp(SHUSlice a, SHUSlice b, f32 t)
{
    f32 x[4], y[4];

    if (LinearReadPair(x, y, a, b, 4, 4, "slerp") == 0)
    {
        return LinearNone();
    }

    Linear_Slerp(LINEAR.result, x, y, t);
    return LinearResult(4);
}

static SHUSlice LinearQuaternionMatrix(SHUSlice quaternion)
{
    f32 q[4];

    if (LinearCount(quaternion, 4, 4, "quaternionMatrix") == 0)
    {
        return LinearNone();
    }

    LinearRead(q, quaternion, 4);
    Linear_QuaternionMatrix(LINEAR.result, q);
    return LinearResult(16);
}

static SHUSlice LinearCompose(SHUSlice translation, SHUSlice rotation, SHUSlice scale)
{
    f32 t[3], r[4], s[3];

    if (LinearCount(translation, 3, 3, "compose") == 0 || LinearCount(rotation, 4, 4, "compose") == 0 || LinearCount(scale, 3, 3, "compose") == 0)
    {
        return LinearNone();
    }

    LinearRead(t, translation, 3);
    LinearRead(r, rotation, 4);
    LinearRead(s, scale, 3);
    Linear_Compose(LINEAR.result, t, r, s);
    return LinearResult(16);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    LINEAR.plugin = plugin;
    const ECSPluginDesc desc = {0};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));

    // each result is a buffer of floats, kept until the next call; C plugins call header.h's functions instead
    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"vec2", (ECSFunction)LinearVec2, "buffer(float x, float y)", "Make a vector of 2 floats"},
        {"vec3", (ECSFunction)LinearVec3, "buffer(float x, float y, float z)", "Make a vector of 3 floats, such as a point or a direction"},
        {"vec4", (ECSFunction)LinearVec4, "buffer(float x, float y, float z, float w)", "Make a vector of 4 floats, such as a colour for a shader"},
        {"components", (ECSFunction)LinearComponents, "void(buffer vector, out float x, out float y, out float z, out float w)", "Give the floats of a vector or quaternion; those it lacks are 0"},
        {"add", (ECSFunction)LinearAdd, "buffer(buffer a, buffer b)", "Add two vectors, or two matrices, of as many floats"},
        {"subtract", (ECSFunction)LinearSubtract, "buffer(buffer a, buffer b)", "Subtract b from a, two vectors or matrices of as many floats"},
        {"scale", (ECSFunction)LinearScale, "buffer(buffer vector, float factor)", "Multiply each float of a vector or matrix by a number"},
        {"dot", (ECSFunction)LinearDot, "float(buffer a, buffer b)", "Give the dot product of two vectors of as many floats"},
        {"length", (ECSFunction)LinearLength, "float(buffer vector)", "Give the length of a vector"},
        {"normalize", (ECSFunction)LinearNormalize, "buffer(buffer vector)", "Give a vector the length 1, in its direction; a vector of length 0 stays 0"},
        {"lerp", (ECSFunction)LinearLerp, "buffer(buffer a, buffer b, float t)", "Give the vector at t between a, at 0, and b, at 1"},
        {"cross", (ECSFunction)LinearCross, "buffer(buffer a, buffer b)", "Give the cross product of two vectors of 3 floats"},
        {"identity", (ECSFunction)LinearIdentity, "buffer()", "Give the matrix that changes nothing"},
        {"multiply", (ECSFunction)LinearMultiply, "buffer(buffer a, buffer b)", "Multiply matrix a by matrix b: the result does b, then a"},
        {"transform", (ECSFunction)LinearTransform, "buffer(buffer matrix, buffer vector)", "Multiply a matrix by a vector of 4 floats, or of 3, a point: its w is 1, and the result is divided by its own w"},
        {"translation", (ECSFunction)LinearTranslation, "buffer(float x, float y, float z)", "Give the matrix that moves by x, y and z"},
        {"scaling", (ECSFunction)LinearScaling, "buffer(float x, float y, float z)", "Give the matrix that scales by x, y and z"},
        {"rotation", (ECSFunction)LinearRotation, "buffer(buffer axis, float angle)", "Give the matrix that turns by an angle in radians around an axis, counterclockwise as the axis points at the viewer"},
        {"perspective", (ECSFunction)LinearPerspective, "buffer(float fov, float aspect, float near, float far)", "Give a perspective that sees down -z, with a vertical field of view in radians and the width's ratio to the height; the depth goes from 0, at near, to 1, at far, as gpu takes it"},
        {"orthographic", (ECSFunction)LinearOrthographic, "buffer(float left, float right, float bottom, float top, float near, float far)", "Give a projection without perspective, of a box that sees down -z; the depth goes from 0, at near, to 1, at far"},
        {"lookAt", (ECSFunction)LinearLookAt, "buffer(buffer eye, buffer target, buffer up)", "Give the matrix of a camera at eye that looks at target, with up as near its up as it can be"},
        {"transpose", (ECSFunction)LinearTranspose, "buffer(buffer matrix)", "Swap a matrix's rows and columns"},
        {"inverse", (ECSFunction)LinearInverse, "buffer(buffer matrix)", "Give the matrix that undoes a matrix; an empty buffer if it has none"},
        {"quaternion", (ECSFunction)LinearQuaternion, "buffer(buffer axis, float angle)", "Give the quaternion that turns by an angle in radians around an axis, as rotation does"},
        {"quaternionMultiply", (ECSFunction)LinearQuaternionMultiply, "buffer(buffer a, buffer b)", "Multiply quaternion a by quaternion b: the result turns as b, then as a"},
        {"slerp", (ECSFunction)LinearSlerp, "buffer(buffer a, buffer b, float t)", "Give the turn at t between quaternions a, at 0, and b, at 1, the short way round, at an even speed"},
        {"quaternionMatrix", (ECSFunction)LinearQuaternionMatrix, "buffer(buffer quaternion)", "Give the matrix that turns as a quaternion of length 1 does"},
        {"compose", (ECSFunction)LinearCompose, "buffer(buffer translation, buffer rotation, buffer scale)", "Give the matrix that scales, then turns by a quaternion, then moves, as a glTF node does"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
