-- a server and a client on this computer; a timer moves them on, because nothing waits; what happened is kept in the plugin state

local ecs = require("ecs")
local net = require("net")

local PORT = 47123
local results = { received = "" }
local server = net.listen(PORT)
local client = net.connect("127.0.0.1", PORT)
local accepted

results.listening = server ~= nil

local timer = ecs.timer.start(0.02, true, function()
  accepted = accepted or net.accept(server)
  results.status = net.status(client)

  if results.status == 1 and accepted and not results.sent then
    results.sent = net.send(client, "hello")
  end

  if accepted then
    results.received = results.received .. net.receive(accepted, 64)
  end
end)

ecs.plugin.onShutdown(function()
  timer:stop()
end)

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
