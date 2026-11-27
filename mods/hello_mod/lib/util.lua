-- mod-local module: require("lib.util") loads <mod folder>/lib/util.lua
local util = {}

function util.fmt_vec(v)
  return string.format("(%.1f, %.1f, %.1f)", v.x, v.y, v.z)
end

return util
