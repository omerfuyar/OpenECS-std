// The gltf standard plugin: reads glTF 2.0 models, .gltf with their files or .glb, and gives their parts and their vertex data (DESIGN 8).
// It is a loader: draw's triangles and lines draw the vertex data, once a plugin has placed it on the screen.
// cgltf reads the files; a model's handle owns everything read, and frees it when the handle is gone.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#pragma region Source Only

/// @brief A model that a file was read into.
typedef struct GltfModel
{
    cgltf_data *data;
    char *path;  // the full path of its file, which the files it names are relative to
    void *given; // what attribute, indices or image gave last, kept until the next call
} GltfModel;

static struct
{
    ECSPlugin plugin;
} GLTF = {0};

static void *GltfAllocate(void *user, cgltf_size size)
{
    (void)user;
    return SDL_malloc(size);
}

static void GltfRelease(void *user, void *pointer)
{
    (void)user;
    SDL_free(pointer);
}

static const char *GltfResultText(cgltf_result result)
{
    switch (result)
    {
    case cgltf_result_data_too_short:
    case cgltf_result_invalid_json:
    case cgltf_result_invalid_gltf:
    case cgltf_result_unknown_format:
        return "not a valid glTF file";
    case cgltf_result_file_not_found:
        return "a file is missing";
    case cgltf_result_io_error:
        return "a file cannot be read";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "glTF 1.0 is not supported";
    default:
        return "invalid data";
    }
}

static GltfModel *GltfLoad(const char *path)
{
    char *full = nullptr;

    // a relative path starts at the executable's folder
    if (SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
    {
        return nullptr;
    }

    cgltf_options options = {.memory = {.alloc_func = GltfAllocate, .free_func = GltfRelease}};
    cgltf_data *data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, full, &data);

    if (result == cgltf_result_success)
    {
        result = cgltf_load_buffers(&options, data, full);
    }

    if (result == cgltf_result_success)
    {
        result = cgltf_validate(data);
    }

    GltfModel *model = result == cgltf_result_success ? SDL_calloc(1, sizeof(GltfModel)) : nullptr;

    if (model == nullptr)
    {
        ECS_Log(GLTF.plugin, ECSLogLevel_Error, "Cannot read the model '%s': %s", full, GltfResultText(result == cgltf_result_success ? cgltf_result_out_of_memory : result));
        cgltf_free(data);
        SDL_free(full);
        return nullptr;
    }

    model->data = data;
    model->path = full;
    return model;
}

/// @brief Sets a field of a table to a string; nullptr leaves it nil.
static SHUResult GltfSetString(ECSValue *table, const char *name, const char *string)
{
    ECSValue *field = nullptr;

    if (string == nullptr)
    {
        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSValue_TableSetField(table, name, &field));
    return ECSValue_SetString(field, string);
}

/// @brief Sets a field of a table, or an item added to a list when the name is nullptr, to a whole number.
static SHUResult GltfSetInteger(ECSValue *table, const char *name, i64 integer)
{
    ECSValue *field = nullptr;
    SHU_ReturnResult(name == nullptr ? ECSValue_ListAddItem(table, &field) : ECSValue_TableSetField(table, name, &field));
    ECSValue_SetInteger(field, integer);
    return SHUResult_Ok;
}

/// @brief Sets a field of a table, or an item added to a list when the name is nullptr, to an empty table.
static SHUResult GltfSetTable(ECSValue *table, const char *name, ECSValue **retTable)
{
    SHU_ReturnResult(name == nullptr ? ECSValue_ListAddItem(table, retTable) : ECSValue_TableSetField(table, name, retTable));
    ECSValue_SetTable(*retTable);
    return SHUResult_Ok;
}

/// @brief Sets a field of a table to a list of numbers.
static SHUResult GltfSetNumbers(ECSValue *table, const char *name, const cgltf_float *numbers, usz count)
{
    ECSValue *list = nullptr;
    SHU_ReturnResult(GltfSetTable(table, name, &list));

    for (usz i = 0; i < count; i++)
    {
        ECSValue *item = nullptr;
        SHU_ReturnResult(ECSValue_ListAddItem(list, &item));
        ECSValue_SetNumber(item, (f64)numbers[i]);
    }

    return SHUResult_Ok;
}

static SHUResult GltfDescribePrimitive(const cgltf_data *data, const cgltf_primitive *primitive, ECSValue *part)
{
    // how the indices make shapes, as cgltf orders its primitive types
    static const char *const modes[] = {"triangles", "points", "lines", "lineLoop", "lineStrip", "triangles", "triangleStrip", "triangleFan"};
    SHU_ReturnResult(GltfSetString(part, "mode", (usz)primitive->type < SDL_arraysize(modes) ? modes[primitive->type] : "triangles"));

    if (primitive->material != nullptr)
    {
        SHU_ReturnResult(GltfSetInteger(part, "material", (i64)cgltf_material_index(data, primitive->material) + 1));
    }

    const cgltf_accessor *position = nullptr;
    ECSValue *attributes = nullptr;
    SHU_ReturnResult(GltfSetTable(part, "attributes", &attributes));

    for (usz i = 0; i < primitive->attributes_count; i++)
    {
        ECSValue *name = nullptr;
        SHU_ReturnResult(ECSValue_ListAddItem(attributes, &name));
        SHU_ReturnResult(ECSValue_SetString(name, primitive->attributes[i].name));

        if (primitive->attributes[i].type == cgltf_attribute_type_position)
        {
            position = primitive->attributes[i].data;
        }
    }

    // a primitive without indices draws its vertices in order
    i64 vertices = position == nullptr ? 0 : (i64)position->count;
    SHU_ReturnResult(GltfSetInteger(part, "vertices", vertices));
    return GltfSetInteger(part, "indices", primitive->indices != nullptr ? (i64)primitive->indices->count : vertices);
}

static SHUResult GltfDescribeMesh(const cgltf_data *data, const cgltf_mesh *mesh, ECSValue *item)
{
    ECSValue *primitives = nullptr;
    SHU_ReturnResult(GltfSetString(item, "name", mesh->name));
    SHU_ReturnResult(GltfSetTable(item, "primitives", &primitives));

    for (usz i = 0; i < mesh->primitives_count; i++)
    {
        ECSValue *part = nullptr;
        SHU_ReturnResult(GltfSetTable(primitives, nullptr, &part));
        SHU_ReturnResult(GltfDescribePrimitive(data, &mesh->primitives[i], part));
    }

    return SHUResult_Ok;
}

static SHUResult GltfDescribeMaterial(const cgltf_data *data, const cgltf_material *material, ECSValue *item)
{
    const cgltf_pbr_metallic_roughness *pbr = &material->pbr_metallic_roughness;
    const cgltf_texture *texture = pbr->base_color_texture.texture;
    SHU_ReturnResult(GltfSetString(item, "name", material->name));
    SHU_ReturnResult(GltfSetNumbers(item, "color", pbr->base_color_factor, 4));

    // a texture inside a .glb has no file of its own, so only a texture in a file is named; image names any by its number, which gltf.image reads
    if (texture != nullptr && texture->image != nullptr)
    {
        SHU_ReturnResult(GltfSetInteger(item, "image", (i64)cgltf_image_index(data, texture->image) + 1));
    }

    const char *uri = texture != nullptr && texture->image != nullptr ? texture->image->uri : nullptr;
    return GltfSetString(item, "texture", uri != nullptr && SDL_strncmp(uri, "data:", 5) != 0 ? uri : nullptr);
}

static SHUResult GltfDescribeImage(const cgltf_image *image, ECSValue *item)
{
    SHU_ReturnResult(GltfSetString(item, "name", image->name));
    SHU_ReturnResult(GltfSetString(item, "mimeType", image->mime_type));

    // an image is in a file, in the data of a URI, or in a buffer, as in a .glb
    const char *uri = image->uri;
    return GltfSetString(item, "file", uri != nullptr && SDL_strncmp(uri, "data:", 5) != 0 ? uri : nullptr);
}

static SHUResult GltfDescribeNode(const cgltf_data *data, const cgltf_node *node, ECSValue *item)
{
    cgltf_float matrix[16];
    cgltf_node_transform_local(node, matrix);
    SHU_ReturnResult(GltfSetString(item, "name", node->name));
    SHU_ReturnResult(GltfSetNumbers(item, "matrix", matrix, 16));

    if (node->mesh != nullptr)
    {
        SHU_ReturnResult(GltfSetInteger(item, "mesh", (i64)cgltf_mesh_index(data, node->mesh) + 1));
    }

    if (node->skin != nullptr)
    {
        SHU_ReturnResult(GltfSetInteger(item, "skin", (i64)cgltf_skin_index(data, node->skin) + 1));
    }

    ECSValue *children = nullptr;
    SHU_ReturnResult(GltfSetTable(item, "children", &children));

    for (usz i = 0; i < node->children_count; i++)
    {
        SHU_ReturnResult(GltfSetInteger(children, nullptr, (i64)cgltf_node_index(data, node->children[i]) + 1));
    }

    return SHUResult_Ok;
}

/// @brief The time of an animation's last key, in seconds: when it ends.
static f32 GltfDuration(const cgltf_animation *animation)
{
    f32 duration = 0.0f;

    for (usz i = 0; i < animation->samplers_count; i++)
    {
        const cgltf_accessor *input = animation->samplers[i].input;
        f32 last = 0.0f;

        if (input != nullptr && input->count > 0 && cgltf_accessor_read_float(input, input->count - 1, &last, 1))
        {
            duration = SDL_max(duration, last);
        }
    }

    return duration;
}

static SHUResult GltfDescribeModel(const cgltf_data *data, ECSValue *info)
{
    ECSValue *meshes = nullptr;
    ECSValue *materials = nullptr;
    ECSValue *nodes = nullptr;
    ECSValue *roots = nullptr;
    ECSValue *images = nullptr;
    ECSValue *animations = nullptr;
    ECSValue *skins = nullptr;
    ECSValue *item = nullptr;
    ECSValue_SetTable(info);
    SHU_ReturnResult(GltfSetTable(info, "meshes", &meshes));
    SHU_ReturnResult(GltfSetTable(info, "materials", &materials));
    SHU_ReturnResult(GltfSetTable(info, "nodes", &nodes));
    SHU_ReturnResult(GltfSetTable(info, "roots", &roots));
    SHU_ReturnResult(GltfSetTable(info, "images", &images));
    SHU_ReturnResult(GltfSetTable(info, "animations", &animations));
    SHU_ReturnResult(GltfSetTable(info, "skins", &skins));

    for (usz i = 0; i < data->meshes_count; i++)
    {
        SHU_ReturnResult(GltfSetTable(meshes, nullptr, &item));
        SHU_ReturnResult(GltfDescribeMesh(data, &data->meshes[i], item));
    }

    for (usz i = 0; i < data->materials_count; i++)
    {
        SHU_ReturnResult(GltfSetTable(materials, nullptr, &item));
        SHU_ReturnResult(GltfDescribeMaterial(data, &data->materials[i], item));
    }

    for (usz i = 0; i < data->images_count; i++)
    {
        SHU_ReturnResult(GltfSetTable(images, nullptr, &item));
        SHU_ReturnResult(GltfDescribeImage(&data->images[i], item));
    }

    for (usz i = 0; i < data->animations_count; i++)
    {
        ECSValue *field = nullptr;
        SHU_ReturnResult(GltfSetTable(animations, nullptr, &item));
        SHU_ReturnResult(GltfSetString(item, "name", data->animations[i].name));
        SHU_ReturnResult(ECSValue_TableSetField(item, "duration", &field));
        ECSValue_SetNumber(field, (f64)GltfDuration(&data->animations[i]));
    }

    for (usz i = 0; i < data->skins_count; i++)
    {
        ECSValue *list = nullptr;
        SHU_ReturnResult(GltfSetTable(skins, nullptr, &item));
        SHU_ReturnResult(GltfSetString(item, "name", data->skins[i].name));
        SHU_ReturnResult(GltfSetTable(item, "joints", &list));

        for (usz j = 0; j < data->skins[i].joints_count; j++)
        {
            SHU_ReturnResult(GltfSetInteger(list, nullptr, (i64)cgltf_node_index(data, data->skins[i].joints[j]) + 1));
        }
    }

    for (usz i = 0; i < data->nodes_count; i++)
    {
        SHU_ReturnResult(GltfSetTable(nodes, nullptr, &item));
        SHU_ReturnResult(GltfDescribeNode(data, &data->nodes[i], item));
    }

    // the nodes the scene starts from: the file's scene, or its first one
    const cgltf_scene *scene = data->scene != nullptr ? data->scene : data->scenes_count > 0 ? &data->scenes[0]
                                                                                          : nullptr;

    for (usz i = 0; scene != nullptr && i < scene->nodes_count; i++)
    {
        SHU_ReturnResult(GltfSetInteger(roots, nullptr, (i64)cgltf_node_index(data, scene->nodes[i]) + 1));
    }

    return SHUResult_Ok;
}

/// @brief Gives a model's parts; a model too big for the memory left gives nil.
static void GltfDescribe(GltfModel *model, ECSValue *retInfo)
{
    if (GltfDescribeModel(model->data, retInfo) != SHUResult_Ok)
    {
        ECS_Log(GLTF.plugin, ECSLogLevel_Error, "Out of memory while describing a model");
        ECSValue_SetNil(retInfo);
    }
}

/// @brief Finds a primitive by the 1-based numbers of its mesh and of itself in the mesh.
static const cgltf_primitive *GltfFindPrimitive(const GltfModel *model, i32 mesh, i32 primitive)
{
    if (mesh < 1 || (usz)mesh > model->data->meshes_count || primitive < 1 || (usz)primitive > model->data->meshes[mesh - 1].primitives_count)
    {
        return nullptr;
    }

    return &model->data->meshes[mesh - 1].primitives[primitive - 1];
}

/// @brief The bytes of a number of values of a size each, or 0 if they would not fit in memory, as a hostile file may ask for.
static usz GltfBytes(usz count, usz each)
{
    usz bytes = 0;
    return SDL_size_mul_check_overflow(count, each, &bytes) ? bytes : 0;
}

/// @brief Keeps what the model gives, freeing what it gave last.
static SHUSlice GltfGive(GltfModel *model, void *data, usz size)
{
    SDL_free(model->given);
    model->given = data;
    return (SHUSlice){.data = data, .size = data == nullptr ? 0 : size};
}

static SHUSlice GltfAttribute(GltfModel *model, i32 mesh, i32 primitive, const char *name)
{
    const cgltf_primitive *part = GltfFindPrimitive(model, mesh, primitive);
    const cgltf_accessor *accessor = nullptr;

    for (usz i = 0; part != nullptr && i < part->attributes_count; i++)
    {
        if (SDL_strcmp(part->attributes[i].name, name) == 0)
        {
            accessor = part->attributes[i].data;
        }
    }

    // sparse and normalized data are unpacked to plain floats
    usz count = accessor == nullptr ? 0 : GltfBytes(accessor->count, cgltf_num_components(accessor->type));
    count = GltfBytes(count, sizeof(f32)) == 0 ? 0 : count;
    f32 *floats = count == 0 ? nullptr : SDL_malloc(count * sizeof(f32));

    if (floats != nullptr && cgltf_accessor_unpack_floats(accessor, floats, count) != count)
    {
        SDL_free(floats);
        floats = nullptr;
    }

    return GltfGive(model, floats, count * sizeof(f32));
}

static SHUSlice GltfIndices(GltfModel *model, i32 mesh, i32 primitive)
{
    const cgltf_primitive *part = GltfFindPrimitive(model, mesh, primitive);
    const cgltf_accessor *position = nullptr;

    for (usz i = 0; part != nullptr && i < part->attributes_count; i++)
    {
        if (part->attributes[i].type == cgltf_attribute_type_position)
        {
            position = part->attributes[i].data;
        }
    }

    // a primitive without indices draws its vertices in order
    usz count = part == nullptr ? 0 : part->indices != nullptr ? part->indices->count
                               : position != nullptr        ? position->count
                                                         : 0;
    count = GltfBytes(count, sizeof(u32)) == 0 ? 0 : count;
    u32 *indices = count == 0 ? nullptr : SDL_malloc(count * sizeof(u32));

    for (usz i = 0; indices != nullptr && i < count; i++)
    {
        indices[i] = (u32)(part->indices != nullptr ? cgltf_accessor_read_index(part->indices, i) : i);
    }

    return GltfGive(model, indices, count * sizeof(u32));
}

/// @brief A node's translation, rotation as a quaternion x, y, z, w, and scale.
typedef struct GltfTransform
{
    f32 translation[3];
    f32 rotation[4];
    f32 scale[3];
} GltfTransform;

/// @brief The matrix of a transform, column by column: the translation of the rotation of the scale.
static void GltfMatrix(const GltfTransform *transform, f32 *matrix)
{
    f32 x = transform->rotation[0], y = transform->rotation[1], z = transform->rotation[2], w = transform->rotation[3];
    const f32 *s = transform->scale;
    const f32 *t = transform->translation;
    const f32 columns[16] = {
        (1.0f - 2.0f * (y * y + z * z)) * s[0], 2.0f * (x * y + z * w) * s[0], 2.0f * (x * z - y * w) * s[0], 0.0f,
        2.0f * (x * y - z * w) * s[1], (1.0f - 2.0f * (x * x + z * z)) * s[1], 2.0f * (y * z + x * w) * s[1], 0.0f,
        2.0f * (x * z + y * w) * s[2], 2.0f * (y * z - x * w) * s[2], (1.0f - 2.0f * (x * x + y * y)) * s[2], 0.0f,
        t[0], t[1], t[2], 1.0f,
    };
    SDL_memcpy(matrix, columns, sizeof(columns));
}

/// @brief Makes a quaternion one unit long, as a rotation must be.
static void GltfNormalize(f32 *quaternion)
{
    f32 length = SDL_sqrtf(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] + quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);

    for (usz i = 0; length > 0.0f && i < 4; i++)
    {
        quaternion[i] /= length;
    }
}

/// @brief Turns from one rotation toward another by a share from 0 to 1, along the shorter way: spherical linear interpolation.
static void GltfSlerp(const f32 *from, const f32 *to, f32 share, f32 *result)
{
    f32 dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
    f32 sign = dot < 0.0f ? -1.0f : 1.0f;
    dot *= sign;
    f32 a = 1.0f - share;
    f32 b = share;

    // nearly the same rotations are mixed straight, as the sine below would be about 0
    if (dot < 0.9995f)
    {
        f32 angle = SDL_acosf(dot);
        a = SDL_sinf(a * angle) / SDL_sinf(angle);
        b = SDL_sinf(b * angle) / SDL_sinf(angle);
    }

    for (usz i = 0; i < 4; i++)
    {
        result[i] = a * from[i] + b * sign * to[i];
    }

    GltfNormalize(result);
}

/// @brief Reads a key's value from a sampler's output: one element, or the middle of its three for a cubic spline, whose others are its tangents.
static void GltfKey(const cgltf_animation_sampler *sampler, usz key, usz part, f32 *value, usz components)
{
    bool cubic = sampler->interpolation == cgltf_interpolation_type_cubic_spline;
    (void)cgltf_accessor_read_float(sampler->output, cubic ? key * 3 + part : key, value, components);
}

/// @brief Sets the part of a transform that a channel animates to its value at a time: before the first key the first, after the last the last.
static void GltfSample(const cgltf_animation_channel *channel, f32 seconds, GltfTransform *transform)
{
    const cgltf_animation_sampler *sampler = channel->sampler;
    usz keys = sampler->input->count;
    bool rotation = channel->target_path == cgltf_animation_path_type_rotation;
    usz components = rotation ? 4 : 3;
    f32 *target = rotation ? transform->rotation : channel->target_path == cgltf_animation_path_type_translation ? transform->translation : transform->scale;

    // the key before the time, found by halves, as the keys' times rise
    usz low = 0;
    usz high = keys;

    while (high - low > 1)
    {
        usz middle = (low + high) / 2;
        f32 time = 0.0f;
        (void)cgltf_accessor_read_float(sampler->input, middle, &time, 1);
        low = time <= seconds ? middle : low;
        high = time <= seconds ? high : middle;
    }

    f32 start = 0.0f;
    f32 end = 0.0f;
    (void)cgltf_accessor_read_float(sampler->input, low, &start, 1);

    if (low + 1 >= keys || seconds <= start || sampler->interpolation == cgltf_interpolation_type_step)
    {
        GltfKey(sampler, seconds < start ? 0 : low, 1, target, components);
        return;
    }

    (void)cgltf_accessor_read_float(sampler->input, low + 1, &end, 1);
    f32 span = end - start;
    f32 share = span > 0.0f ? (seconds - start) / span : 0.0f;
    f32 from[4] = {0};
    f32 to[4] = {0};
    GltfKey(sampler, low, 1, from, components);
    GltfKey(sampler, low + 1, 1, to, components);

    if (sampler->interpolation == cgltf_interpolation_type_linear)
    {
        if (rotation)
        {
            GltfSlerp(from, to, share, target);
            return;
        }

        for (usz i = 0; i < components; i++)
        {
            target[i] = from[i] + (to[i] - from[i]) * share;
        }

        return;
    }

    // a cubic Hermite spline between the two keys, with the first key's out-tangent and the second's in-tangent, scaled by the time between them
    f32 out[4] = {0};
    f32 in[4] = {0};
    GltfKey(sampler, low, 2, out, components);
    GltfKey(sampler, low + 1, 0, in, components);
    f32 s2 = share * share;
    f32 s3 = s2 * share;

    for (usz i = 0; i < components; i++)
    {
        target[i] = (2.0f * s3 - 3.0f * s2 + 1.0f) * from[i] + (s3 - 2.0f * s2 + share) * span * out[i] + (-2.0f * s3 + 3.0f * s2) * to[i] + (s3 - s2) * span * in[i];
    }

    if (rotation)
    {
        GltfNormalize(target);
    }
}

/// @brief Gives each node's matrix in its parent at a time of an animation, 16 floats each, column by column; animation 0 gives the nodes' own matrices.
/// @return The matrices, to free with SDL_free, or nullptr if out of memory or the model has no nodes.
static f32 *GltfLocalMatrices(const cgltf_data *data, i32 animation, f32 seconds)
{
    usz count = data->nodes_count;
    GltfTransform *transforms = count == 0 ? nullptr : SDL_malloc(count * sizeof(GltfTransform));
    f32 *matrices = transforms == nullptr ? nullptr : SDL_malloc(count * 16 * sizeof(f32));

    if (matrices == nullptr)
    {
        SDL_free(transforms);
        return nullptr;
    }

    for (usz i = 0; i < count; i++)
    {
        const cgltf_node *node = &data->nodes[i];
        GltfTransform *transform = &transforms[i];
        *transform = (GltfTransform){.rotation = {0.0f, 0.0f, 0.0f, 1.0f}, .scale = {1.0f, 1.0f, 1.0f}};
        SDL_memcpy(transform->translation, node->translation, node->has_translation ? sizeof(transform->translation) : 0);
        SDL_memcpy(transform->rotation, node->rotation, node->has_rotation ? sizeof(transform->rotation) : 0);
        SDL_memcpy(transform->scale, node->scale, node->has_scale ? sizeof(transform->scale) : 0);
    }

    // a node that glTF animates has no matrix of its own, so its parts change; morph weights are not part of a matrix
    if (animation >= 1 && (usz)animation <= data->animations_count)
    {
        const cgltf_animation *clip = &data->animations[animation - 1];

        for (usz i = 0; i < clip->channels_count; i++)
        {
            const cgltf_animation_channel *channel = &clip->channels[i];

            if (channel->target_node != nullptr && !channel->target_node->has_matrix && channel->target_path != cgltf_animation_path_type_weights && channel->sampler->input->count > 0)
            {
                GltfSample(channel, seconds, &transforms[cgltf_node_index(data, channel->target_node)]);
            }
        }
    }

    for (usz i = 0; i < count; i++)
    {
        if (data->nodes[i].has_matrix)
        {
            SDL_memcpy(matrices + i * 16, data->nodes[i].matrix, 16 * sizeof(f32));
        }
        else
        {
            GltfMatrix(&transforms[i], matrices + i * 16);
        }
    }

    SDL_free(transforms);
    return matrices;
}

static SHUSlice GltfPose(GltfModel *model, i32 animation, f32 seconds)
{
    f32 *matrices = GltfLocalMatrices(model->data, animation, seconds);
    return GltfGive(model, matrices, model->data->nodes_count * 16 * sizeof(f32));
}

/// @brief Multiplies two matrices of 16 floats, column by column: result = a times b; result may not be a or b.
static void GltfMultiply(const f32 *a, const f32 *b, f32 *result)
{
    for (usz column = 0; column < 4; column++)
    {
        for (usz row = 0; row < 4; row++)
        {
            f32 sum = 0.0f;

            for (usz k = 0; k < 4; k++)
            {
                sum += a[k * 4 + row] * b[column * 4 + k];
            }

            result[column * 4 + row] = sum;
        }
    }
}

/// @brief Sets a node's matrix in the model's space from its matrix in its parent, the parent's first; done marks the nodes whose matrix is set.
static void GltfWorld(const cgltf_data *data, const f32 *locals, usz node, f32 *worlds, bool *done)
{
    if (done[node])
    {
        return;
    }

    const cgltf_node *parent = data->nodes[node].parent;

    if (parent == nullptr)
    {
        SDL_memcpy(worlds + node * 16, locals + node * 16, 16 * sizeof(f32));
    }
    else
    {
        usz index = cgltf_node_index(data, parent);
        GltfWorld(data, locals, index, worlds, done);
        GltfMultiply(worlds + index * 16, locals + node * 16, worlds + node * 16);
    }

    done[node] = true;
}

/// @brief Finds a primitive's attribute of a type, such as its positions, or nullptr.
static const cgltf_accessor *GltfFindAttribute(const cgltf_primitive *part, cgltf_attribute_type type)
{
    for (usz i = 0; part != nullptr && i < part->attributes_count; i++)
    {
        if (part->attributes[i].type == type && part->attributes[i].index == 0)
        {
            return part->attributes[i].data;
        }
    }

    return nullptr;
}

/// @brief Gives a primitive's positions moved by a skin's joints at a time of an animation, three floats each, in the model's space, as glTF skins them:
/// each vertex goes through the joints that JOINTS_0 names, by the shares that WEIGHTS_0 gives. A primitive without them, or a missing skin, gives its positions as they are.
static SHUSlice GltfSkin(GltfModel *model, i32 mesh, i32 primitive, i32 skin, i32 animation, f32 seconds)
{
    const cgltf_data *data = model->data;
    const cgltf_primitive *part = GltfFindPrimitive(model, mesh, primitive);
    const cgltf_accessor *positions = GltfFindAttribute(part, cgltf_attribute_type_position);
    const cgltf_accessor *joints = GltfFindAttribute(part, cgltf_attribute_type_joints);
    const cgltf_accessor *weights = GltfFindAttribute(part, cgltf_attribute_type_weights);
    const cgltf_skin *bones = skin >= 1 && (usz)skin <= data->skins_count ? &data->skins[skin - 1] : nullptr;
    usz count = positions == nullptr || GltfBytes(positions->count, 3 * sizeof(f32)) == 0 ? 0 : positions->count;
    f32 *out = count == 0 ? nullptr : SDL_malloc(count * 3 * sizeof(f32));

    if (out == nullptr || cgltf_accessor_unpack_floats(positions, out, count * 3) != count * 3)
    {
        SDL_free(out);
        return GltfGive(model, nullptr, 0);
    }

    if (bones == nullptr || joints == nullptr || weights == nullptr || bones->joints_count == 0)
    {
        return GltfGive(model, out, count * 3 * sizeof(f32));
    }

    // each joint's matrix: where its node is now, after the inverse of where it was when the mesh was bound to it
    f32 *locals = GltfLocalMatrices(data, animation, seconds);
    f32 *worlds = locals == nullptr ? nullptr : SDL_malloc(data->nodes_count * 16 * sizeof(f32));
    bool *done = worlds == nullptr ? nullptr : SDL_calloc(data->nodes_count, sizeof(bool));
    f32 *matrices = done == nullptr ? nullptr : SDL_malloc(bones->joints_count * 16 * sizeof(f32));

    for (usz i = 0; matrices != nullptr && i < bones->joints_count; i++)
    {
        usz node = cgltf_node_index(data, bones->joints[i]);
        f32 inverse[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        // glTF asks for a matrix for each joint, but nothing checks it, so a joint past the matrices keeps the identity
        if (bones->inverse_bind_matrices != nullptr && i < bones->inverse_bind_matrices->count)
        {
            (void)cgltf_accessor_read_float(bones->inverse_bind_matrices, i, inverse, 16);
        }

        GltfWorld(data, locals, node, worlds, done);
        GltfMultiply(worlds + node * 16, inverse, matrices + i * 16);
    }

    for (usz v = 0; matrices != nullptr && v < count; v++)
    {
        cgltf_uint index[4] = {0};
        f32 share[4] = {0};
        f32 *p = out + v * 3;
        f32 x = p[0], y = p[1], z = p[2];
        f32 sum[3] = {0};
        f32 total = 0.0f;
        (void)cgltf_accessor_read_uint(joints, v, index, 4);
        (void)cgltf_accessor_read_float(weights, v, share, 4);

        for (usz k = 0; k < 4; k++)
        {
            if (share[k] == 0.0f || index[k] >= bones->joints_count)
            {
                continue;
            }

            const f32 *m = matrices + (usz)index[k] * 16;
            sum[0] += share[k] * (m[0] * x + m[4] * y + m[8] * z + m[12]);
            sum[1] += share[k] * (m[1] * x + m[5] * y + m[9] * z + m[13]);
            sum[2] += share[k] * (m[2] * x + m[6] * y + m[10] * z + m[14]);
            total += share[k];
        }

        // a vertex that no joint moves stays
        if (total > 0.0f)
        {
            p[0] = sum[0] / total;
            p[1] = sum[1] / total;
            p[2] = sum[2] / total;
        }
    }

    if (matrices == nullptr)
    {
        ECS_LogError(GLTF.plugin, "Out of memory while skinning a primitive of the model '%s'", model->path);
    }

    SDL_free(matrices);
    SDL_free(done);
    SDL_free(worlds);
    SDL_free(locals);
    return GltfGive(model, out, count * 3 * sizeof(f32));
}

/// @brief Reads the bytes of an image: from its buffer, as in a .glb, from the data of its URI, or from its file, next to the model's.
/// @return The bytes, to free with SDL_free, or nullptr; size is set.
static void *GltfReadImage(const GltfModel *model, const cgltf_image *image, usz *retSize)
{
    *retSize = 0;

    if (image->buffer_view != nullptr)
    {
        const u8 *bytes = cgltf_buffer_view_data(image->buffer_view);
        void *copy = bytes == nullptr ? nullptr : SDL_malloc(SDL_max(image->buffer_view->size, 1));

        if (copy != nullptr)
        {
            SDL_memcpy(copy, bytes, image->buffer_view->size);
            *retSize = image->buffer_view->size;
        }

        return copy;
    }

    const char *uri = image->uri;

    if (uri == nullptr)
    {
        return nullptr;
    }

    // a data URI holds the bytes in base64 after its comma: four letters for three bytes, less the padding
    if (SDL_strncmp(uri, "data:", 5) == 0)
    {
        const char *comma = SDL_strchr(uri, ',');
        const char *base64 = comma == nullptr ? nullptr : SDL_strstr(uri, ";base64,") == comma - 7 ? comma + 1 : nullptr;
        usz length = base64 == nullptr ? 0 : SDL_strlen(base64);
        usz size = length / 4 * 3 - (length >= 1 && base64[length - 1] == '=' ? 1 : 0) - (length >= 2 && base64[length - 2] == '=' ? 1 : 0);
        void *bytes = nullptr;
        cgltf_options options = {.memory = {.alloc_func = GltfAllocate, .free_func = GltfRelease}};

        if (length < 4 || length % 4 != 0 || cgltf_load_buffer_base64(&options, size, base64, &bytes) != cgltf_result_success)
        {
            return nullptr;
        }

        *retSize = size;
        return bytes;
    }

    // a file's URI is relative to the model's folder, and may escape characters, such as %20 for a space
    const char *slash = SDL_strrchr(model->path, '/');
    char *path = nullptr;

    if (SDL_asprintf(&path, "%.*s%s", slash == nullptr ? 0 : (int)(slash - model->path + 1), model->path, uri) < 0)
    {
        return nullptr;
    }

    cgltf_decode_uri(path + (slash == nullptr ? 0 : slash - model->path + 1));
    usz size = 0;
    void *bytes = SDL_LoadFile(path, &size);
    SDL_free(path);
    *retSize = bytes == nullptr ? 0 : size;
    return bytes;
}

static SHUSlice GltfImage(GltfModel *model, i32 image)
{
    if (image < 1 || (usz)image > model->data->images_count)
    {
        return GltfGive(model, nullptr, 0);
    }

    usz size = 0;
    void *bytes = GltfReadImage(model, &model->data->images[image - 1], &size);

    if (bytes == nullptr)
    {
        ECS_LogWarn(GLTF.plugin, "Cannot read image %d of the model '%s'", (int)image, model->path);
    }

    return GltfGive(model, bytes, size);
}

static void GltfDestroy(void *object)
{
    GltfModel *model = object;
    cgltf_free(model->data);
    SDL_free(model->path);
    SDL_free(model->given);
    SDL_free(model);
}

/// @brief Frees a model now, instead of when no one uses its handle.
static void GltfFree(void *object)
{
    ECSHandle_Forget(object);
    GltfDestroy(object);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    GLTF.plugin = plugin;
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "model", GltfDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"load", (ECSFunction)GltfLoad, "handle<gltf.model>(string path)", "Read a .gltf or .glb file with the files it uses; nothing if it cannot be read"},
        {"free", (ECSFunction)GltfFree, "void(handle<gltf.model> model)", "Free a model now, instead of when no one uses its handle"},
        {"describe", (ECSFunction)GltfDescribe, "void(handle<gltf.model> model, out value info)", "Give a model's meshes, materials, nodes and root nodes as a table"},
        {"attribute", (ECSFunction)GltfAttribute, "buffer(handle<gltf.model> model, int mesh, int primitive, string name)", "Give a vertex attribute of a primitive, such as POSITION, as floats; kept until the next attribute, indices, image, pose or skin call on the model"},
        {"indices", (ECSFunction)GltfIndices, "buffer(handle<gltf.model> model, int mesh, int primitive)", "Give the indices of a primitive as 32-bit numbers; kept until the next attribute, indices, image, pose or skin call on the model"},
        {"skin", (ECSFunction)GltfSkin, "buffer(handle<gltf.model> model, int mesh, int primitive, int skin, int animation, float seconds)", "Give a primitive's positions moved by a skin's joints at a time of an animation, in the model's space; kept until the next attribute, indices, image, pose or skin call on the model"},
        {"pose", (ECSFunction)GltfPose, "buffer(handle<gltf.model> model, int animation, float seconds)", "Give each node's matrix in its parent at a time of an animation, 16 floats each; animation 0 gives the nodes' own; kept until the next attribute, indices, image, pose or skin call on the model"},
        {"image", (ECSFunction)GltfImage, "buffer(handle<gltf.model> model, int image)", "Give the bytes of an image, such as a PNG file's, from the model's file or from its own; kept until the next attribute, indices, image, pose or skin call on the model"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
