---@meta
-- The types of gpu's descriptions, for editors. gpu never runs this file: OpenECS --definitions adds it to gpu's definitions,
-- and the value<...> types of gpu's signatures name these classes (OpenECS DESIGN 10.10).

-- the handle types that these descriptions name; gpu's definitions declare them with their methods too
---@class gpu.shader
---@class gpu.buffer
---@class gpu.texture

---A shader: compiled code for one stage. The GPU's driver takes one kind of code, SPIR-V for Vulkan, DXIL for Direct3D 12 or MSL for Metal;
---gpu uses the first one given that it takes, so a plugin gives the kinds of the systems it runs on.
---@class gpu.ShaderDesc
---@field stage "vertex"|"fragment"
---@field spirv? string SPIR-V bytes. Uniform blocks are in set 1 for vertex shaders and set 3 for fragment shaders, and samplers in set 2, as SDL's GPU API sets them.
---@field dxil? string DXIL bytes.
---@field msl? string MSL source.
---@field entry? string The function the stage starts in; "main" if missing.
---@field uniforms? integer How many uniform blocks it reads; 0 if missing. A draw gives the first one.
---@field samplers? integer How many textures it samples; 0 if missing. A draw gives the first one.

---A pipeline: two shaders, and how vertices and primitives are drawn.
---@class gpu.PipelineDesc
---@field vertex gpu.shader
---@field fragment gpu.shader
---@field attributes ("float"|"float2"|"float3"|"float4"|"color")[] The attributes of a vertex, in order, at locations from 0; a color is 4 bytes, 0 to 255.
---@field primitive? "triangles"|"triangleStrip"|"lines"|"lineStrip"|"points" "triangles" if missing.
---@field depth? boolean Whether its primitives test and write the frame's depth, so nearer ones cover farther ones; true if missing.
---@field cull? "none"|"front"|"back" Which triangles it leaves out; front faces are counter-clockwise. "none" if missing.
---@field blend? "none"|"alpha"|"add" How its colours mix with what is under them; "none" if missing.

---What a draw draws.
---@class gpu.Draw
---@field vertices gpu.buffer A vertex buffer.
---@field indices? gpu.buffer An index buffer; without one, the vertices are drawn in order.
---@field count? integer How many indices, or vertices without indices, it draws; all if missing.
---@field vertexUniforms? string The vertex shader's first uniform block, as std140 lays it out, such as from string.pack.
---@field fragmentUniforms? string The fragment shader's first uniform block.
---@field texture? gpu.texture The fragment shader's first sampler.
---@field atlas? integer A texture of the text's glyphs as the fragment shader's first sampler, by the number gpu.glyphs gave, if there is no texture.

---The glyphs of a text, which a plugin draws with a pipeline of its own.
---@class gpu.Glyphs
---@field width number The text's width in pixels.
---@field height number The height of its line in pixels.
---@field parts gpu.GlyphPart[] One for each texture that holds some of its glyphs.

---The glyphs of a text that are in one texture of the text engine. The texture is white, with each glyph's coverage as its alpha, not premultiplied.
---@class gpu.GlyphPart
---@field atlas integer The texture's number, which a draw's atlas takes.
---@field vertices string Four floats for each vertex: x and y in pixels from the line's top left, y going down, then u and v in the texture.
---@field indices string 32-bit indices, three for each triangle, two triangles for each glyph.
---@field count integer How many indices it has.
