---@type ecs.Manifest
return {
  name = "net",
  version = "0.1.0",
  api = 1,
  description = "TCP connections and servers that never block the program; a standard plugin",
  native = "libnet.so",
}
