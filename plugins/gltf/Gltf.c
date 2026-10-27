// The gltf standard plugin: reads glTF 2.0 models, .gltf with their files or .glb, and gives their parts and their vertex data (DESIGN 8).
// It is a loader: draw's triangles and lines draw the vertex data, once a plugin has placed it on the screen.
// cgltf reads the files; a model's handle owns everything read, and frees it when the handle is gone. cglm does the matrix and quaternion math.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "cglm/cglm.h"

#pragma region Source Only

/// @brief A model that a file was read into.
typedef struct GltfModel
{
    cgltf_data *data;
    char *path;   // the full path of its file, which the files it names are relative to
    void *result; // what attribute, indices, image, pose or skin gave last, kept until the next call
} GltfModel;

static struct
{
    ECSPlugin plugin;
} GLTF = {0};

// cgltf allocates with SDL's allocator, as the plugin does
static void *GltfAllocate(void *user, cgltf_size size)
{
    (void)user;
    return SDL_malloc(size);
}

static void GltfDeallocate(void *user, void *pointer)
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
    char *fullPath = nullptr;

    // a relative path starts at the executable's folder
    if (ECSPath_GetFull(&fullPath, path))
    {
        return nullptr;
    }

    cgltf_options options = {.memory = {.alloc_func = GltfAllocate, .free_func = GltfDeallocate}};
    cgltf_data *data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, fullPath, &data);

    if (result == cgltf_result_success)
    {
        result = cgltf_load_buffers(&options, data, fullPath);
    }

    if (result == cgltf_result_success)
    {
        result = cgltf_validate(data);
    }

    GltfModel *model = result == cgltf_result_success ? SDL_calloc(1, sizeof(GltfModel)) : nullptr;

    if (model == nullptr)
    {
        ECS_LogError(GLTF.plugin, "Cannot read the model '%s': %s", fullPath, GltfResultText(result == cgltf_result_success ? cgltf_result_out_of_memory : result));
        cgltf_free(data);
        SDL_free(fullPath);
        return nullptr;
    }

    model->data = data;
    model->path = fullPath;
    return model;
}

static void GltfModelDestroy(void *object)
{
    GltfModel *model = object;
    cgltf_free(model->data);
    SDL_free(model->path);
    SDL_free(model->result);
    SDL_free(model);
}

/// @brief Frees a model now, instead of when no one uses its handle.
static void GltfFreeModel(GltfModel *model)
{
    ECSHandle_Free(GLTF.plugin, "model", model);
}

#pragma region Describe

/// @brief Sets a field of a table to a string; nullptr leaves it nil.
static SHUResult GltfSetFieldString(ECSValue *table, const char *name, const char *string)
{
    ECSValue *field = nullptr;

    if (string == nullptr)
    {
        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSValue_SetTableField(table, name, &field));
    return ECSValue_SetString(field, string);
}

/// @brief Sets a field of a table to a whole number.
static SHUResult GltfSetFieldInteger(ECSValue *table, const char *name, i64 integer)
{
    ECSValue *field = nullptr;
    SHU_ReturnResult(ECSValue_SetTableField(table, name, &field));
    ECSValue_SetInteger(field, integer);
    return SHUResult_Ok;
}

/// @brief Sets a field of a table to an empty table, which it gives.
static SHUResult GltfSetFieldTable(ECSValue *table, const char *name, ECSValue **retTable)
{
    SHU_ReturnResult(ECSValue_SetTableField(table, name, retTable));
    ECSValue_SetTable(*retTable);
    return SHUResult_Ok;
}

/// @brief Sets a field of a table to a list of numbers.
static SHUResult GltfSetFieldNumbers(ECSValue *table, const char *name, const cgltf_float *numbers, usz count)
{
    ECSValue *list = nullptr;
    SHU_ReturnResult(GltfSetFieldTable(table, name, &list));

    for (usz i = 0; i < count; i++)
    {
        ECSValue *item = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(list, &item));
        ECSValue_SetNumber(item, (f64)numbers[i]);
    }

    return SHUResult_Ok;
}

/// @brief Adds a whole number at the end of a list.
static SHUResult GltfAddItemInteger(ECSValue *list, i64 integer)
{
    ECSValue *item = nullptr;
    SHU_ReturnResult(ECSValue_AddListItem(list, &item));
    ECSValue_SetInteger(item, integer);
    return SHUResult_Ok;
}

/// @brief Adds an empty table at the end of a list, which it gives.
static SHUResult GltfAddItemTable(ECSValue *list, ECSValue **retTable)
{
    SHU_ReturnResult(ECSValue_AddListItem(list, retTable));
    ECSValue_SetTable(*retTable);
    return SHUResult_Ok;
}

/// @brief Finds a primitive's first attribute of a type, such as its positions, or nullptr.
static const cgltf_accessor *GltfFindAttribute(const cgltf_primitive *primitive, cgltf_attribute_type type)
{
    for (usz i = 0; primitive != nullptr && i < primitive->attributes_count; i++)
    {
        if (primitive->attributes[i].type == type && primitive->attributes[i].index == 0)
        {
            return primitive->attributes[i].data;
        }
    }

    return nullptr;
}

/// @brief Finds a primitive's attribute by its name, such as POSITION or TEXCOORD_0, or nullptr.
static const cgltf_accessor *GltfFindAttributeNamed(const cgltf_primitive *primitive, const char *name)
{
    for (usz i = 0; primitive != nullptr && i < primitive->attributes_count; i++)
    {
        if (SDL_strcmp(primitive->attributes[i].name, name) == 0)
        {
            return primitive->attributes[i].data;
        }
    }

    return nullptr;
}

/// @brief How many indices a primitive draws: its indices, or, without them, its vertices in order.
static usz GltfIndexCount(const cgltf_primitive *primitive)
{
    const cgltf_accessor *positions = GltfFindAttribute(primitive, cgltf_attribute_type_position);
    return primitive->indices != nullptr ? primitive->indices->count : positions != nullptr ? positions->count
                                                                                            : 0;
}

static SHUResult GltfDescribePrimitive(const cgltf_data *data, const cgltf_primitive *primitive, ECSValue *retPrimitive)
{
    // how the indices make shapes, as cgltf orders its primitive types
    static const char *const modes[] = {"triangles", "points", "lines", "lineLoop", "lineStrip", "triangles", "triangleStrip", "triangleFan"};
    SHU_ReturnResult(GltfSetFieldString(retPrimitive, "mode", (usz)primitive->type < SDL_arraysize(modes) ? modes[primitive->type] : "triangles"));

    if (primitive->material != nullptr)
    {
        SHU_ReturnResult(GltfSetFieldInteger(retPrimitive, "material", (i64)cgltf_material_index(data, primitive->material) + 1));
    }

    ECSValue *attributes = nullptr;
    SHU_ReturnResult(GltfSetFieldTable(retPrimitive, "attributes", &attributes));

    for (usz i = 0; i < primitive->attributes_count; i++)
    {
        ECSValue *name = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(attributes, &name));
        SHU_ReturnResult(ECSValue_SetString(name, primitive->attributes[i].name));
    }

    const cgltf_accessor *positions = GltfFindAttribute(primitive, cgltf_attribute_type_position);
    SHU_ReturnResult(GltfSetFieldInteger(retPrimitive, "vertices", positions == nullptr ? 0 : (i64)positions->count));
    return GltfSetFieldInteger(retPrimitive, "indices", (i64)GltfIndexCount(primitive));
}

static SHUResult GltfDescribeMesh(const cgltf_data *data, const cgltf_mesh *mesh, ECSValue *retMesh)
{
    ECSValue *primitives = nullptr;
    SHU_ReturnResult(GltfSetFieldString(retMesh, "name", mesh->name));
    SHU_ReturnResult(GltfSetFieldTable(retMesh, "primitives", &primitives));

    for (usz i = 0; i < mesh->primitives_count; i++)
    {
        ECSValue *primitive = nullptr;
        SHU_ReturnResult(GltfAddItemTable(primitives, &primitive));
        SHU_ReturnResult(GltfDescribePrimitive(data, &mesh->primitives[i], primitive));
    }

    return SHUResult_Ok;
}

/// @brief The URI of a file, or nullptr for the data of a URI, which is not a file.
static const char *GltfFileUri(const char *uri)
{
    return uri != nullptr && SDL_strncmp(uri, "data:", 5) != 0 ? uri : nullptr;
}

static SHUResult GltfDescribeMaterial(const cgltf_data *data, const cgltf_material *material, ECSValue *retMaterial)
{
    const cgltf_pbr_metallic_roughness *pbr = &material->pbr_metallic_roughness;
    const cgltf_texture *texture = pbr->base_color_texture.texture;
    SHU_ReturnResult(GltfSetFieldString(retMaterial, "name", material->name));
    SHU_ReturnResult(GltfSetFieldNumbers(retMaterial, "color", pbr->base_color_factor, 4));

    if (texture == nullptr || texture->image == nullptr)
    {
        return SHUResult_Ok;
    }

    // a texture inside a .glb has no file of its own, so only a texture in a file is named; image names any by its number, which gltf.image reads
    SHU_ReturnResult(GltfSetFieldInteger(retMaterial, "image", (i64)cgltf_image_index(data, texture->image) + 1));
    return GltfSetFieldString(retMaterial, "texture", GltfFileUri(texture->image->uri));
}

static SHUResult GltfDescribeImage(const cgltf_image *image, ECSValue *retImage)
{
    SHU_ReturnResult(GltfSetFieldString(retImage, "name", image->name));
    SHU_ReturnResult(GltfSetFieldString(retImage, "mimeType", image->mime_type));

    // an image is in a file, in the data of a URI, or in a buffer, as in a .glb
    return GltfSetFieldString(retImage, "file", GltfFileUri(image->uri));
}

static SHUResult GltfDescribeNode(const cgltf_data *data, const cgltf_node *node, ECSValue *retNode)
{
    cgltf_float matrix[16];
    cgltf_node_transform_local(node, matrix);
    SHU_ReturnResult(GltfSetFieldString(retNode, "name", node->name));
    SHU_ReturnResult(GltfSetFieldNumbers(retNode, "matrix", matrix, 16));

    if (node->mesh != nullptr)
    {
        SHU_ReturnResult(GltfSetFieldInteger(retNode, "mesh", (i64)cgltf_mesh_index(data, node->mesh) + 1));
    }

    if (node->skin != nullptr)
    {
        SHU_ReturnResult(GltfSetFieldInteger(retNode, "skin", (i64)cgltf_skin_index(data, node->skin) + 1));
    }

    ECSValue *children = nullptr;
    SHU_ReturnResult(GltfSetFieldTable(retNode, "children", &children));

    for (usz i = 0; i < node->children_count; i++)
    {
        SHU_ReturnResult(GltfAddItemInteger(children, (i64)cgltf_node_index(data, node->children[i]) + 1));
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

static SHUResult GltfDescribeAnimation(const cgltf_animation *animation, ECSValue *retAnimation)
{
    ECSValue *field = nullptr;
    SHU_ReturnResult(GltfSetFieldString(retAnimation, "name", animation->name));
    SHU_ReturnResult(ECSValue_SetTableField(retAnimation, "duration", &field));
    ECSValue_SetNumber(field, (f64)GltfDuration(animation));
    return SHUResult_Ok;
}

static SHUResult GltfDescribeSkin(const cgltf_data *data, const cgltf_skin *skin, ECSValue *retSkin)
{
    ECSValue *joints = nullptr;
    SHU_ReturnResult(GltfSetFieldString(retSkin, "name", skin->name));
    SHU_ReturnResult(GltfSetFieldTable(retSkin, "joints", &joints));

    for (usz i = 0; i < skin->joints_count; i++)
    {
        SHU_ReturnResult(GltfAddItemInteger(joints, (i64)cgltf_node_index(data, skin->joints[i]) + 1));
    }

    return SHUResult_Ok;
}

static SHUResult GltfDescribeModel(const cgltf_data *data, ECSValue *retInfo)
{
    ECSValue *meshes = nullptr;
    ECSValue *materials = nullptr;
    ECSValue *nodes = nullptr;
    ECSValue *roots = nullptr;
    ECSValue *images = nullptr;
    ECSValue *animations = nullptr;
    ECSValue *skins = nullptr;
    ECSValue *item = nullptr;
    ECSValue_SetTable(retInfo);
    SHU_ReturnResult(GltfSetFieldTable(retInfo, "meshes", &meshes));
    SHU_ReturnResult(GltfSetFieldTable(retInfo, "materials", &materials));
    SHU_ReturnResult(GltfSetFieldTable(retInfo, "nodes", &nodes));
    SHU_ReturnResult(GltfSetFieldTable(retInfo, "roots", &roots));
    SHU_ReturnResult(GltfSetFieldTable(retInfo, "images", &images));
    SHU_ReturnResult(GltfSetFieldTable(retInfo, "animations", &animations));
    SHU_ReturnResult(GltfSetFieldTable(retInfo, "skins", &skins));

    for (usz i = 0; i < data->meshes_count; i++)
    {
        SHU_ReturnResult(GltfAddItemTable(meshes, &item));
        SHU_ReturnResult(GltfDescribeMesh(data, &data->meshes[i], item));
    }

    for (usz i = 0; i < data->materials_count; i++)
    {
        SHU_ReturnResult(GltfAddItemTable(materials, &item));
        SHU_ReturnResult(GltfDescribeMaterial(data, &data->materials[i], item));
    }

    for (usz i = 0; i < data->images_count; i++)
    {
        SHU_ReturnResult(GltfAddItemTable(images, &item));
        SHU_ReturnResult(GltfDescribeImage(&data->images[i], item));
    }

    for (usz i = 0; i < data->animations_count; i++)
    {
        SHU_ReturnResult(GltfAddItemTable(animations, &item));
        SHU_ReturnResult(GltfDescribeAnimation(&data->animations[i], item));
    }

    for (usz i = 0; i < data->skins_count; i++)
    {
        SHU_ReturnResult(GltfAddItemTable(skins, &item));
        SHU_ReturnResult(GltfDescribeSkin(data, &data->skins[i], item));
    }

    for (usz i = 0; i < data->nodes_count; i++)
    {
        SHU_ReturnResult(GltfAddItemTable(nodes, &item));
        SHU_ReturnResult(GltfDescribeNode(data, &data->nodes[i], item));
    }

    // the nodes the scene starts from: the file's scene, or its first one
    const cgltf_scene *scene = data->scene != nullptr ? data->scene : data->scenes_count > 0 ? &data->scenes[0]
                                                                                             : nullptr;

    for (usz i = 0; scene != nullptr && i < scene->nodes_count; i++)
    {
        SHU_ReturnResult(GltfAddItemInteger(roots, (i64)cgltf_node_index(data, scene->nodes[i]) + 1));
    }

    return SHUResult_Ok;
}

/// @brief Gives a model's parts; a model too big for the memory left gives nil.
static void GltfDescribe(GltfModel *model, ECSValue *retInfo)
{
    if (GltfDescribeModel(model->data, retInfo) != SHUResult_Ok)
    {
        ECS_LogError(GLTF.plugin, "Out of memory while describing a model");
        ECSValue_SetNil(retInfo);
    }
}

#pragma endregion Describe

#pragma region Vertex data

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
static usz GltfCheckedSize(usz count, usz each)
{
    usz bytes = 0;
    return SDL_size_mul_check_overflow(count, each, &bytes) ? bytes : 0;
}

/// @brief Keeps what the model gives, freeing what it gave last, and gives it.
static SHUSlice GltfKeepResult(GltfModel *model, void *data, usz size)
{
    SDL_free(model->result);
    model->result = data;
    return cs(data, data == nullptr ? 0 : size);
}

static SHUSlice GltfAttribute(GltfModel *model, i32 mesh, i32 primitive, const char *name)
{
    const cgltf_accessor *accessor = GltfFindAttributeNamed(GltfFindPrimitive(model, mesh, primitive), name);

    // sparse and normalized data are unpacked to plain floats
    usz count = accessor == nullptr ? 0 : GltfCheckedSize(accessor->count, cgltf_num_components(accessor->type));
    f32 *floats = count == 0 || GltfCheckedSize(count, sizeof(f32)) == 0 ? nullptr : SDL_malloc(count * sizeof(f32));

    if (floats != nullptr && cgltf_accessor_unpack_floats(accessor, floats, count) != count)
    {
        SDL_free(floats);
        floats = nullptr;
    }

    return GltfKeepResult(model, floats, count * sizeof(f32));
}

static SHUSlice GltfIndices(GltfModel *model, i32 mesh, i32 primitive)
{
    const cgltf_primitive *found = GltfFindPrimitive(model, mesh, primitive);
    usz count = found == nullptr ? 0 : GltfIndexCount(found);
    u32 *indices = count == 0 || GltfCheckedSize(count, sizeof(u32)) == 0 ? nullptr : SDL_malloc(count * sizeof(u32));

    // a primitive without indices draws its vertices in order
    for (usz i = 0; indices != nullptr && i < count; i++)
    {
        indices[i] = (u32)(found->indices != nullptr ? cgltf_accessor_read_index(found->indices, i) : i);
    }

    return GltfKeepResult(model, indices, count * sizeof(u32));
}

#pragma endregion Vertex data

#pragma region Animation

/// @brief A node's translation, rotation as a quaternion x, y, z, w, and scale.
typedef struct GltfTransform
{
    vec3 translation;
    versor rotation;
    vec3 scale;
} GltfTransform;

/// @brief Reads a keyframe's value from a sampler's output: one element, or for a cubic spline one of its three, the in-tangent, the value and the out-tangent.
static void GltfReadKeyframe(const cgltf_animation_sampler *sampler, usz keyframe, usz element, f32 *retValue, usz components)
{
    bool cubic = sampler->interpolation == cgltf_interpolation_type_cubic_spline;
    (void)cgltf_accessor_read_float(sampler->output, cubic ? keyframe * 3 + element : keyframe, retValue, components);
}

/// @brief The keyframe at or before a time, found by halves, as the keyframes' times rise.
static usz GltfFindKeyframe(const cgltf_accessor *input, f32 seconds)
{
    usz low = 0;
    usz high = input->count;

    while (high - low > 1)
    {
        usz middle = (low + high) / 2;
        f32 time = 0.0f;
        (void)cgltf_accessor_read_float(input, middle, &time, 1);
        low = time <= seconds ? middle : low;
        high = time <= seconds ? high : middle;
    }

    return low;
}

/// @brief Sets the part of a transform that a channel animates to its value at a time: before the first keyframe the first, after the last the last.
static void GltfSample(const cgltf_animation_channel *channel, f32 seconds, GltfTransform *transform)
{
    const cgltf_animation_sampler *sampler = channel->sampler;
    bool rotation = channel->target_path == cgltf_animation_path_type_rotation;
    usz components = rotation ? 4 : 3;
    f32 *target = rotation ? transform->rotation : channel->target_path == cgltf_animation_path_type_translation ? transform->translation
                                                                                                                 : transform->scale;
    usz previous = GltfFindKeyframe(sampler->input, seconds);
    f32 start = 0.0f;
    f32 end = 0.0f;
    (void)cgltf_accessor_read_float(sampler->input, previous, &start, 1);

    if (previous + 1 >= sampler->input->count || seconds <= start || sampler->interpolation == cgltf_interpolation_type_step)
    {
        GltfReadKeyframe(sampler, seconds < start ? 0 : previous, 1, target, components);
        return;
    }

    (void)cgltf_accessor_read_float(sampler->input, previous + 1, &end, 1);
    f32 span = end - start;
    f32 progress = span > 0.0f ? (seconds - start) / span : 0.0f;
    versor previousKey = {0};
    versor nextKey = {0};
    GltfReadKeyframe(sampler, previous, 1, previousKey, components);
    GltfReadKeyframe(sampler, previous + 1, 1, nextKey, components);

    if (sampler->interpolation == cgltf_interpolation_type_linear && rotation)
    {
        glm_quat_slerp(previousKey, nextKey, progress, target);
        glm_quat_normalize(target);
        return;
    }

    if (sampler->interpolation == cgltf_interpolation_type_linear)
    {
        glm_vec3_lerp(previousKey, nextKey, progress, target);
        return;
    }

    // a cubic Hermite spline between the two keyframes, with the first one's out-tangent and the second one's in-tangent, scaled by the time between them
    versor outTangent = {0};
    versor inTangent = {0};
    GltfReadKeyframe(sampler, previous, 2, outTangent, components);
    GltfReadKeyframe(sampler, previous + 1, 0, inTangent, components);
    f32 squared = progress * progress;
    f32 cubed = squared * progress;

    for (usz i = 0; i < components; i++)
    {
        target[i] = (2.0f * cubed - 3.0f * squared + 1.0f) * previousKey[i] + (cubed - 2.0f * squared + progress) * span * outTangent[i] + (-2.0f * cubed + 3.0f * squared) * nextKey[i] +
                    (cubed - squared) * span * inTangent[i];
    }

    if (rotation)
    {
        glm_quat_normalize(target);
    }
}

/// @brief Gives each node's matrix in its parent at a time of an animation; animation 0 gives the nodes' own matrices.
/// @return The matrices, to free with SDL_free, or nullptr if out of memory or the model has no nodes.
static mat4 *GltfLocalMatrices(const cgltf_data *data, i32 animation, f32 seconds)
{
    usz count = data->nodes_count;
    GltfTransform *transforms = count == 0 ? nullptr : SDL_malloc(count * sizeof(GltfTransform));
    mat4 *matrices = count == 0 ? nullptr : SDL_malloc(count * sizeof(mat4));

    if (transforms == nullptr || matrices == nullptr)
    {
        SDL_free(transforms);
        SDL_free(matrices);
        return nullptr;
    }

    for (usz i = 0; i < count; i++)
    {
        const cgltf_node *node = &data->nodes[i];
        GltfTransform *transform = &transforms[i];
        *transform = (GltfTransform){.rotation = GLM_QUAT_IDENTITY_INIT, .scale = {1.0f, 1.0f, 1.0f}};
        SDL_memcpy(transform->translation, node->translation, node->has_translation ? sizeof(transform->translation) : 0);
        SDL_memcpy(transform->rotation, node->rotation, node->has_rotation ? sizeof(transform->rotation) : 0);
        SDL_memcpy(transform->scale, node->scale, node->has_scale ? sizeof(transform->scale) : 0);
    }

    // a node that glTF animates has no matrix of its own, so its parts change; morph weights are not part of a matrix
    if (animation >= 1 && (usz)animation <= data->animations_count)
    {
        const cgltf_animation *played = &data->animations[animation - 1];

        for (usz i = 0; i < played->channels_count; i++)
        {
            const cgltf_animation_channel *channel = &played->channels[i];

            if (channel->target_node != nullptr && !channel->target_node->has_matrix && channel->target_path != cgltf_animation_path_type_weights && channel->sampler->input->count > 0)
            {
                GltfSample(channel, seconds, &transforms[cgltf_node_index(data, channel->target_node)]);
            }
        }
    }

    // a matrix is the translation of the rotation of the scale
    for (usz i = 0; i < count; i++)
    {
        if (data->nodes[i].has_matrix)
        {
            SDL_memcpy(matrices[i], data->nodes[i].matrix, sizeof(mat4));
            continue;
        }

        glm_translate_make(matrices[i], transforms[i].translation);
        glm_quat_rotate(matrices[i], transforms[i].rotation, matrices[i]);
        glm_scale(matrices[i], transforms[i].scale);
    }

    SDL_free(transforms);
    return matrices;
}

static SHUSlice GltfPose(GltfModel *model, i32 animation, f32 seconds)
{
    mat4 *matrices = GltfLocalMatrices(model->data, animation, seconds);
    return GltfKeepResult(model, matrices, model->data->nodes_count * sizeof(mat4));
}

/// @brief Sets a node's matrix in the model's space from its matrix in its parent, the parent's first; done marks the nodes whose matrix is set.
static void GltfWorldMatrix(const cgltf_data *data, mat4 *locals, usz node, mat4 *worlds, bool *done)
{
    if (done[node])
    {
        return;
    }

    const cgltf_node *parent = data->nodes[node].parent;

    if (parent == nullptr)
    {
        glm_mat4_copy(locals[node], worlds[node]);
    }
    else
    {
        usz index = cgltf_node_index(data, parent);
        GltfWorldMatrix(data, locals, index, worlds, done);
        glm_mat4_mul(worlds[index], locals[node], worlds[node]);
    }

    done[node] = true;
}

/// @brief Gives each joint's matrix of a skin at a time of an animation: where its node is now, after the inverse of where it was when the mesh was bound to it.
/// @return The matrices, to free with SDL_free, or nullptr if out of memory.
static mat4 *GltfJointMatrices(const cgltf_data *data, const cgltf_skin *skin, i32 animation, f32 seconds)
{
    mat4 *locals = GltfLocalMatrices(data, animation, seconds);
    mat4 *worlds = SDL_malloc(data->nodes_count * sizeof(mat4));
    bool *done = SDL_calloc(data->nodes_count, sizeof(bool));
    mat4 *joints = SDL_malloc(skin->joints_count * sizeof(mat4));

    if (locals == nullptr || worlds == nullptr || done == nullptr || joints == nullptr)
    {
        SDL_free(joints);
        joints = nullptr;
    }

    for (usz i = 0; joints != nullptr && i < skin->joints_count; i++)
    {
        usz node = cgltf_node_index(data, skin->joints[i]);
        mat4 inverse = GLM_MAT4_IDENTITY_INIT;

        // glTF asks for a matrix for each joint, but nothing checks it, so a joint past the matrices keeps the identity
        if (skin->inverse_bind_matrices != nullptr && i < skin->inverse_bind_matrices->count)
        {
            (void)cgltf_accessor_read_float(skin->inverse_bind_matrices, i, (f32 *)inverse, 16);
        }

        GltfWorldMatrix(data, locals, node, worlds, done);
        glm_mat4_mul(worlds[node], inverse, joints[i]);
    }

    SDL_free(done);
    SDL_free(worlds);
    SDL_free(locals);
    return joints;
}

/// @brief Gives a primitive's positions moved by a skin's joints at a time of an animation, three floats each, in the model's space, as glTF skins them:
/// each vertex goes through the joints that JOINTS_0 names, by the weights that WEIGHTS_0 gives. A primitive without them, or a missing skin, gives its positions as they are.
static SHUSlice GltfSkin(GltfModel *model, i32 mesh, i32 primitive, i32 skinNumber, i32 animation, f32 seconds)
{
    const cgltf_data *data = model->data;
    const cgltf_primitive *found = GltfFindPrimitive(model, mesh, primitive);
    const cgltf_accessor *positions = GltfFindAttribute(found, cgltf_attribute_type_position);
    const cgltf_accessor *jointsOfVertices = GltfFindAttribute(found, cgltf_attribute_type_joints);
    const cgltf_accessor *weightsOfVertices = GltfFindAttribute(found, cgltf_attribute_type_weights);
    const cgltf_skin *skin = skinNumber >= 1 && (usz)skinNumber <= data->skins_count ? &data->skins[skinNumber - 1] : nullptr;
    usz count = positions == nullptr || GltfCheckedSize(positions->count, sizeof(vec3)) == 0 ? 0 : positions->count;
    vec3 *vertices = count == 0 ? nullptr : SDL_malloc(count * sizeof(vec3));

    if (vertices == nullptr || cgltf_accessor_unpack_floats(positions, (f32 *)vertices, count * 3) != count * 3)
    {
        SDL_free(vertices);
        return GltfKeepResult(model, nullptr, 0);
    }

    if (skin == nullptr || jointsOfVertices == nullptr || weightsOfVertices == nullptr || skin->joints_count == 0)
    {
        return GltfKeepResult(model, vertices, count * sizeof(vec3));
    }

    mat4 *joints = GltfJointMatrices(data, skin, animation, seconds);

    if (joints == nullptr)
    {
        ECS_LogError(GLTF.plugin, "Out of memory while skinning a primitive of the model '%s'", model->path);
        return GltfKeepResult(model, vertices, count * sizeof(vec3));
    }

    for (usz vertex = 0; vertex < count; vertex++)
    {
        cgltf_uint jointIndices[4] = {0};
        f32 weights[4] = {0};
        vec3 sum = GLM_VEC3_ZERO_INIT;
        f32 total = 0.0f;
        (void)cgltf_accessor_read_uint(jointsOfVertices, vertex, jointIndices, 4);
        (void)cgltf_accessor_read_float(weightsOfVertices, vertex, weights, 4);

        for (usz i = 0; i < 4; i++)
        {
            if (weights[i] == 0.0f || jointIndices[i] >= skin->joints_count)
            {
                continue;
            }

            vec3 moved;
            glm_mat4_mulv3(joints[jointIndices[i]], vertices[vertex], 1.0f, moved);
            glm_vec3_muladds(moved, weights[i], sum);
            total += weights[i];
        }

        // a vertex that no joint moves stays
        if (total > 0.0f)
        {
            glm_vec3_divs(sum, total, vertices[vertex]);
        }
    }

    SDL_free(joints);
    return GltfKeepResult(model, vertices, count * sizeof(vec3));
}

#pragma endregion Animation

#pragma region Images

/// @brief Copies the bytes of an image in a buffer, as in a .glb.
/// @return The bytes, to free with SDL_free, or nullptr.
static void *GltfReadBufferImage(const cgltf_image *image, usz *retSize)
{
    const u8 *bytes = cgltf_buffer_view_data(image->buffer_view);
    void *copy = bytes == nullptr ? nullptr : SDL_malloc(SDL_max(image->buffer_view->size, 1));

    if (copy == nullptr)
    {
        return nullptr;
    }

    SDL_memcpy(copy, bytes, image->buffer_view->size);
    *retSize = image->buffer_view->size;
    return copy;
}

/// @brief Decodes the bytes of a data URI, which holds them in base64 after its comma: four letters for three bytes, less the padding.
/// @return The bytes, to free with SDL_free, or nullptr.
static void *GltfReadDataUri(const char *uri, usz *retSize)
{
    const char *comma = SDL_strchr(uri, ',');
    const char *base64 = comma == nullptr ? nullptr : SDL_strstr(uri, ";base64,") == comma - 7 ? comma + 1
                                                                                               : nullptr;
    usz length = base64 == nullptr ? 0 : SDL_strlen(base64);

    if (length < 4 || length % 4 != 0)
    {
        return nullptr;
    }

    usz size = length / 4 * 3 - (base64[length - 1] == '=' ? 1 : 0) - (base64[length - 2] == '=' ? 1 : 0);
    void *bytes = nullptr;
    cgltf_options options = {.memory = {.alloc_func = GltfAllocate, .free_func = GltfDeallocate}};

    if (cgltf_load_buffer_base64(&options, size, base64, &bytes) != cgltf_result_success)
    {
        return nullptr;
    }

    *retSize = size;
    return bytes;
}

/// @brief Reads an image's file, whose URI is relative to the model's folder, and may escape characters, such as %20 for a space.
/// @return The bytes, to free with SDL_free, or nullptr.
static void *GltfReadImageFile(const GltfModel *model, const char *uri, usz *retSize)
{
    const char *slash = ECSPath_FindLastSeparator(model->path);
    int folderLength = slash == nullptr ? 0 : (int)(slash - model->path + 1);
    char *path = nullptr;

    if (SDL_asprintf(&path, "%.*s%s", folderLength, model->path, uri) < 0)
    {
        return nullptr;
    }

    cgltf_decode_uri(path + folderLength);
    void *bytes = SDL_LoadFile(path, retSize);
    SDL_free(path);
    return bytes;
}

/// @brief Reads the bytes of an image: from its buffer, as in a .glb, from the data of its URI, or from its file, next to the model's.
/// @return The bytes, to free with SDL_free, or nullptr.
static void *GltfReadImage(const GltfModel *model, const cgltf_image *image, usz *retSize)
{
    if (image->buffer_view != nullptr)
    {
        return GltfReadBufferImage(image, retSize);
    }

    if (image->uri == nullptr)
    {
        return nullptr;
    }

    if (GltfFileUri(image->uri) == nullptr)
    {
        return GltfReadDataUri(image->uri, retSize);
    }

    return GltfReadImageFile(model, image->uri, retSize);
}

static SHUSlice GltfImage(GltfModel *model, i32 image)
{
    if (image < 1 || (usz)image > model->data->images_count)
    {
        return GltfKeepResult(model, nullptr, 0);
    }

    usz size = 0;
    void *bytes = GltfReadImage(model, &model->data->images[image - 1], &size);

    if (bytes == nullptr)
    {
        ECS_LogWarn(GLTF.plugin, "Cannot read image %d of the model '%s'", (int)image, model->path);
    }

    return GltfKeepResult(model, bytes, size);
}

#pragma endregion Images

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    GLTF.plugin = plugin;
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "model", GltfModelDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"load", (ECSFunction)GltfLoad, "handle<gltf.model>(string path)",
         "Read a .gltf or .glb file with the files it uses; nothing if it cannot be read\n"
         "@param path The file's path; a relative path starts at the executable's folder, and the files a .gltf names are relative to its own folder\n"
         "@return A new model, which owns all that was read, or nothing if the file cannot be read or is not valid glTF; the reason is logged"},
        {"free", (ECSFunction)GltfFreeModel, "void(handle<gltf.model> model)",
         "Free a model now, instead of when no one uses its handle\n"
         "@param model The model, which is not used again after this"},
        {"describe", (ECSFunction)GltfDescribe, "void(handle<gltf.model> model, out value<gltf.Info?> info)",
         "Give a model's meshes, materials, nodes and root nodes as a table\n"
         "@param model The model\n"
         "@param info Set to the model's meshes, materials, images, nodes, animations, skins and roots, numbered from 1; nothing if memory runs out"},
        {"attribute", (ECSFunction)GltfAttribute, "buffer(handle<gltf.model> model, int mesh, int primitive, string name)",
         "Give a vertex attribute of a primitive, such as POSITION, as floats; kept until the next attribute, indices, image, pose or skin call on the model\n"
         "@param model The model\n"
         "@param mesh The mesh's number in describe's meshes, from 1\n"
         "@param primitive The primitive's number in the mesh's primitives, from 1\n"
         "@param name The attribute's name as describe gives it, such as POSITION or TEXCOORD_0; case matters\n"
         "@return The attribute's 32-bit floats, one vertex after another, such as three for a POSITION; empty for a missing mesh, primitive or attribute"},
        {"indices", (ECSFunction)GltfIndices, "buffer(handle<gltf.model> model, int mesh, int primitive)",
         "Give the indices of a primitive as 32-bit numbers; kept until the next attribute, indices, image, pose or skin call on the model\n"
         "@param model The model\n"
         "@param mesh The mesh's number in describe's meshes, from 1\n"
         "@param primitive The primitive's number in the mesh's primitives, from 1\n"
         "@return The indices, from 0, or the vertices in order for a primitive without indices; empty for a missing mesh or primitive"},
        {"skin", (ECSFunction)GltfSkin, "buffer(handle<gltf.model> model, int mesh, int primitive, int skin, int animation, float seconds)",
         "Give a primitive's positions moved by a skin's joints at a time of an animation, in the model's space; kept until the next attribute, indices, image, pose or skin call on the model\n"
         "@param model The model\n"
         "@param mesh The mesh's number in describe's meshes, from 1\n"
         "@param primitive The primitive's number in the mesh's primitives, from 1\n"
         "@param skin The skin's number in describe's skins, from 1, such as the one its node names\n"
         "@param animation The animation's number in describe's animations, from 1; 0 or a missing one leaves the joints where the nodes' own matrices put them\n"
         "@param seconds The time in the animation; before its first key the first key holds, and after its last the last\n"
         "@return Three 32-bit floats for each vertex; the positions as they are for a missing skin or a primitive without JOINTS_0 and WEIGHTS_0, and empty for a missing mesh or primitive"},
        {"pose", (ECSFunction)GltfPose, "buffer(handle<gltf.model> model, int animation, float seconds)",
         "Give each node's matrix in its parent at a time of an animation, 16 floats each; animation 0 gives the nodes' own; kept until the next attribute, indices, image, pose or skin call on the model\n"
         "@param model The model\n"
         "@param animation The animation's number in describe's animations, from 1; 0 or a missing one gives the nodes' own matrices\n"
         "@param seconds The time in the animation; before its first key the first key holds, and after its last the last\n"
         "@return 16 32-bit floats for each node, column by column, in the order of describe's nodes; empty for a model without nodes"},
        {"image", (ECSFunction)GltfImage, "buffer(handle<gltf.model> model, int image)",
         "Give the bytes of an image, such as a PNG file's, from the model's file or from its own; kept until the next attribute, indices, image, pose or skin call on the model\n"
         "@param model The model\n"
         "@param image The image's number in describe's images, from 1, such as a material's image\n"
         "@return The image file's bytes, which image.decode reads; empty for a missing image or one that cannot be read"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a handle, such as handle:describe(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"model", "describe", "describe"},
        {"model", "attribute", "attribute"},
        {"model", "indices", "indices"},
        {"model", "pose", "pose"},
        {"model", "skin", "skin"},
        {"model", "image", "image"},
        {"model", "free", "free"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function));
    }

    return SHUResult_Ok;
}
