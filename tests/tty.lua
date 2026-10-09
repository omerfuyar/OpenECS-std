-- the tty plugin's grids print, wrap and scroll like a terminal, fit a panel at a font size, and draw their characters
return {
  preset = "presets/teletype.lua",
  run = function(test)
    local results = test.session().pluginState.teletype.state

    -- after "one\ntwo\n" the cursor is at the start of the third row
    test.match(results.afterTwo, { 0, 2 }, "the cursor after two lines")

    -- "three" fills row 3, the new line scrolls, and "four is long" wraps after 10 columns and scrolls again
    test.match(results.afterScroll, { 2, 2 }, "the cursor after scrolling")
    test.match(results.size, { 10, 3 }, "the grid's size")
    test.match(results.fits, true, "more cells fit the panel")
    test.match(results.cell, true, "a cell is taller than wide")
    test.match(results.inked, true, "the first cell's letter")
  end,
}
