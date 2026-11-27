// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/mods/ModManager.hpp>
#include <noggit/Log.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include <algorithm>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace Noggit::Mods
{
  namespace
  {
    std::string lower(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
    }

    std::string join(std::vector<std::string> const& v, char const* sep)
    {
      std::string out;
      for (std::size_t i = 0; i < v.size(); ++i)
      {
        if (i) out += sep;
        out += v[i];
      }
      return out;
    }

    QString settingsKey(Mod const& mod)
    {
      return QString::fromStdString("mods/approval/" + mod.manifest.id);
    }

    QString approvalValue(Mod const& mod, char const* verdict)
    {
      return QString::fromStdString(std::string(verdict) + "|" + mod.manifest.version + "|" + mod.manifest.permissions.fingerprint());
    }

    // Reads a JSON array of strings; a non-array or a non-string element is an error.
    bool readStringArray(QJsonObject const& obj, char const* key, std::vector<std::string>& out, std::string& err)
    {
      if (!obj.contains(key))
        return true;
      if (!obj.value(key).isArray())
      {
        err = std::string("'") + key + "' must be an array of strings";
        return false;
      }
      for (auto const& v : obj.value(key).toArray())
      {
        if (!v.isString())
        {
          err = std::string("'") + key + "' must be an array of strings";
          return false;
        }
        out.push_back(v.toString().toStdString());
      }
      return true;
    }
  }

  // ---------------------------------------------------------------- permissions

  bool ModPermissions::hasHook(std::string const& name) const
  {
    return std::find(hooks.begin(), hooks.end(), name) != hooks.end();
  }

  std::string ModPermissions::fingerprint() const
  {
    auto sorted = [](std::vector<std::string> v) { std::sort(v.begin(), v.end()); return v; };
    std::stringstream ss;
    ss << "net=" << join(sorted(net), ",")
       << ";exec=" << join(sorted(exec), ",")
       << ";pwb=" << project_write_binary
       << ";menu=" << menu
       << ";hooks=" << join(sorted(hooks), ",");
    return ss.str();
  }

  std::string ModPermissions::describe() const
  {
    std::stringstream ss;
    ss << "- Place / remove models and read the map, camera and selection (every mod)\n";
    if (menu)
      ss << "- Add entries to the Mods menu\n";
    if (!net.empty())
      ss << "- Connect to the internet (HTTPS) ONLY to: " << join(net, ", ") << "\n";
    if (!exec.empty())
      ss << "- Run programs shipped inside its own folder: " << join(exec, ", ") << "\n";
    if (project_write_binary)
      ss << "- Write files (any content) inside the open project folder\n";
    if (!hooks.empty())
      ss << "- Run automatically when you: " << join(hooks, ", ") << "\n";
    return ss.str();
  }

  // ---------------------------------------------------------------- Mod

  std::string Mod::displayName() const
  {
    std::string n = manifest.name.empty() ? folder : manifest.name;
    if (!manifest.version.empty())
      n += " " + manifest.version;
    return n;
  }

  void Mod::addLog(std::string const& line)
  {
    log.push_back(line);
    while (log.size() > 200)
      log.pop_front();
    Log << "[mod " << (manifest.id.empty() ? folder : manifest.id) << "] " << line << std::endl;
  }

  // ---------------------------------------------------------------- manager

  ModManager& ModManager::instance()
  {
    // Deliberately leaked: Lua states must not be torn down during static destruction (after QApplication).
    static ModManager* manager = new ModManager();
    return *manager;
  }

  fs::path ModManager::modsRoot() const
  {
    return fs::path(QCoreApplication::applicationDirPath().toStdWString()) / "mods";
  }

  void ModManager::setProjectPath(fs::path const& path)
  {
    std::error_code ec;
    auto canon = path.empty() ? fs::path() : fs::weakly_canonical(path, ec);
    _project_path = ec ? path : canon;
  }

  glm::vec3 ModManager::cursorPos() const
  {
    return _cursor ? _cursor() : glm::vec3(0.f);
  }

  QWidget* ModManager::dialogParent() const
  {
    return _window ? _window : QApplication::activeWindow();
  }

  void ModManager::attachMapView(MapView* view, std::function<glm::vec3()> cursor)
  {
    _view = view;
    _cursor = std::move(cursor);
  }

  void ModManager::detachMapView(MapView* view)
  {
    if (_view == view)
    {
      _view = nullptr;
      _cursor = nullptr;
    }
  }

  void ModManager::ensureLoaded(QWidget* parent)
  {
    if (_loaded)
      return;
    _loaded = true;

    fs::path root = modsRoot();
    std::error_code ec;
    if (!fs::is_directory(root, ec))
    {
      Log << "[mods] no mods folder at " << root.string() << std::endl;
      return;
    }

    std::vector<fs::path> dirs;
    for (auto const& entry : fs::directory_iterator(root, ec))
    {
      if (entry.is_directory(ec) && fs::exists(entry.path() / "mod.json", ec))
        dirs.push_back(entry.path());
    }
    std::sort(dirs.begin(), dirs.end());

    for (auto const& dir : dirs)
    {
      try
      {
        loadOne(dir, parent);
      }
      catch (std::exception const& e)
      {
        // Belt and braces: nothing a mod does may stop Noggit.
        LogError << "[mods] unexpected error loading " << dir.string() << ": " << e.what() << std::endl;
      }
    }
    Log << "[mods] " << _mods.size() << " mod(s) found in " << root.string() << std::endl;
  }

  bool ModManager::readManifest(Mod& mod)
  {
    QFile file(QString::fromStdWString((mod.dir / "mod.json").wstring()));
    if (!file.open(QIODevice::ReadOnly))
    {
      mod.error = "cannot open mod.json";
      return false;
    }
    QJsonParseError perr;
    auto doc = QJsonDocument::fromJson(file.readAll(), &perr);
    if (doc.isNull() || !doc.isObject())
    {
      mod.error = "mod.json is not a JSON object: " + perr.errorString().toStdString();
      return false;
    }
    QJsonObject o = doc.object();
    auto& m = mod.manifest;
    m.id = o.value("id").toString(QString::fromStdString(mod.folder)).toStdString();
    m.name = o.value("name").toString(QString::fromStdString(m.id)).toStdString();
    m.version = o.value("version").toString().toStdString();
    m.author = o.value("author").toString().toStdString();
    m.description = o.value("description").toString().toStdString();
    m.api = o.value("noggit_api").toInt(API_VERSION);

    if (!std::regex_match(m.id, std::regex("[A-Za-z0-9_.-]{1,64}")))
    {
      mod.error = "'id' must be 1-64 characters of A-Z a-z 0-9 _ . -";
      return false;
    }
    if (m.version.empty())
    {
      mod.error = "'version' is required";
      return false;
    }
    if (m.api > API_VERSION || m.api < 1)
    {
      mod.error = "needs Noggit mod API " + std::to_string(m.api) + ", this Noggit has API " + std::to_string(API_VERSION);
      return false;
    }

    // entry: "main.lua" or ["a.lua", "b.lua"]
    if (o.value("entry").isString())
      m.entries.push_back(o.value("entry").toString().toStdString());
    else if (!readStringArray(o, "entry", m.entries, mod.error))
      return false;
    if (m.entries.empty())
      m.entries.push_back("main.lua");
    for (auto const& e : m.entries)
    {
      if (!isSafeRelativePath(e))
      {
        mod.error = "entry '" + e + "' must be a relative path inside the mod folder";
        return false;
      }
    }

    QJsonObject p = o.value("permissions").toObject();
    static const std::set<std::string> known = { "net", "exec", "project_write_binary", "menu", "hooks" };
    for (auto const& key : p.keys())
    {
      if (!known.count(key.toStdString()))
      {
        mod.error = "unknown permission '" + key.toStdString() + "' (known: net, exec, project_write_binary, menu, hooks)";
        return false;
      }
    }
    auto& perms = m.permissions;
    if (!readStringArray(p, "net", perms.net, mod.error)
      || !readStringArray(p, "exec", perms.exec, mod.error)
      || !readStringArray(p, "hooks", perms.hooks, mod.error))
      return false;
    perms.project_write_binary = p.value("project_write_binary").toBool(false);
    perms.menu = p.value("menu").toBool(false);

    for (auto& h : perms.net)
    {
      h = lower(h);
      if (!std::regex_match(h, std::regex("(\\*\\.)?[a-z0-9-]+(\\.[a-z0-9-]+)*")))
      {
        mod.error = "net host '" + h + "' is not a plain host name (no scheme, port or path)";
        return false;
      }
    }
    for (auto const& e : perms.exec)
    {
      if (!isSafeRelativePath(e))
      {
        mod.error = "exec '" + e + "' must be a relative path inside the mod folder";
        return false;
      }
    }
    for (auto const& h : perms.hooks)
    {
      if (h != "save" && h != "export" && h != "map_open")
      {
        mod.error = "unknown hook '" + h + "' (known: save, export, map_open)";
        return false;
      }
    }
    return true;
  }

  int ModManager::askApproval(Mod& mod, QWidget* parent, bool force)
  {
    QSettings settings;
    QString stored = settings.value(settingsKey(mod)).toString();
    if (!force)
    {
      if (stored == approvalValue(mod, "allow"))
        return 1;
      if (stored == approvalValue(mod, "deny"))
        return 0;
    }

    auto const& m = mod.manifest;
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("Noggit Mods - permissions");
    box.setText(QString::fromStdString("The mod \"" + m.name + "\" " + m.version
      + (m.author.empty() ? "" : " by " + m.author) + " wants to load."));
    box.setInformativeText(QString::fromStdString(
      (m.description.empty() ? "" : m.description + "\n\n")
      + "It asks for:\n" + m.permissions.describe()
      + "\nFolder: " + mod.dir.string()
      + "\n\nYour answer is remembered for this version."));
    auto allow = box.addButton("Allow", QMessageBox::AcceptRole);
    auto deny = box.addButton("Deny", QMessageBox::RejectRole);
    auto later = box.addButton("Not now", QMessageBox::DestructiveRole);
    box.setDefaultButton(deny);
    box.exec();

    if (box.clickedButton() == allow)
    {
      settings.setValue(settingsKey(mod), approvalValue(mod, "allow"));
      settings.sync();
      return 1;
    }
    if (box.clickedButton() == deny)
    {
      settings.setValue(settingsKey(mod), approvalValue(mod, "deny"));
      settings.sync();
      return 0;
    }
    (void)later;
    return -1;
  }

  void ModManager::loadOne(fs::path const& dir, QWidget* parent)
  {
    auto mod = std::make_unique<Mod>();
    std::error_code ec;
    mod->dir = fs::weakly_canonical(dir, ec);
    if (ec)
      mod->dir = dir;
    mod->folder = dir.filename().string();
    Mod& m = *mod;
    _mods.push_back(std::move(mod));

    if (!readManifest(m))
    {
      m.state = ModState::Failed;
      m.addLog("not loaded: " + m.error);
      return;
    }
    for (auto const& other : _mods)
    {
      if (other.get() != &m && other->manifest.id == m.manifest.id)
      {
        m.state = ModState::Failed;
        m.error = "another mod already uses the id '" + m.manifest.id + "'";
        m.addLog("not loaded: " + m.error);
        return;
      }
    }

    int verdict = askApproval(m, parent, false);
    if (verdict != 1)
    {
      m.state = ModState::NotApproved;
      m.error = verdict == 0 ? "permissions denied" : "not approved yet";
      m.addLog(m.error + " (Mods menu > " + m.displayName() + " > Review permissions)");
      return;
    }
    startMod(m);
  }

  void ModManager::startMod(Mod& m)
  {
    m.actions.clear();
    m.hooks.clear();
    m.modules.clear();
    m.lua.reset();
    m.lua = std::make_unique<sol::state>();

    bool ok = runGuarded(m, "load", [&]
    {
      registerModApi(m, *m.lua);
      for (auto const& entry : m.manifest.entries)
      {
        fs::path file = m.dir / fs::path(entry);
        if (!isInside(m.dir, file) || !fs::is_regular_file(file))
          throw std::runtime_error("entry file not found: " + entry);
        auto result = m.lua->safe_script_file(file.string(), sol::script_pass_on_error);
        if (!result.valid())
        {
          sol::error err = result;
          throw std::runtime_error(err.what());
        }
      }
    }, false);

    if (ok)
    {
      m.state = ModState::Loaded;
      m.error.clear();
      m.addLog("loaded (" + std::to_string(m.actions.size()) + " menu action(s))");
    }
    else
    {
      m.state = ModState::Failed;
      m.actions.clear();
      m.hooks.clear();
      m.modules.clear();
      m.lua.reset();
    }
  }

  bool ModManager::runGuarded(Mod& mod, std::string const& what, std::function<void()> const& fn, bool show_errors)
  {
    if (mod.busy)
    {
      if (show_errors)
        QMessageBox::information(dialogParent(), "Noggit Mods",
          QString::fromStdString(mod.displayName() + " is still busy, try again in a moment."));
      return false;
    }
    std::string error;
    mod.busy = true;
    try
    {
      fn();
    }
    catch (sol::error const& e)
    {
      error = e.what();
    }
    catch (std::exception const& e)
    {
      error = e.what();
    }
    catch (...)
    {
      error = "unknown error";
    }
    mod.busy = false;

    if (error.empty())
      return true;

    mod.error = what + ": " + error;
    mod.addLog("ERROR " + mod.error);
    if (show_errors)
    {
      QMessageBox::warning(dialogParent(), "Noggit Mods",
        QString::fromStdString(mod.displayName() + " - " + what + " failed:\n\n" + error));
    }
    return false;
  }

  void ModManager::fireHook(std::string const& name)
  {
    for (auto& mod : _mods)
    {
      if (mod->state != ModState::Loaded)
        continue;
      auto it = mod->hooks.find(name);
      if (it == mod->hooks.end())
        continue;
      // copy: a callback may register more callbacks
      auto callbacks = it->second;
      for (auto& cb : callbacks)
      {
        runGuarded(*mod, "on_" + name, [&]
        {
          auto r = cb();
          if (!r.valid())
          {
            sol::error err = r;
            throw std::runtime_error(err.what());
          }
        }, true);
      }
    }
  }

  void ModManager::attachMenuBar(QMenuBar* bar, QWidget* window)
  {
    _window = window;
    QObject::connect(window, &QObject::destroyed, [this, window] { if (_window == window) _window = nullptr; });
    auto menu = bar->addMenu("Mods");
    _menu = menu;
    QObject::connect(menu, &QObject::destroyed, [this, menu] { if (_menu == menu) _menu = nullptr; });
    QObject::connect(menu, &QMenu::aboutToShow, menu, [this, menu] { populateMenu(menu); });
    // Populate once now so the menu is not empty before its first opening (and for keyboard nav).
    populateMenu(menu);
  }

  Mod* ModManager::find(std::string const& id) const
  {
    for (auto const& mod : _mods)
      if (mod->manifest.id == id || mod->folder == id)
        return mod.get();
    return nullptr;
  }

  void ModManager::runAction(Mod& mod, std::size_t i)
  {
    if (mod.state != ModState::Loaded || i >= mod.actions.size())
      return;
    auto fn = mod.actions[i].fn;  // copy: the callback may add actions
    runGuarded(mod, "'" + mod.actions[i].label + "'", [&]
    {
      auto r = fn();
      if (!r.valid())
      {
        sol::error err = r;
        throw std::runtime_error(err.what());
      }
    }, true);
  }

  bool ModManager::triggerAction(std::string const& id, std::string const& label)
  {
    Mod* mod = find(id);
    if (!mod || mod->state != ModState::Loaded)
      return false;
    for (std::size_t i = 0; i < mod->actions.size(); ++i)
    {
      if (mod->actions[i].label == label)
      {
        runAction(*mod, i);
        return true;
      }
    }
    return false;
  }

  void ModManager::populateMenu(QMenu* menu)
  {
    menu->clear();

    if (_mods.empty())
    {
      auto none = menu->addAction("(no mods installed)");
      none->setEnabled(false);
    }

    for (auto& owned : _mods)
    {
      Mod* mod = owned.get();
      std::string label = mod->displayName();
      switch (mod->state)
      {
      case ModState::Loaded: break;
      case ModState::Failed: label += "  (failed)"; break;
      case ModState::NotApproved: label += "  (not allowed)"; break;
      }
      auto sub = menu->addMenu(QString::fromStdString(label));

      if (mod->state == ModState::Loaded)
      {
        for (std::size_t i = 0; i < mod->actions.size(); ++i)
        {
          auto act = sub->addAction(QString::fromStdString(mod->actions[i].label));
          QObject::connect(act, &QAction::triggered, act, [this, mod, i] { runAction(*mod, i); });
        }
        if (!mod->actions.empty())
          sub->addSeparator();
      }

      auto info = sub->addAction("Info, permissions and log...");
      QObject::connect(info, &QAction::triggered, info, [this, mod] { showInfo(*mod); });

      if (mod->state != ModState::Failed || mod->error == "permissions denied")
      {
        auto review = sub->addAction("Review permissions...");
        QObject::connect(review, &QAction::triggered, review, [this, mod]
        {
          if (mod->busy)
          {
            QMessageBox::information(dialogParent(), "Noggit Mods",
              QString::fromStdString(mod->displayName() + " is still busy, try again in a moment."));
            return;
          }
          int verdict = askApproval(*mod, dialogParent(), true);
          if (verdict == 1 && mod->state != ModState::Loaded)
          {
            startMod(*mod);
            if (mod->state != ModState::Loaded)
              showInfo(*mod);
          }
          else if (verdict == 0 && mod->state == ModState::Loaded)
          {
            // Stop it now: menu actions and hooks go away with its Lua state.
            mod->actions.clear();
            mod->hooks.clear();
            mod->modules.clear();
            mod->lua.reset();
            mod->state = ModState::NotApproved;
            mod->error = "permissions denied";
            mod->addLog("stopped: permissions revoked");
          }
        });
      }
    }

    menu->addSeparator();
    auto open = menu->addAction("Open mods folder");
    QObject::connect(open, &QAction::triggered, open, [this]
    {
      std::error_code ec;
      fs::create_directories(modsRoot(), ec);
      QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(modsRoot().wstring())));
    });
    auto note = menu->addAction("New mods load on the next Noggit start");
    note->setEnabled(false);
  }

  void ModManager::showInfo(Mod& mod)
  {
    auto const& m = mod.manifest;
    std::string state;
    switch (mod.state)
    {
    case ModState::Loaded: state = "loaded"; break;
    case ModState::Failed: state = "FAILED"; break;
    case ModState::NotApproved: state = "not allowed"; break;
    }
    std::stringstream ss;
    ss << "Id: " << m.id << "\nVersion: " << m.version << "\nAuthor: " << m.author
       << "\nFolder: " << mod.dir.string() << "\nState: " << state;
    if (!mod.error.empty())
      ss << "\nLast error: " << mod.error;
    ss << "\n\nPermissions:\n" << m.permissions.describe();

    QMessageBox box(dialogParent());
    box.setWindowTitle(QString::fromStdString("Noggit Mods - " + mod.displayName()));
    box.setText(QString::fromStdString(ss.str()));
    std::string log;
    for (auto const& l : mod.log)
      log += l + "\n";
    box.setDetailedText(QString::fromStdString(log.empty() ? "(empty log)" : log));
    box.exec();
  }
}
