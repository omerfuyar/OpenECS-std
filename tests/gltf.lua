-- the gltf plugin reads a model of one triangle and gives its parts, its positions and its indices
return {
  preset = "presets/viewer.lua",
  run = function(test)
    local results = test.session().pluginState.viewer.state
    local info = results.info
    test.match(results.missing, true, "a missing file gives nothing")
    test.match(info.meshes, { { name = "tri", primitives = { { mode = "triangles", material = 1, attributes = { "POSITION" }, vertices = 3, indices = 3 } } } }, "the meshes")
    test.match(info.materials, { { name = "red", color = { 1, 0, 0, 1 } } }, "the materials")
    test.match(info.roots, { 1 }, "the root nodes")
    test.match(info.nodes[1].children, { 2 }, "a node's children")
    test.match(info.nodes[2].mesh, 1, "a node's mesh")
    test.match({ info.nodes[1].matrix[13], info.nodes[1].matrix[14], info.nodes[1].matrix[15] }, { 1, 2, 3 }, "a node's translation")
    test.match(results.positions, { 0, 0, 0, 1, 0, 0, 0, 1, 0 }, "the positions")
    test.match(results.indices, { 0, 2, 1 }, "the indices")
    test.match(results.noAttribute, true, "a missing attribute gives nothing")
    test.match(results.noMesh, true, "a missing mesh gives nothing")
  end,
}
