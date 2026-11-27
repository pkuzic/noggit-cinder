# Noggit Mods

Noggit Cinder loads **mods**: drop a folder into `mods\` next to `noggit.exe` and start Noggit.
A mod is Lua plus a `mod.json` manifest. Each mod runs in its **own Lua state** (separate from the
`scripts\` brush scripts, which keep working exactly as before) and only gets the capabilities it
declares and the user approves.

```
noggit.exe
mods\
  hello_mod\
    mod.json
    main.lua
    lib\util.lua
```

## Loading

- At the first main window of a run (after the project is chosen) Noggit scans `mods\*\mod.json`
  in folder-name order. A folder without `mod.json` is ignored.
- The first time a mod (or a new **version** of it, or the same version with different permissions)
  is seen, Noggit shows its permissions and asks **Allow / Deny / Not now**. Allow and Deny are
  remembered in Noggit's settings (`mods/approval/<id>`); Not now asks again next start.
- A mod that fails (bad `mod.json`, a Lua error at load) is marked *failed*; Noggit carries on.
  Errors in menu actions and hooks are shown in a message box and logged; they never stop Noggit.
- New or changed mods load on the next Noggit start. **Mods > <mod> > Review permissions...** asks
  again at any time; Allow starts a not-yet-running mod at once, Deny stops a running one.
- Everything a mod logs goes to Noggit's `log.txt` as `[mod <id>] ...` and to
  **Mods > <mod> > Info, permissions and log...** (last 200 lines).

## mod.json

```json
{
  "id": "hello_mod",
  "name": "Hello Mod",
  "version": "1.0.0",
  "author": "Noggit Cinder",
  "description": "Shown in the permission prompt.",
  "noggit_api": 1,
  "entry": "main.lua",
  "permissions": {
    "menu": true,
    "project_write_binary": true,
    "hooks": ["save", "export", "map_open"],
    "net": ["api.example.com", "*.cdn.example.com"],
    "exec": ["bin/helper.exe"]
  }
}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | no (folder name) | `A-Z a-z 0-9 _ . -`, 1-64 chars, unique. Approvals are stored per id. |
| `name`, `author`, `description` | no | Shown in the Mods menu and the prompt. |
| `version` | **yes** | Any string. A new version asks for approval again. |
| `noggit_api` | no (1) | The mod API the mod was written for. A higher number than this Noggit supports refuses to load. |
| `entry` | no (`main.lua`) | One file or an array of files, relative to the mod folder, run in order. |
| `permissions` | no | See below. An unknown permission key refuses to load (no silent typos). |

### Permissions

| Permission | Unlocks | Rules |
|---|---|---|
| `menu: true` | `mod.add_menu_action` | Entries go under **Mods > <mod name>**. |
| `hooks: [...]` | `mod.on_save`, `mod.on_export`, `mod.on_map_open` | Only the listed hooks. |
| `net: [hosts]` | `mod.http_get`, `mod.http_post` | **https only**; the URL host must be listed exactly, or match a `*.domain` entry (sub-domains only). `http://` is allowed only for a declared `localhost` / `127.0.0.1`. No `user:pass@` URLs. Redirects are **not** followed (you get the 3xx status). 64 MB response cap. |
| `exec: [paths]` | `mod.run_helper` | Only the listed files, which must lie inside the mod folder. Started directly (no shell), working directory = the mod folder. |
| `project_write_binary: true` | `mod.write_project_file` | Any bytes, only inside the open project folder; `..`, `:` and absolute paths are refused, the resolved path is checked again. |

Calling a gated function without its permission raises a Lua error such as
`mod.http_get: not allowed - declare "net" in the "permissions" of hello_mod/mod.json (the user must approve it)`.

## The Lua environment

Libraries: `base`, `table`, `string`, `math`, `utf8`, `coroutine`. **No** `io`, `os`, `package`,
`debug`; `dofile` / `loadfile` are removed and `load` only accepts text chunks.
`print(...)` writes to the mod log. `require("lib.util")` loads `<mod folder>/lib/util.lua` (cached).

Positions and rotations are plain tables `{x=, y=, z=}` (also accepted: `{x, y, z}`), in Noggit
world coordinates (y = up); rotations are degrees, as in the object editor.

## API (`mod` table), noggit_api 1

### Always available
| Call | Returns | Notes |
|---|---|---|
| `mod.id`, `mod.name`, `mod.version`, `mod.dir`, `mod.api_version` | values | `mod.dir` = absolute mod folder. |
| `mod.log(...)` / `print(...)` | - | |
| `mod.message(text)` | - | Information box. |
| `mod.ask(text)` | bool | Yes / No box. |
| `mod.input(prompt [, default])` | string or nil | nil = cancelled. |
| `mod.pick_file([title [, filter]])` | path or nil | Open-file dialog (starts in the project folder). The chosen file becomes readable by `mod.read_file`. Filter like `"Plans (*.json)"`. |
| `mod.read_file(path)` | string (bytes) | Relative = inside the mod folder. Absolute paths only inside the mod folder, the project folder, or a file the user picked. |
| `mod.file_exists(path)` | bool | Relative = inside the mod folder. |
| `mod.game_file_exists(path)` | bool | In the client data / project (e.g. `World\wmo\...\x.wmo`). |
| `mod.project_path()` | string or nil | The open project folder. |
| `mod.has_map()` | bool | Whether a map is open in the 3D editor. |
| `mod.map_id()` | int | Errors when no map is open (as do all calls below). |
| `mod.area_id(pos)` | int | |
| `mod.camera_pos()` / `mod.cursor_pos()` | `{x,y,z}` | The cursor is the last point under the mouse in the 3D view. |
| `mod.add_wmo(path, pos [, {scale=, rot=}])` | uid | One undo step (Ctrl+Z). |
| `mod.add_m2(path, pos [, {scale=, rot=}])` | uid | One undo step; random scale/rotation settings are ignored. |
| `mod.delete_object(uid)` | bool | false if no such object. One undo step. |
| `mod.selection()` | array of objects | Only models/WMOs (chunks are skipped). |
| `mod.get_object(uid)` | object or nil | |

An *object* is `{uid=, kind="wmo"|"m2", path=, pos={...}, rot={...}, scale=}`.

### Gated
| Call | Permission | Returns |
|---|---|---|
| `mod.add_menu_action(label, fn)` | `menu` | - |
| `mod.on_save(fn)` | `hooks: ["save"]` | - , runs after the map tiles were saved successfully. |
| `mod.on_export(fn)` | `hooks: ["export"]` | - , runs when **Patch Client** is chosen, *before* its dialog - write your project files here so they are packed. |
| `mod.on_map_open(fn)` | `hooks: ["map_open"]` | - , runs once the 3D editor for a map is up. |
| `mod.http_get(url [, opts])` | `net` | `{status=, body=, headers={lowercase=value}, error=?}` |
| `mod.http_post(url, body [, opts])` | `net` | same; default `Content-Type: application/octet-stream` |
| `mod.run_helper(path [, args [, opts]])` | `exec` | `{exit_code=, stdout=, stderr=, timed_out=}` (`exit_code` -1 = crashed/killed) |
| `mod.write_project_file(rel_path, bytes)` | `project_write_binary` | absolute path written (folders are created) |

`opts` for HTTP: `timeout` (ms, default 30000, 1000-300000), `headers` (table). A network failure is
not a Lua error: `status` is 0 and `error` says why. For `run_helper`: `timeout` (ms, default
120000, max 600000; the helper is killed), `stdin` (string). Arguments are strings or numbers.

HTTP and helpers block the mod (Noggit keeps repainting, user input waits). While a mod is busy its
other actions/hooks are refused with a "still busy" message.

## Writing a mod, step by step

1. Make `mods\my_mod\mod.json` (copy `hello_mod`) and `main.lua`.
2. Declare only what you need; every permission is shown to the user.
3. Register menu actions / hooks at load time; do the work inside them (a map may not be open yet
   at load time - check `mod.has_map()`).
4. Start Noggit, open a project, allow the mod, and use **Mods > My Mod**.
5. Errors show up in a box and in **Info, permissions and log...**.

## Sample: hello_mod

`mods\hello_mod` (copied next to `noggit.exe` by the build) adds:
- *Place a stock WMO at the cursor* - `World\wmo\Azeroth\Buildings\Human_Farm\Farm.wmo` (asks for
  another path if the client lacks it), undoable.
- *Write hello.txt into the project* - `<project>\hello_mod\hello.txt`.
- *Show the selection*.
- *Try an undeclared call (http_get)* - shows the permission error on purpose.
- `on_map_open` / `on_save` log lines.
