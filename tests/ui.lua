-- the ui plugin lays out elements, and calls their callbacks: a click on a button, a check box, a choice and a clickable row,
-- typing in a field, a key combination, and the wheel over a scroll element
return {
  preset = "presets/former.lua",
  run = function(test)
    local panel = test.rect(1)

    local function results()
      return test.session().pluginState.former.state
    end

    -- clicks the middle of an element
    local function click(id)
      local rect = results().rects[id]
      assert(rect, "no rectangle for " .. id)
      test.click(panel.x + rect.x + rect.width / 2, panel.y + rect.y + rect.height / 2)
    end

    -- a row lays out its children from the left, and a field in a column's row grows across
    local rects = results().rects
    assert(rects.check.x > rects.button.x + rects.button.width, "the check box is right of the button")
    assert(rects.choice.x > rects.check.x + rects.check.width, "the choice is right of the check box")
    assert(rects.list.y > rects.wide.y + rects.wide.height, "the list is below the fields")
    assert(rects.list.y + rects.list.height <= panel.height + 0.5, "the list ends inside the panel")

    click("button")
    click("button")
    test.match(results(), { clicks = 2 }, "two clicks on the button")

    click("check")
    test.match(results(), { on = true }, "a click on the check box")

    click("choice")
    test.match(results(), { choice = "two" }, "a click on the choice")

    -- a click gives the field the keys; text goes to it, and Return submits it
    click("field")
    test.key("A")
    test.text("abc")
    test.key("Backspace")
    test.match(results(), { typed = "ab", submitted = "" }, "typing in the field")
    test.key("Return")
    test.match(results(), { submitted = "ab" }, "Return in the field")

    -- a key element takes the next key combination
    click("key")
    test.key("Ctrl+Shift+J")
    test.match(results(), { key = "Ctrl+Shift+J" }, "a key combination")

    -- a clickable row, then the wheel scrolls the list, so the last row moves up
    click("row:1")
    test.match(results(), { rowClicks = 1 }, "a click on a row")

    local before = results().rects["row:30"].y
    local list = results().rects.list
    test.wheel(panel.x + list.x + 20, panel.y + list.y + 20, -3)
    local after = results().rects["row:30"].y
    assert(after < before, ("the last row moves up: %g, then %g"):format(before, after))
  end,
}
