// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// Noggit Mods: drop-in extensions loaded from <noggit exe dir>/mods/<folder>/mod.json.
// Every mod runs in its OWN Lua state (never the shared scripts/ brush state) and only gets
// the capabilities it declares in mod.json and the user approved. See docs/MODS.md.
#pragma once

#include <sol/sol.hpp>
#include <glm/vec3.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class MapView;
class QMenu;
class QMenuBar;
class QWidget;

namespace Noggit::Mods
{
  constexpr int API_VERSION = 1;

  struct ModPermissions
  {
    std::vector<std::string> net;        // allowed hosts (lower case; "*.example.com" = any sub-domain)
    std::vector<std::string> exec;       // helper exes, paths relative to the mod folder ("bin/tool.exe")
    bool project_write_binary = false;   // mod.write_project_file
    bool menu = false;                   // mod.add_menu_action
    std::vector<std::string> hooks;      // "save", "export", "map_open"

    bool hasHook(std::string const& name) const;
    std::string fingerprint() const;     // stable text; a change re-asks the user
    std::string describe() const;        // human readable, one permission per line
  };

  struct ModManifest
  {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    int api = API_VERSION;
    std::vector<std::string> entries;    // lua files relative to the mod folder, run in order
    ModPermissions permissions;
  };

  enum class ModState
  {
    Loaded,       // running
    Failed,       // mod.json or the lua raised an error; Noggit carries on without it
    NotApproved,  // the user denied (or postponed) the permissions
  };

  struct ModMenuAction
  {
    std::string label;
    sol::protected_function fn;
  };

  struct Mod
  {
    std::filesystem::path dir;           // canonical mod folder
    std::string folder;                  // folder name
    ModManifest manifest;
    ModState state = ModState::Failed;
    std::string error;
    std::unique_ptr<sol::state> lua;
    std::vector<ModMenuAction> actions;
    std::map<std::string, std::vector<sol::protected_function>> hooks;
    std::deque<std::string> log;
    std::set<std::filesystem::path> picked_files;  // files the user chose in mod.pick_file (readable)
    std::map<std::string, sol::table> modules;      // mod-local require() cache
    bool busy = false;

    std::string displayName() const;
    void addLog(std::string const& line);
  };

  class ModManager
  {
  public:
    static ModManager& instance();

    // First call scans <exe dir>/mods and loads every mod (asking for approvals). Later calls do nothing.
    void ensureLoaded(QWidget* parent);
    void setProjectPath(std::filesystem::path const& path);
    std::filesystem::path const& projectPath() const { return _project_path; }
    std::filesystem::path modsRoot() const;

    // Adds the "Mods" menu to a main window's menu bar (rebuilt every time it opens).
    void attachMenuBar(QMenuBar* bar, QWidget* window);

    void attachMapView(MapView* view, std::function<glm::vec3()> cursor);
    void detachMapView(MapView* view);
    MapView* mapView() const { return _view; }
    glm::vec3 cursorPos() const;
    QWidget* dialogParent() const;

    // Runs every loaded mod's callbacks for this hook ("save", "export", "map_open").
    void fireHook(std::string const& name);

    std::vector<std::unique_ptr<Mod>> const& mods() const { return _mods; }
    Mod* find(std::string const& id) const;

    // Runs a mod's menu action by its label, as clicking it would (used by the smoke-test hook).
    bool triggerAction(std::string const& id, std::string const& label);
    QMenu* menu() const { return _menu; }

  private:
    ModManager() = default;

    void loadOne(std::filesystem::path const& dir, QWidget* parent);
    bool readManifest(Mod& mod);
    int askApproval(Mod& mod, QWidget* parent, bool force);  // 1 allow, 0 deny, -1 not now
    void startMod(Mod& mod);
    void populateMenu(QMenu* menu);
    void runAction(Mod& mod, std::size_t index);
    void showInfo(Mod& mod);
    // Runs fn inside the mod's error barrier: a Lua/C++ error is logged and shown, never propagated.
    bool runGuarded(Mod& mod, std::string const& what, std::function<void()> const& fn, bool show_errors);

    std::vector<std::unique_ptr<Mod>> _mods;
    bool _loaded = false;
    std::filesystem::path _project_path;
    MapView* _view = nullptr;
    std::function<glm::vec3()> _cursor;
    QWidget* _window = nullptr;
    QMenu* _menu = nullptr;
  };

  // ModApi.cpp: builds the per-mod `mod` table (core API + permission-gated API).
  void registerModApi(Mod& mod, sol::state& lua);

  // Path helpers (ModApi.cpp), exposed for ModManager's manifest checks.
  bool isSafeRelativePath(std::string const& rel);
  bool isInside(std::filesystem::path const& root, std::filesystem::path const& candidate);
}
