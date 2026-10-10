-- the image plugin reads a PNG and an SVG once, gives their sizes and pixels, and draws them stretched
return {
  preset = "presets/gallery.lua",
  run = function(test)
    local results = test.session().pluginState.gallery.state
    test.match(results.same, true, "the same file gives the same picture")
    test.match(results.missing, true, "a missing file gives nothing")
    test.match(results.greenSize, { 4, 4 }, "the PNG's size")
    test.match(results.squareSize, { 10, 10 }, "the SVG's size")
    test.match(results.greenPixel, 0xFF00FF00, "a pixel of the PNG")
    test.match(results.drawnGreen, 0xFF00FF00, "the PNG drawn")
    test.match(results.drawnSquare, 0xFF0000FF, "the SVG drawn")
  end,
}
