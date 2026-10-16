---@meta
-- The types of gltf's tables, for editors. gltf never runs this file: OpenECS --definitions adds it to gltf's definitions (OpenECS DESIGN 10.10).
-- Meshes, materials, nodes, images, animations and skins name each other by their numbers in the lists of gltf.Info, from 1.

---A model's parts, as gltf.describe gives them.
---@class gltf.Info
---@field meshes gltf.Mesh[]
---@field materials gltf.Material[]
---@field nodes gltf.Node[]
---@field roots integer[] The nodes that the model's scene starts from.
---@field images gltf.Image[]
---@field animations gltf.Animation[]
---@field skins gltf.Skin[]

---@class gltf.Mesh
---@field name? string
---@field primitives gltf.Primitive[]

---@class gltf.Primitive
---@field mode "triangles"|"triangleStrip"|"triangleFan"|"points"|"lines"|"lineLoop"|"lineStrip"
---@field material? integer
---@field attributes string[] The names of its vertex attributes, such as POSITION.
---@field vertices integer
---@field indices integer

---@class gltf.Material
---@field name? string
---@field color number[] The base colour: red, green, blue and alpha, from 0 to 1.
---@field texture? string The base colour's image file, when it is a file.
---@field image? integer The base colour's image, which gltf.image reads.

---@class gltf.Node
---@field name? string
---@field mesh? integer
---@field skin? integer
---@field children integer[]
---@field matrix number[] 16 numbers, column by column, that place it in its parent.

---@class gltf.Image
---@field name? string
---@field mimeType? string Such as "image/png".
---@field file? string Its file, when it is not inside the model.

---@class gltf.Animation
---@field name? string
---@field duration number In seconds: the time of its last key.

---@class gltf.Skin
---@field name? string
---@field joints integer[] The nodes whose moves bend the meshes it skins.
