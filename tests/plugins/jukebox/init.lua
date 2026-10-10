-- reads the plugin's own sound once and plays it; what happened is kept in the plugin state
-- a computer without an audio output plays nothing, so the voice is 0 there

local ecs = require("ecs")
local audio = require("audio")

local results = {}
local beep = audio.load(ecs.plugin.folder .. "beep.wav")
results.same = audio.load(ecs.plugin.folder .. "beep.wav") == beep
results.missing = audio.load("nope.wav") == nil
results.length = audio.length(beep)

local voice = audio.play(beep, 0.5, true)
results.played = voice == 0 or audio.playing(voice)
audio.stop(voice)
results.stopped = not audio.playing(voice)

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
