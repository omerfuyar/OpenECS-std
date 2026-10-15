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

    ECSValue *children = nullptr;
    SHU_ReturnResult(GltfSetTable(item, "children", &children));

    for (usz i = 0; i < node->children_count; i++)
    {
        SHU_ReturnResult(GltfSetInteger(children, nullptr, (i64)cgltf_node_index(data, node->children[i]) + 1));
    }

    return SHUResult_Ok;
}

static SHUResult GltfDescribeModel(const cgltf_data *data, ECSValue *info)
{
    ECSValue *meshes = nullptr;
    ECSValue *materials = nullptr;
    ECSValue *nodes = nullptr;
    ECSValue *roots = nullptr;
    ECSValue *images = nullptr;
    ECSValue *item = nullptr;
    ECSValue_SetTable(info);
    SHU_ReturnResult(GltfSetTable(info, "meshes", &meshes));
    SHU_ReturnResult(GltfSetTable(info, "materials", &materials));
    SHU_ReturnResult(GltfSetTable(info, "nodes", &nodes));
    SHU_ReturnResult(GltfSetTable(info, "roots", &roots));
    SHU_ReturnResult(GltfSetTable(info, "images", &images));

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
    usz count = accessor == nullptr ? 0 : accessor->count * cgltf_num_components(accessor->type);
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
    u32 *indices = count == 0 ? nullptr : SDL_malloc(count * sizeof(u32));

    for (usz i = 0; indices != nullptr && i < count; i++)
    {
        indices[i] = (u32)(part->indices != nullptr ? cgltf_accessor_read_index(part->indices, i) : i);
    }

    return GltfGive(model, indices, count * sizeof(u32));
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
        {"attribute", (ECSFunction)GltfAttribute, "buffer(handle<gltf.model> model, int mesh, int primitive, string name)", "Give a vertex attribute of a primitive, such as POSITION, as floats; kept until the next attribute, indices or image call on the model"},
        {"indices", (ECSFunction)GltfIndices, "buffer(handle<gltf.model> model, int mesh, int primitive)", "Give the indices of a primitive as 32-bit numbers; kept until the next attribute, indices or image call on the model"},
        {"image", (ECSFunction)GltfImage, "buffer(handle<gltf.model> model, int image)", "Give the bytes of an image, such as a PNG file's, from the model's file or from its own; kept until the next attribute, indices or image call on the model"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}
