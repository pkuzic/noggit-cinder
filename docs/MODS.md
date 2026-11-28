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
  "noggit_api": 2,
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
| `noggit_api` | no (1) | The mod API the mod was written for (this Noggit: **2**). A higher number than this Noggit supports refuses to load. API 2 only ADDS calls to API 1 (marked *API 2* below). |
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

## API (`mod` table), noggit_api 2

### Always available
| Call | Returns | Notes |
|---|---|---|
| `mod.id`, `mod.name`, `mod.version`, `mod.dir`, `mod.api_version` | values | `mod.dir` = absolute mod folder. |
| `mod.noggit_version` | string | *API 2.* Noggit build text, for files a mod writes. |
| `mod.RESERVED_UID_FIRST`, `mod.RESERVED_UID_END` | int | *API 2.* The fixed-uid block `0x57F00000 .. 0x57FFFFFF` (see `add_wmo` `uid`). |
| `mod.log(...)` / `print(...)` | - | |
| `mod.message(text)` | - | Information box. |
| `mod.ask(text)` | bool | Yes / No box. |
| `mod.input(prompt [, default [, {secret=true}]])` | string or nil | nil = cancelled. *API 2:* `secret` hides the typing (password field). |
| `mod.choose(prompt, items [, current])` | string or nil | *API 2.* Pick one of a list of strings (`current` = 1-based preselection); nil = cancelled. |
| `mod.pick_file([title [, filter]])` | path or nil | Open-file dialog (starts in the project folder). The chosen file becomes readable by `mod.read_file`. Filter like `"Plans (*.json)"`. |
| `mod.read_file(path)` | string (bytes) | Relative = inside the mod folder. Absolute paths only inside the mod folder, the project folder, or a file the user picked. |
| `mod.file_exists(path)` | bool | Relative = inside the mod folder. |
| `mod.game_file_exists(path)` | bool | In the client data / project (e.g. `World\wmo\...\x.wmo`). |
| `mod.project_path()` | string or nil | The open project folder. |
| `mod.has_map()` | bool | Whether a map is open in the 3D editor. |
| `mod.map_id()` | int | Errors when no map is open (as do all calls below). |
| `mod.map_info()` | `{id=, directory=}` | *API 2.* `directory` = the map folder (`World\Maps\<directory>\`). |
| `mod.area_id(pos)` | int | |
| `mod.camera_pos()` / `mod.cursor_pos()` | `{x,y,z}` | The cursor is the last point under the mouse in the 3D view. |
| `mod.add_wmo(path, pos [, {scale=, rot=, uid=}])` | uid | One undo step (Ctrl+Z). *API 2:* `uid` = a FIXED uid inside the reserved block; refused when it is used on the map. Noggit never hands out a uid of that block itself, and the max-uid search and *fix all uids* leave uids in it alone, so the uid stays what the mod chose. |
| `mod.add_m2(path, pos [, {scale=, rot=}])` | uid | One undo step; random scale/rotation settings are ignored. |
| `mod.delete_object(uid)` | bool | false if no such object. One undo step. |
| `mod.selection()` | array of objects | Only models/WMOs (chunks are skipped). |
| `mod.get_object(uid)` | object or nil | |

An *object* is `{uid=, kind="wmo"|"m2", path=, pos={...}, rot={...}, scale=}`.

### Settings, hashes, time, timers (API 2, always available)
| Call | Returns | Notes |
|---|---|---|
| `mod.settings_get(name [, default])` | value or default | Per-mod settings in Noggit's settings (`mods/settings/<id>/<name>`); `name` is `A-Z a-z 0-9 _ . -`, 1-64. |
| `mod.settings_set(name, value)` | - | string, number, boolean; `nil` deletes. At most 64 KB. **Values are never written to any log** (only "setting 'name' saved"), so a mod may keep a personal token here. They are stored in plain text in Noggit's settings of this Windows user. |
| `mod.hash(algo, data)` | hex string | `sha256`, `sha1` or `md5` of a byte string. |
| `mod.time()` | int | Seconds since 1970 (UTC). |
| `mod.utc_iso([secs])` / `mod.utc_stamp([secs])` | string | `2026-11-28T12:00:00Z` / `20261128T120000`. |
| `mod.getenv(name)` | string or nil | Only `NOGGIT_MODS_DEV_*` names (a development switch, not general environment access). |
| `mod.after(ms, fn)` | handle | Calls `fn()` once after `ms` (0 - 3600000), on the UI thread, inside the mod's error barrier. At most 64 pending. |
| `mod.cancel(handle)` | bool | Cancels a pending `after` timer or async HTTP request. |

### Gated
| Call | Permission | Returns |
|---|---|---|
| `mod.add_menu_action(label, fn)` | `menu` | - |
| `mod.on_save(fn)` | `hooks: ["save"]` | - , runs after the map tiles were saved successfully. |
| `mod.on_export(fn)` | `hooks: ["export"]` | - , runs when **Patch Client** is chosen, *before* its dialog - write your project files here so they are packed. |
| `mod.on_map_open(fn)` | `hooks: ["map_open"]` | - , runs once the 3D editor for a map is up. |
| `mod.http_get(url [, opts])` | `net` | `{status=, body=, headers={lowercase=value}, error=?}` |
| `mod.http_post(url, body [, opts])` | `net` | same; default `Content-Type: application/octet-stream` |
| `mod.http_get_async(url, opts, callback)` | `net` | *API 2.* a handle at once; later `callback(result)` with the same result table. Noggit does not wait. |
| `mod.http_post_async(url, body, opts, callback)` | `net` | *API 2.* same, POST. |
| `mod.run_helper(path [, args [, opts]])` | `exec` | `{exit_code=, stdout=, stderr=, timed_out=}` (`exit_code` -1 = crashed/killed) |
| `mod.write_project_file(rel_path, bytes)` | `project_write_binary` | absolute path written (folders are created) |

`opts` for HTTP: `timeout` (ms, default 30000, 1000-300000), `headers` (table). A network failure is
not a Lua error: `status` is 0 and `error` says why. For `run_helper`: `timeout` (ms, default
120000, max 600000; the helper is killed), `stdin` (string). Arguments are strings or numbers.

`http_get` / `http_post` and helpers block the mod (Noggit keeps repainting, user input waits). While a mod is busy its
other actions/hooks are refused with a "still busy" message.

*API 2* async HTTP: same host / scheme / size rules (declared hosts only, https only, 64 MB, no redirects) and the same
`timeout` / `headers` opts, plus `opts.progress(received, total)` (called at most every 250 ms while the body arrives).
At most 8 requests per mod in flight. The callback and `after` timers run on the UI thread inside the mod's error barrier;
when the mod is busy (one of its own dialogs is open) they wait until it is free. Reviewing permissions (Deny), a load
error or the end of the mod aborts its requests and drops its timers - their callbacks never run.

## Writing a mod, step by step

1. Make `mods\my_mod\mod.json` (copy `hello_mod`) and `main.lua`.
2. Declare only what you need; every permission is shown to the user.
3. Register menu actions / hooks at load time; do the work inside them (a map may not be open yet
   at load time - check `mod.has_map()`).
4. Start Noggit, open a project, allow the mod, and use **Mods > My Mod**.
5. Errors show up in a box and in **Info, permissions and log...**.

## Smoke-test commands (EverwoodSmoke, `NOGGIT_EVERWOOD_SMOKE=<script>`)
`mod_action <id> <label>`, `mod_report <id>`, `mods_menu <png prefix>`, `modal <png> <button>`, and (API 2)
`modal_text <png> <text>` (the next `mod.input` / `mod.choose` dialog gets `<text>`, then OK; a secret field is never echoed),
`mod_pick <path>` (the next `mod.pick_file` answers it - no native dialog), `mod_hook <save|export|map_open>`,
`wait_mod_log <id> <timeout ms> <text>` (waits for a log line containing `<text>`, FAIL at the deadline).

## Sample: hello_mod

`mods\hello_mod` (copied next to `noggit.exe` by the build) adds:
- *Place a stock WMO at the cursor* - `World\wmo\Azeroth\Buildings\Human_Farm\Farm.wmo` (asks for
  another path if the client lacks it), undoable.
- *Write hello.txt into the project* - `<project>\hello_mod\hello.txt`.
- *Show the selection*.
- *Try an undeclared call (http_get)* - shows the permission error on purpose.
- `on_map_open` / `on_save` log lines.
