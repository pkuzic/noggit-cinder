-- Hello Mod: the open sample for Noggit Mods (docs/MODS.md).
-- Shows: a menu action that places a stock WMO at the cursor, writing a project file,
-- a save hook, a mod-local require, and what an undeclared call does.

local util = require("lib.util")

-- A WMO that ships with every 1.12 client (Elwynn farm house).
local DEFAULT_WMO = "World\\wmo\\Azeroth\\Buildings\\Human_Farm\\Farm.wmo"

mod.add_menu_action("Place a stock WMO at the cursor", function()
  local path = DEFAULT_WMO
  if not mod.game_file_exists(path) then
    path = mod.input("This client has no " .. DEFAULT_WMO .. ".\nWMO to place:", DEFAULT_WMO)
    if not path then return end
  end
  local pos = mod.cursor_pos()
  local uid = mod.add_wmo(path, pos, { scale = 1.0, rot = { x = 0, y = 0, z = 0 } })
  mod.log("placed " .. path .. " uid " .. uid .. " at " .. util.fmt_vec(pos))
  mod.message("Placed " .. path .. "\nuid " .. uid .. " at " .. util.fmt_vec(pos) .. "\n(Ctrl+Z undoes it)")
end)

mod.add_menu_action("Write hello.txt into the project", function()
  local text = "Hello from " .. mod.name .. " " .. mod.version .. "\n"
  if mod.has_map() then
    text = text .. "map " .. mod.map_id() .. ", camera " .. util.fmt_vec(mod.camera_pos()) .. "\n"
  end
  local written = mod.write_project_file("hello_mod/hello.txt", text)
  mod.message("Wrote " .. written)
end)

mod.add_menu_action("Show the selection", function()
  local sel = mod.selection()
  local lines = {}
  for i, o in ipairs(sel) do
    lines[#lines + 1] = o.kind .. " uid " .. o.uid .. "  " .. o.path .. "  " .. util.fmt_vec(o.pos)
  end
  mod.message(#lines == 0 and "Nothing selected." or table.concat(lines, "\n"))
end)

-- This mod did not declare "net": the call raises an error that names the missing permission.
mod.add_menu_action("Try an undeclared call (http_get)", function()
  mod.http_get("https://example.com/")
end)

mod.on_map_open(function()
  mod.log("map opened: " .. mod.map_id())
end)

mod.on_save(function()
  mod.log("map saved")
end)

mod.log("hello_mod ready")
