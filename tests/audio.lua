-- the audio plugin reads a WAV file once, gives its length, and plays and stops it
return {
  preset = "presets/jukebox.lua",
  run = function(test)
    local results = test.session().pluginState.jukebox.state
    test.match(results.same, true, "the same file gives the same sound")
    test.match(results.missing, true, "a missing file gives nothing")
    assert(math.abs(results.length - 0.5) < 0.001, "the sound's length: " .. tostring(results.length))
    test.match(results.played, true, "the voice plays, where there is an output")
    test.match(results.stopped, true, "the voice stops")
  end,
}
