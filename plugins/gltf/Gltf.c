// The gltf standard plugin: reads glTF 2.0 models, .gltf with their files or .glb, and gives their parts and their vertex data (DESIGN 8).
// cgltf reads the files; a model's handle owns everything read, and frees it when the handle is gone.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#pragma region Source Only

/// @brief A name of the plugin: "gltf." and a local name.
#define GLTF_NAME(localName) "gltf." localName

/// @brief A model that a file was read into.
typedef struct GltfModel
{
    cgltf_data *data;
    void *given; // what attribute or indices gave last, kept until the next call
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
    char *full = NULL;

    // a relative path starts at the executable's folder
    if (SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
    {
        return NULL;
    }

    cgltf_options options = {.memory = {.alloc_func = GltfAllocate, .free_func = GltfRelease}};
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse_file(&options, full, &data);

    if (result == cgltf_result_success)
    {
        result = cgltf_load_buffers(&options, data, full);
    }

    if (result == cgltf_result_success)
    {
        result = cgltf_validate(data);
    }

    GltfModel *model = result == cgltf_result_success ? SDL_calloc(1, sizeof(GltfModel)) : NULL;

    if (model == NULL)
    {
        ECS_Log(GLTF.plugin, ECSLogLevel_Error, "Cannot read the model '%s': %s", full, GltfResultText(result == cgltf_result_success ? cgltf_result_out_of_memory : result));
        cgltf_free(data);
        SDL_free(full);
        return NULL;
    }

    SDL_free(full);
    model->data = data;
    return model;
}

/// @brief Sets a field of a table to a string; NULL leaves it nil.
static SHUResult GltfSetString(ECSValue *table, const char *name, const char *string)
{
    ECSValue *field = NULL;

    if (string == NULL)
    {
        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSValue_TableSetField(table, name, &field));
    return ECSValue_SetString(field, string);
}

/// @brief Sets a field of a table, or an item added to a list when the name is NULL, to a whole number.
static SHUResult GltfSetInteger(ECSValue *table, const char *name, i64 integer)
{
    ECSValue *field = NULL;
    SHU_ReturnResult(name == NULL ? ECSValue_ListAddItem(table, &field) : ECSValue_TableSetField(table, name, &field));
    ECSValue_SetInteger(field, integer);
    return SHUResult_Ok;
}

/// @brief Sets a field of a table, or an item added to a list when the name is NULL, to an empty table.
static SHUResult GltfSetTable(ECSValue *table, const char *name, ECSValue **retTable)
{
    SHU_ReturnResult(name == NULL ? ECSValue_ListAddItem(table, retTable) : ECSValue_TableSetField(table, name, retTable));
    ECSValue_SetTable(*retTable);
    return SHUResult_Ok;
}

/// @brief Sets a field of a table to a list of numbers.
static SHUResult GltfSetNumbers(ECSValue *table, const char *name, const cgltf_float *numbers, usz count)
{
    ECSValue *list = NULL;
    SHU_ReturnResult(GltfSetTable(table, name, &list));

    for (usz i = 0; i < count; i++)
    {
        ECSValue *item = NULL;
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

    if (primitive->material != NULL)
    {
        SHU_ReturnResult(GltfSetInteger(part, "material", (i64)cgltf_material_index(data, primitive->material) + 1));
    }

    const cgltf_accessor *position = NULL;
    ECSValue *attributes = NULL;
    SHU_ReturnResult(GltfSetTable(part, "attributes", &attributes));

    for (usz i = 0; i < primitive->attributes_count; i++)
    {
        ECSValue *name = NULL;
        SHU_ReturnResult(ECSValue_ListAddItem(attributes, &name));
        SHU_ReturnResult(ECSValue_SetString(name, primitive->attributes[i].name));

        if (primitive->attributes[i].type == cgltf_attribute_type_position)
        {
            position = primitive->attributes[i].data;
        }
    }

    // a primitive without indices draws its vertices in order
    i64 vertices = position == NULL ? 0 : (i64)position->count;
    SHU_ReturnResult(GltfSetInteger(part, "vertices", vertices));
    return GltfSetInteger(part, "indices", primitive->indices != NULL ? (i64)primitive->indices->count : vertices);
}

static SHUResult GltfDescribeMesh(const cgltf_data *data, const cgltf_mesh *mesh, ECSValue *item)
{
    ECSValue *primitives = NULL;
    SHU_ReturnResult(GltfSetString(item, "name", mesh->name));
    SHU_ReturnResult(GltfSetTable(item, "primitives", &primitives));

    for (usz i = 0; i < mesh->primitives_count; i++)
    {
        ECSValue *part = NULL;
        SHU_ReturnResult(GltfSetTable(primitives, NULL, &part));
        SHU_ReturnResult(GltfDescribePrimitive(data, &mesh->primitives[i], part));
    }

    return SHUResult_Ok;
}

static SHUResult GltfDescribeMaterial(const cgltf_material *material, ECSValue *item)
{
    const cgltf_pbr_metallic_roughness *pbr = &material->pbr_metallic_roughness;
    const cgltf_texture *texture = pbr->base_color_texture.texture;
    SHU_ReturnResult(GltfSetString(item, "name", material->name));
    SHU_ReturnResult(GltfSetNumbers(item, "color", pbr->base_color_factor, 4));

    // a texture inside a .glb has no file of its own, so only a texture in a file is named
    return GltfSetString(item, "texture", texture != NULL && texture->image != NULL ? texture->image->uri : NULL);
}

static SHUResult GltfDescribeNode(const cgltf_data *data, const cgltf_node *node, ECSValue *item)
{
    cgltf_float matrix[16];
    cgltf_node_transform_local(node, matrix);
    SHU_ReturnResult(GltfSetString(item, "name", node->name));
    SHU_ReturnResult(GltfSetNumbers(item, "matrix", matrix, 16));

    if (node->mesh != NULL)
    {
        SHU_ReturnResult(GltfSetInteger(item, "mesh", (i64)cgltf_mesh_index(data, node->mesh) + 1));
    }

    ECSValue *children = NULL;
    SHU_ReturnResult(GltfSetTable(item, "children", &children));

    for (usz i = 0; i < node->children_count; i++)
    {
        SHU_ReturnResult(GltfSetInteger(children, NULL, (i64)cgltf_node_index(data, node->children[i]) + 1));
    }

    return SHUResult_Ok;
}

static SHUResult GltfDescribeModel(const cgltf_data *data, ECSValue *info)
{
    ECSValue *meshes = NULL;
    ECSValue *materials = NULL;
    ECSValue *nodes = NULL;
    ECSValue *roots = NULL;
    ECSValue *item = NULL;
    ECSValue_SetTable(info);
    SHU_ReturnResult(GltfSetTable(info, "meshes", &meshes));
    SHU_ReturnResult(GltfSetTable(info, "materials", &materials));
    SHU_ReturnResult(GltfSetTable(info, "nodes", &nodes));
    SHU_ReturnResult(GltfSetTable(info, "roots", &roots));

    for (usz i = 0; i < data->meshes_count; i++)
    {
        SHU_ReturnResult(GltfSetTable(meshes, NULL, &item));
        SHU_ReturnResult(GltfDescribeMesh(data, &data->meshes[i], item));
    }

    for (usz i = 0; i < data->materials_count; i++)
    {
        SHU_ReturnResult(GltfSetTable(materials, NULL, &item));
        SHU_ReturnResult(GltfDescribeMaterial(&data->materials[i], item));
    }

    for (usz i = 0; i < data->nodes_count; i++)
    {
        SHU_ReturnResult(GltfSetTable(nodes, NULL, &item));
        SHU_ReturnResult(GltfDescribeNode(data, &data->nodes[i], item));
    }

    // the nodes the scene starts from: the file's scene, or its first one
    const cgltf_scene *scene = data->scene != NULL ? data->scene : data->scenes_count > 0 ? &data->scenes[0] : NULL;

    for (usz i = 0; scene != NULL && i < scene->nodes_count; i++)
    {
        SHU_ReturnResult(GltfSetInteger(roots, NULL, (i64)cgltf_node_index(data, scene->nodes[i]) + 1));
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
        return NULL;
    }

    return &model->data->meshes[mesh - 1].primitives[primitive - 1];
}

/// @brief Keeps what the model gives, freeing what it gave last.
static SHUSlice GltfGive(GltfModel *model, void *data, usz size)
{
    SDL_free(model->given);
    model->given = data;
    return (SHUSlice){.data = data, .size = data == NULL ? 0 : size};
}

static SHUSlice GltfAttribute(GltfModel *model, i32 mesh, i32 primitive, const char *name)
{
    const cgltf_primitive *part = GltfFindPrimitive(model, mesh, primitive);
    const cgltf_accessor *accessor = NULL;

    for (usz i = 0; part != NULL && i < part->attributes_count; i++)
    {
        if (SDL_strcmp(part->attributes[i].name, name) == 0)
        {
            accessor = part->attributes[i].data;
        }
    }

    // sparse and normalized data are unpacked to plain floats
    usz count = accessor == NULL ? 0 : accessor->count * cgltf_num_components(accessor->type);
    f32 *floats = count == 0 ? NULL : SDL_malloc(count * sizeof(f32));

    if (floats != NULL && cgltf_accessor_unpack_floats(accessor, floats, count) != count)
    {
        SDL_free(floats);
        floats = NULL;
    }

    return GltfGive(model, floats, count * sizeof(f32));
}

static SHUSlice GltfIndices(GltfModel *model, i32 mesh, i32 primitive)
{
    const cgltf_primitive *part = GltfFindPrimitive(model, mesh, primitive);
    const cgltf_accessor *position = NULL;

    for (usz i = 0; part != NULL && i < part->attributes_count; i++)
    {
        if (part->attributes[i].type == cgltf_attribute_type_position)
        {
            position = part->attributes[i].data;
        }
    }

    // a primitive without indices draws its vertices in order
    usz count = part == NULL ? 0 : part->indices != NULL ? part->indices->count : position != NULL ? position->count : 0;
    u32 *indices = count == 0 ? NULL : SDL_malloc(count * sizeof(u32));

    for (usz i = 0; indices != NULL && i < count; i++)
    {
        indices[i] = (u32)(part->indices != NULL ? cgltf_accessor_read_index(part->indices, i) : i);
    }

    return GltfGive(model, indices, count * sizeof(u32));
}

static void GltfDestroy(void *object)
{
    GltfModel *model = object;
    cgltf_free(model->data);
    SDL_free(model->given);
    SDL_free(model);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    GLTF.plugin = plugin;
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, GLTF_NAME("model"), GltfDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {GLTF_NAME("load"), (ECSFunction)GltfLoad, "handle<gltf.model>(string path)", "Read a .gltf or .glb file with the files it uses; nothing if it cannot be read"},
        {GLTF_NAME("describe"), (ECSFunction)GltfDescribe, "void(handle<gltf.model> model, out value info)", "Give a model's meshes, materials, nodes and root nodes as a table"},
        {GLTF_NAME("attribute"), (ECSFunction)GltfAttribute, "buffer(handle<gltf.model> model, int mesh, int primitive, string name)", "Give a vertex attribute of a primitive, such as POSITION, as floats; kept until the next attribute or indices call on the model"},
        {GLTF_NAME("indices"), (ECSFunction)GltfIndices, "buffer(handle<gltf.model> model, int mesh, int primitive)", "Give the indices of a primitive as 32-bit numbers; kept until the next attribute or indices call on the model"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
    SDL_zero(GLTF);
}
