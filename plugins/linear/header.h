// linear in C: the same math as linear's functions, as inline functions on arrays of floats, so C needs no call (DESIGN 16).
// A vector is 2, 3 or 4 floats, a quaternion is 4 (x, y, z and w, as glTF has them), and a matrix is 16, column by column, as shaders read them.
// A function writes its result into out, which may be one of its inputs:
//
//     f32 model[16], view[16], projection[16], viewProjection[16];
//     Linear_Rotation(model, (f32[3]){0.0f, 1.0f, 0.0f}, seconds);
//     Linear_LookAt(view, (f32[3]){0.0f, 2.0f, 5.0f}, (f32[3]){0.0f, 0.0f, 0.0f}, (f32[3]){0.0f, 1.0f, 0.0f});
//     Linear_Perspective(projection, 0.8f, width / height, 0.1f, 100.0f);
//     Linear_Multiply(viewProjection, projection, view);

#include <math.h>
#include <string.h>

/// @brief Adds two vectors of count floats.
static inline void Linear_Add(f32 *out, const f32 *a, const f32 *b, usz count)
{
    for (usz i = 0; i < count; i++)
    {
        out[i] = a[i] + b[i];
    }
}

/// @brief Subtracts b from a, two vectors of count floats.
static inline void Linear_Subtract(f32 *out, const f32 *a, const f32 *b, usz count)
{
    for (usz i = 0; i < count; i++)
    {
        out[i] = a[i] - b[i];
    }
}

/// @brief Multiplies each float of a vector of count floats by a number.
static inline void Linear_Scale(f32 *out, const f32 *v, f32 factor, usz count)
{
    for (usz i = 0; i < count; i++)
    {
        out[i] = v[i] * factor;
    }
}

/// @brief Gives the dot product of two vectors of count floats.
static inline f32 Linear_Dot(const f32 *a, const f32 *b, usz count)
{
    f32 sum = 0.0f;

    for (usz i = 0; i < count; i++)
    {
        sum += a[i] * b[i];
    }

    return sum;
}

/// @brief Gives the length of a vector of count floats.
static inline f32 Linear_Length(const f32 *v, usz count)
{
    return sqrtf(Linear_Dot(v, v, count));
}

/// @brief Gives a vector of count floats the length 1, in its direction; a vector of length 0 stays 0.
static inline void Linear_Normalize(f32 *out, const f32 *v, usz count)
{
    f32 length = Linear_Length(v, count);
    Linear_Scale(out, v, length > 0.0f ? 1.0f / length : 0.0f, count);
}

/// @brief Gives the point at t between a, at 0, and b, at 1, two vectors of count floats.
static inline void Linear_Lerp(f32 *out, const f32 *a, const f32 *b, f32 t, usz count)
{
    for (usz i = 0; i < count; i++)
    {
        out[i] = a[i] + (b[i] - a[i]) * t;
    }
}

/// @brief Gives the cross product of two vectors of 3 floats.
static inline void Linear_Cross(f32 *out, const f32 *a, const f32 *b)
{
    f32 x = a[1] * b[2] - a[2] * b[1];
    f32 y = a[2] * b[0] - a[0] * b[2];
    f32 z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/// @brief Gives the matrix that changes nothing.
static inline void Linear_Identity(f32 *out)
{
    for (usz i = 0; i < 16; i++)
    {
        out[i] = i % 5 == 0 ? 1.0f : 0.0f;
    }
}

/// @brief Multiplies matrix a by matrix b: the result does b, then a.
static inline void Linear_Multiply(f32 *out, const f32 *a, const f32 *b)
{
    f32 m[16];

    for (usz column = 0; column < 4; column++)
    {
        for (usz row = 0; row < 4; row++)
        {
            f32 sum = 0.0f;

            for (usz k = 0; k < 4; k++)
            {
                sum += a[k * 4 + row] * b[column * 4 + k];
            }

            m[column * 4 + row] = sum;
        }
    }

    memcpy(out, m, sizeof(m));
}

/// @brief Multiplies a matrix by a vector of 4 floats, or of 3, a point: its w is 1, and the result is divided by its own w, as a perspective needs.
static inline void Linear_Transform(f32 *out, const f32 *m, const f32 *v, usz count)
{
    f32 in[4] = {v[0], v[1], v[2], count == 4 ? v[3] : 1.0f};
    f32 result[4];

    for (usz row = 0; row < 4; row++)
    {
        result[row] = m[row] * in[0] + m[4 + row] * in[1] + m[8 + row] * in[2] + m[12 + row] * in[3];
    }

    f32 divide = count == 4 || result[3] == 0.0f ? 1.0f : 1.0f / result[3];

    for (usz i = 0; i < count; i++)
    {
        out[i] = result[i] * divide;
    }
}

/// @brief Gives the matrix that moves by x, y and z.
static inline void Linear_Translation(f32 *out, f32 x, f32 y, f32 z)
{
    Linear_Identity(out);
    out[12] = x;
    out[13] = y;
    out[14] = z;
}

/// @brief Gives the matrix that scales by x, y and z.
static inline void Linear_Scaling(f32 *out, f32 x, f32 y, f32 z)
{
    Linear_Identity(out);
    out[0] = x;
    out[5] = y;
    out[10] = z;
}

/// @brief Gives the matrix that turns by an angle in radians around an axis of 3 floats, counterclockwise as the axis points at the viewer.
static inline void Linear_Rotation(f32 *out, const f32 *axis, f32 angle)
{
    f32 n[3];
    Linear_Normalize(n, axis, 3);
    f32 x = n[0], y = n[1], z = n[2];
    f32 c = cosf(angle), s = sinf(angle), t = 1.0f - c;

    const f32 m[16] = {
        t * x * x + c,     t * x * y + s * z, t * x * z - s * y, 0.0f,
        t * x * y - s * z, t * y * y + c,     t * y * z + s * x, 0.0f,
        t * x * z + s * y, t * y * z - s * x, t * z * z + c,     0.0f,
        0.0f,              0.0f,              0.0f,              1.0f,
    };

    memcpy(out, m, sizeof(m));
}

/// @brief Gives a perspective that sees down -z, with a vertical field of view in radians and the width's ratio to the height;
/// the depth goes from 0, at zNear, to 1, at zFar, as SDL's GPU API takes it.
static inline void Linear_Perspective(f32 *out, f32 fov, f32 aspect, f32 zNear, f32 zFar)
{
    f32 f = 1.0f / tanf(fov / 2.0f);

    const f32 m[16] = {
        f / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, f, 0.0f, 0.0f,
        0.0f, 0.0f, zFar / (zNear - zFar), -1.0f,
        0.0f, 0.0f, zNear * zFar / (zNear - zFar), 0.0f,
    };

    memcpy(out, m, sizeof(m));
}

/// @brief Gives a projection without perspective, of a box that sees down -z; the depth goes from 0, at zNear, to 1, at zFar.
static inline void Linear_Orthographic(f32 *out, f32 left, f32 right, f32 bottom, f32 top, f32 zNear, f32 zFar)
{
    const f32 m[16] = {
        2.0f / (right - left), 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f / (zNear - zFar), 0.0f,
        -(right + left) / (right - left), -(top + bottom) / (top - bottom), zNear / (zNear - zFar), 1.0f,
    };

    memcpy(out, m, sizeof(m));
}

/// @brief Gives the matrix of a camera at eye that looks at target, with up as near its up as it can be; points of 3 floats each.
static inline void Linear_LookAt(f32 *out, const f32 *eye, const f32 *target, const f32 *up)
{
    f32 f[3], s[3], u[3];
    Linear_Subtract(f, target, eye, 3);
    Linear_Normalize(f, f, 3);
    Linear_Cross(s, f, up);
    Linear_Normalize(s, s, 3);
    Linear_Cross(u, s, f);

    const f32 m[16] = {
        s[0], u[0], -f[0], 0.0f,
        s[1], u[1], -f[1], 0.0f,
        s[2], u[2], -f[2], 0.0f,
        -Linear_Dot(s, eye, 3), -Linear_Dot(u, eye, 3), Linear_Dot(f, eye, 3), 1.0f,
    };

    memcpy(out, m, sizeof(m));
}

/// @brief Swaps a matrix's rows and columns.
static inline void Linear_Transpose(f32 *out, const f32 *m)
{
    f32 t[16];

    for (usz column = 0; column < 4; column++)
    {
        for (usz row = 0; row < 4; row++)
        {
            t[row * 4 + column] = m[column * 4 + row];
        }
    }

    memcpy(out, t, sizeof(t));
}

/// @brief Gives the matrix that undoes a matrix. Gives false, and leaves out as it was, if it has none.
static inline bool Linear_Inverse(f32 *out, const f32 *m)
{
    f32 inv[16];

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    f32 determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (determinant == 0.0f)
    {
        return false;
    }

    Linear_Scale(out, inv, 1.0f / determinant, 16);
    return true;
}

/// @brief Gives the quaternion that turns by an angle in radians around an axis of 3 floats, as Linear_Rotation does.
static inline void Linear_Quaternion(f32 *out, const f32 *axis, f32 angle)
{
    f32 n[3];
    Linear_Normalize(n, axis, 3);
    f32 s = sinf(angle / 2.0f);
    out[0] = n[0] * s;
    out[1] = n[1] * s;
    out[2] = n[2] * s;
    out[3] = cosf(angle / 2.0f);
}

/// @brief Multiplies quaternion a by quaternion b: the result turns as b, then as a.
static inline void Linear_QuaternionMultiply(f32 *out, const f32 *a, const f32 *b)
{
    f32 q[4] = {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };

    memcpy(out, q, sizeof(q));
}

/// @brief Gives the turn at t between quaternions a, at 0, and b, at 1, the short way round, at an even speed.
static inline void Linear_Slerp(f32 *out, const f32 *a, const f32 *b, f32 t)
{
    f32 to[4] = {b[0], b[1], b[2], b[3]};
    f32 cosine = Linear_Dot(a, b, 4);

    if (cosine < 0.0f)
    {
        cosine = -cosine;
        Linear_Scale(to, to, -1.0f, 4);
    }

    // close turns are lerped, where the sine below is too small to divide by
    if (cosine > 0.9995f)
    {
        Linear_Lerp(out, a, to, t, 4);
        Linear_Normalize(out, out, 4);
        return;
    }

    f32 angle = acosf(cosine);
    f32 sine = sinf(angle);
    f32 fromWeight = sinf((1.0f - t) * angle) / sine;
    f32 toWeight = sinf(t * angle) / sine;

    for (usz i = 0; i < 4; i++)
    {
        out[i] = a[i] * fromWeight + to[i] * toWeight;
    }
}

/// @brief Gives the matrix that turns as a quaternion of length 1 does.
static inline void Linear_QuaternionMatrix(f32 *out, const f32 *q)
{
    f32 x = q[0], y = q[1], z = q[2], w = q[3];

    const f32 m[16] = {
        1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w), 0.0f,
        2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w), 0.0f,
        2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    memcpy(out, m, sizeof(m));
}

/// @brief Gives the matrix that scales by scale, turns by rotation and then moves by translation, as a glTF node does;
/// translation and scale are 3 floats, and rotation is a quaternion.
static inline void Linear_Compose(f32 *out, const f32 *translation, const f32 *rotation, const f32 *scale)
{
    Linear_QuaternionMatrix(out, rotation);

    for (usz column = 0; column < 3; column++)
    {
        Linear_Scale(out + column * 4, out + column * 4, scale[column], 3);
    }

    out[12] = translation[0];
    out[13] = translation[1];
    out[14] = translation[2];
}
