-- the net plugin connects a client to a server on this computer, and a word goes from one to the other
return {
  preset = "presets/wire.lua",
  run = function(test)
    local results

    for _ = 1, 150 do
      test.wait(0.02)
      results = test.session().pluginState.wire.state

      if results.received == "hello" then
        break
      end
    end

    test.match(results, { listening = true, status = 1, sent = true, received = "hello" }, "the word arrived")
  end,
}
