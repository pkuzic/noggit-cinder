// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// The `mod` table every Noggit mod sees. Core calls work for every mod; the rest are gated by the
// permissions in mod.json - an undeclared call raises a Lua error that names the missing permission.
#include <noggit/mods/ModManager.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>
#include <noggit/MapView.h>
#include <noggit/ModelInstance.h>
#include <noggit/SceneObject.hpp>
#include <noggit/Selection.h>
#include <noggit/WMOInstance.h>
#include <noggit/World.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/object_paste_params.hpp>
#include <opengl/context.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QEventLoop>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace Noggit::Mods
{
  // ---------------------------------------------------------------- path helpers

  bool isSafeRelativePath(std::string const& rel)
  {
    if (rel.empty() || rel.find(':') != std::string::npos || rel.find('\0') != std::string::npos)
      return false;
    fs::path p(rel);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory())
      return false;
    for (auto const& part : p)
    {
      if (part == "..")
        return false;
    }
    return true;
  }

  bool isInside(fs::path const& root, fs::path const& candidate)
  {
    if (root.empty())
      return false;
    std::error_code ec1, ec2;
    fs::path r = fs::weakly_canonical(root, ec1);
    fs::path c = fs::weakly_canonical(candidate, ec2);
    if (ec1 || ec2)
      return false;
    auto norm = [](fs::path const& part)
    {
      std::wstring s = part.wstring();
#ifdef _WIN32
      std::transform(s.begin(), s.end(), s.begin(), ::towlower);
#endif
      return s;
    };
    auto ri = r.begin(), ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci)
    {
      if (ri->empty())  // trailing separator
        continue;
      if (ci == c.end() || norm(*ri) != norm(*ci))
        return false;
    }
    return true;
  }

  namespace
  {
    [[noreturn]] void fail(std::string const& fn, std::string const& msg)
    {
      throw std::runtime_error("mod." + fn + ": " + msg);
    }

    void needPermission(Mod const& mod, bool granted, std::string const& fn, std::string const& permission)
    {
      if (!granted)
        fail(fn, "not allowed - declare \"" + permission + "\" in the \"permissions\" of "
          + mod.manifest.id + "/mod.json (the user must approve it)");
    }

    MapView* needMap(std::string const& fn)
    {
      MapView* view = ModManager::instance().mapView();
      if (!view || !view->_world)
        fail(fn, "no map is open");
      return view;
    }

    glm::vec3 toVec3(sol::object const& o, std::string const& fn, char const* what)
    {
      if (o.get_type() != sol::type::table)
        fail(fn, std::string(what) + " must be a table {x=, y=, z=}");
      sol::table t = o.as<sol::table>();
      auto comp = [&](char const* key, int index) -> float
      {
        sol::optional<float> v = t[key];
        if (v) return *v;
        sol::optional<float> w = t[index];
        if (w) return *w;
        fail(fn, std::string(what) + " is missing '" + key + "'");
      };
      return glm::vec3(comp("x", 1), comp("y", 2), comp("z", 3));
    }

    sol::table fromVec3(sol::state_view lua, glm::vec3 const& v)
    {
      return lua.create_table_with("x", v.x, "y", v.y, "z", v.z);
    }

    std::string lower(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
    }

    bool hostAllowed(std::vector<std::string> const& allowed, std::string const& host)
    {
      for (auto const& entry : allowed)
      {
        if (entry == host)
          return true;
        if (entry.size() > 2 && entry[0] == '*' && entry[1] == '.')
        {
          std::string suffix = entry.substr(1);  // ".example.com"
          if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
        }
      }
      return false;
    }

    constexpr qint64 MAX_TRANSFER = 64ll * 1024 * 1024;

    struct WaitCursor
    {
      WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
      ~WaitCursor() { QApplication::restoreOverrideCursor(); }
    };

    sol::table httpRequest(Mod& mod, sol::state_view lua, std::string const& fn, std::string const& method
      , std::string const& url, std::string const* body, sol::optional<sol::table> opts)
    {
      auto const& perms = mod.manifest.permissions;
      needPermission(mod, !perms.net.empty(), fn, "net");

      QUrl qurl(QString::fromStdString(url), QUrl::StrictMode);
      if (!qurl.isValid() || qurl.host().isEmpty())
        fail(fn, "invalid URL '" + url + "'");
      std::string scheme = lower(qurl.scheme().toStdString());
      std::string host = lower(qurl.host().toStdString());
      if (!qurl.userInfo().isEmpty())
        fail(fn, "URLs with user:password@ are not allowed");
      if (!hostAllowed(perms.net, host))
        fail(fn, "host '" + host + "' is not in permissions.net of " + mod.manifest.id + "/mod.json");
      bool local = host == "localhost" || host == "127.0.0.1";
      if (scheme != "https" && !(scheme == "http" && local))
        fail(fn, "only https:// is allowed (http:// only for a declared localhost / 127.0.0.1)");

      int timeout_ms = 30000;
      QNetworkRequest req(qurl);
      req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
      req.setRawHeader("User-Agent", "Noggit-Mods/1");
      if (opts)
      {
        timeout_ms = std::clamp(opts->get_or("timeout", 30000), 1000, 300000);
        sol::optional<sol::table> headers = (*opts)["headers"];
        if (headers)
        {
          for (auto const& kv : *headers)
          {
            if (kv.first.get_type() == sol::type::string && kv.second.get_type() == sol::type::string)
              req.setRawHeader(QByteArray::fromStdString(kv.first.as<std::string>()), QByteArray::fromStdString(kv.second.as<std::string>()));
          }
        }
      }

      WaitCursor wait;
      QNetworkAccessManager nam;
      QNetworkReply* reply = nullptr;
      if (method == "POST")
      {
        if (!req.header(QNetworkRequest::ContentTypeHeader).isValid() && req.rawHeader("Content-Type").isEmpty())
          req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        reply = nam.post(req, QByteArray::fromStdString(*body));
      }
      else
        reply = nam.get(req);

      bool timed_out = false, too_big = false;
      QEventLoop loop;
      QTimer timer;
      timer.setSingleShot(true);
      QObject::connect(&timer, &QTimer::timeout, &loop, [&] { timed_out = true; reply->abort(); });
      QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 received, qint64)
      {
        if (received > MAX_TRANSFER) { too_big = true; reply->abort(); }
      });
      QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
      timer.start(timeout_ms);
      if (!reply->isFinished())
        loop.exec(QEventLoop::ExcludeUserInputEvents);
      timer.stop();

      sol::table result = lua.create_table();
      result["status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      QByteArray data = too_big ? QByteArray() : reply->readAll();
      result["body"] = std::string(data.constData(), static_cast<std::size_t>(data.size()));
      sol::table headers = lua.create_table();
      for (auto const& pair : reply->rawHeaderPairs())
        headers[lower(pair.first.toStdString())] = pair.second.toStdString();
      result["headers"] = headers;
      if (timed_out)
        result["error"] = "timeout";
      else if (too_big)
        result["error"] = "response larger than 64 MB";
      else if (reply->error() != QNetworkReply::NoError && result.get<int>("status") == 0)
        result["error"] = reply->errorString().toStdString();
      reply->deleteLater();
      mod.addLog(method + " " + host + " -> " + std::to_string(result.get<int>("status")));
      return result;
    }

    sol::table runHelper(Mod& mod, sol::state_view lua, std::string const& name
      , sol::optional<sol::table> args, sol::optional<sol::table> opts)
    {
      auto const& perms = mod.manifest.permissions;
      needPermission(mod, !perms.exec.empty(), "run_helper", "exec");

      auto norm = [](std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return lower(s); };
      bool declared = std::any_of(perms.exec.begin(), perms.exec.end(), [&](std::string const& e) { return norm(e) == norm(name); });
      if (!declared || !isSafeRelativePath(name))
        fail("run_helper", "'" + name + "' is not in permissions.exec of " + mod.manifest.id + "/mod.json");

      fs::path exe = mod.dir / fs::path(name);
      std::error_code ec;
      if (!isInside(mod.dir, exe) || !fs::is_regular_file(exe, ec))
        fail("run_helper", "helper '" + name + "' not found inside the mod folder");
      exe = fs::weakly_canonical(exe, ec);

      QStringList qargs;
      if (args)
      {
        for (std::size_t i = 1; i <= args->size(); ++i)
        {
          sol::object v = (*args)[i];
          if (v.get_type() == sol::type::string)
            qargs << QString::fromStdString(v.as<std::string>());
          else if (v.get_type() == sol::type::number)
            qargs << QString::number(v.as<double>(), 'g', 17);
          else
            fail("run_helper", "arguments must be strings or numbers");
        }
      }
      int timeout_ms = 120000;
      std::string stdin_data;
      if (opts)
      {
        timeout_ms = std::clamp(opts->get_or("timeout", 120000), 1000, 600000);
        stdin_data = opts->get_or("stdin", std::string());
      }

      WaitCursor wait;
      QProcess proc;
      proc.setProgram(QString::fromStdWString(exe.wstring()));  // no shell: the exe runs directly
      proc.setArguments(qargs);
      proc.setWorkingDirectory(QString::fromStdWString(mod.dir.wstring()));
      QByteArray out, err;
      bool timed_out = false;

      QEventLoop loop;
      QTimer timer;
      timer.setSingleShot(true);
      QObject::connect(&timer, &QTimer::timeout, &loop, [&] { timed_out = true; proc.kill(); });
      QObject::connect(&proc, &QProcess::readyReadStandardOutput, &loop, [&]
      {
        out += proc.readAllStandardOutput();
        if (out.size() > MAX_TRANSFER) proc.kill();
      });
      QObject::connect(&proc, &QProcess::readyReadStandardError, &loop, [&]
      {
        err += proc.readAllStandardError();
        if (err.size() > MAX_TRANSFER) proc.kill();
      });
      QObject::connect(&proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, &QEventLoop::quit);
      QObject::connect(&proc, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError e)
      {
        if (e == QProcess::FailedToStart) loop.quit();
      });

      proc.start(QIODevice::ReadWrite);
      if (!proc.waitForStarted(10000))
        fail("run_helper", "could not start '" + name + "': " + proc.errorString().toStdString());
      if (!stdin_data.empty())
        proc.write(QByteArray::fromStdString(stdin_data));
      proc.closeWriteChannel();
      timer.start(timeout_ms);
      if (proc.state() != QProcess::NotRunning)
        loop.exec(QEventLoop::ExcludeUserInputEvents);
      timer.stop();
      proc.waitForFinished(5000);
      out += proc.readAllStandardOutput();
      err += proc.readAllStandardError();

      sol::table result = lua.create_table();
      result["exit_code"] = proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
      result["stdout"] = std::string(out.constData(), static_cast<std::size_t>(out.size()));
      result["stderr"] = std::string(err.constData(), static_cast<std::size_t>(err.size()));
      result["timed_out"] = timed_out;
      mod.addLog("run_helper " + name + " -> exit " + std::to_string(result.get<int>("exit_code")));
      return result;
    }

    fs::path resolveReadable(Mod& mod, std::string const& path)
    {
      fs::path p(path);
      if (p.is_relative())
      {
        if (!isSafeRelativePath(path))
          fail("read_file", "relative paths may not contain '..' or ':'");
        p = mod.dir / p;
      }
      std::error_code ec;
      fs::path canon = fs::weakly_canonical(p, ec);
      if (ec)
        fail("read_file", "bad path '" + path + "'");
      auto const& project = ModManager::instance().projectPath();
      if (isInside(mod.dir, canon) || isInside(project, canon) || mod.picked_files.count(canon))
        return canon;
      fail("read_file", "'" + path + "' is outside the mod folder and the project folder (use mod.pick_file to let the user choose a file)");
    }

    std::string readAll(fs::path const& p, std::string const& fn)
    {
      std::ifstream in(p, std::ios::binary);
      if (!in)
        fail(fn, "cannot read " + p.string());
      std::stringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    std::string joinArgs(sol::state_view lua, sol::variadic_args va)
    {
      sol::protected_function tostring = lua["tostring"];
      std::string line;
      bool first = true;
      for (auto v : va)
      {
        if (!first) line += " ";
        first = false;
        auto r = tostring(v);
        line += r.valid() ? r.get<std::string>() : std::string("?");
      }
      return line;
    }

    // Undo-able object edit inside the MapView's GL context.
    template<typename Fn>
    auto worldEdit(MapView* view, int flags, Fn&& fn)
    {
      view->makeCurrent();
      OpenGL::context::scoped_setter const _(::gl, view->context());
      NOGGIT_ACTION_MGR->beginAction(view, flags);
      try
      {
        auto r = fn(view->_world.get());
        NOGGIT_ACTION_MGR->endAction();
        return r;
      }
      catch (...)
      {
        NOGGIT_ACTION_MGR->endAction();
        throw;
      }
    }
  }

  void registerModApi(Mod& mod, sol::state& lua)
  {
    Mod* m = &mod;
    auto const& perms = mod.manifest.permissions;

    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math, sol::lib::utf8, sol::lib::coroutine);
    // No io / os / package / debug. Base-library file loaders are removed; load() is text-only.
    lua["dofile"] = sol::lua_nil;
    lua["loadfile"] = sol::lua_nil;
    lua.script("do local _load = load; load = function(chunk, name, mode, env) return _load(chunk, name, 't', env) end end");

    sol::table api = lua.create_named_table("mod");
    api["api_version"] = API_VERSION;
    api["id"] = mod.manifest.id;
    api["name"] = mod.manifest.name;
    api["version"] = mod.manifest.version;
    api["dir"] = mod.dir.string();

    // --------------------------------------------------------- logging + dialogs (core)
    auto log = [m](sol::this_state s, sol::variadic_args va) { m->addLog(joinArgs(s, va)); };
    api.set_function("log", log);
    lua.set_function("print", log);

    api.set_function("message", [m](std::string const& text)
    {
      QMessageBox::information(ModManager::instance().dialogParent(), QString::fromStdString(m->manifest.name), QString::fromStdString(text));
    });
    api.set_function("ask", [m](std::string const& text) -> bool
    {
      return QMessageBox::question(ModManager::instance().dialogParent(), QString::fromStdString(m->manifest.name), QString::fromStdString(text))
        == QMessageBox::Yes;
    });
    api.set_function("input", [m](std::string const& prompt, sol::optional<std::string> def) -> sol::optional<std::string>
    {
      bool ok = false;
      QString v = QInputDialog::getText(ModManager::instance().dialogParent(), QString::fromStdString(m->manifest.name)
        , QString::fromStdString(prompt), QLineEdit::Normal, QString::fromStdString(def.value_or("")), &ok);
      if (!ok)
        return sol::nullopt;
      return v.toStdString();
    });
    api.set_function("pick_file", [m](sol::optional<std::string> title, sol::optional<std::string> filter) -> sol::optional<std::string>
    {
      auto const& project = ModManager::instance().projectPath();
      QString file = QFileDialog::getOpenFileName(ModManager::instance().dialogParent()
        , QString::fromStdString(title.value_or("Choose a file"))
        , QString::fromStdWString(project.wstring())
        , QString::fromStdString(filter.value_or("All files (*.*)")));
      if (file.isEmpty())
        return sol::nullopt;
      std::error_code ec;
      fs::path p = fs::weakly_canonical(fs::path(file.toStdWString()), ec);
      m->picked_files.insert(p);
      return p.string();
    });

    // --------------------------------------------------------- files (core, read-only)
    api.set_function("read_file", [m](std::string const& path) { return readAll(resolveReadable(*m, path), "read_file"); });
    api.set_function("file_exists", [m](std::string const& path) -> bool
    {
      fs::path p(path);
      if (p.is_relative() && isSafeRelativePath(path))
        p = m->dir / p;
      std::error_code ec;
      return fs::exists(p, ec);
    });
    api.set_function("game_file_exists", [](std::string const& path) -> bool
    {
      auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
      return client && client->exists(path);
    });
    api.set_function("project_path", []() -> sol::optional<std::string>
    {
      auto const& p = ModManager::instance().projectPath();
      if (p.empty())
        return sol::nullopt;
      return p.string();
    });

    // mod-local require: require("lib.util") -> <mod>/lib/util.lua
    lua.set_function("require", [m](sol::this_state s, std::string const& name) -> sol::object
    {
      sol::state_view lv(s);
      auto it = m->modules.find(name);
      if (it != m->modules.end())
        return it->second;
      std::string rel = name;
      std::replace(rel.begin(), rel.end(), '.', '/');
      rel += ".lua";
      if (!isSafeRelativePath(rel))
        fail("require", "bad module name '" + name + "'");
      fs::path file = m->dir / fs::path(rel);
      if (!isInside(m->dir, file) || !fs::is_regular_file(file))
        fail("require", "module '" + name + "' not found in the mod folder");
      m->modules[name] = lv.create_table();  // cycle guard: a circular require sees an empty table
      auto r = lv.safe_script_file(file.string(), sol::script_pass_on_error);
      if (!r.valid())
      {
        m->modules.erase(name);
        sol::error err = r;
        throw std::runtime_error(err.what());
      }
      sol::table value = r.get_type() == sol::type::table ? r.get<sol::table>() : lv.create_table();
      m->modules[name] = value;
      return value;
    });

    // --------------------------------------------------------- map / world (core)
    api.set_function("has_map", []() { auto v = ModManager::instance().mapView(); return v && v->_world; });
    api.set_function("map_id", []() { return needMap("map_id")->_world->getMapID(); });
    api.set_function("area_id", [](sol::object pos)
    {
      return needMap("area_id")->_world->getAreaID(toVec3(pos, "area_id", "pos"));
    });
    api.set_function("camera_pos", [](sol::this_state s) { return fromVec3(s, needMap("camera_pos")->_camera.position); });
    api.set_function("cursor_pos", [](sol::this_state s) { needMap("cursor_pos"); return fromVec3(s, ModManager::instance().cursorPos()); });

    auto readPlacement = [](sol::optional<sol::table> const& opts, float& scale, glm::vec3& rot, std::string const& fn)
    {
      scale = 1.f;
      rot = glm::vec3(0.f);
      if (!opts)
        return;
      scale = opts->get_or("scale", 1.f);
      sol::object r = (*opts)["rot"];
      if (r.valid() && r.get_type() != sol::type::lua_nil)
        rot = toVec3(r, fn, "opts.rot");
      if (!(scale > 0.f) || scale > SceneObject::max_scale())
        fail(fn, "opts.scale out of range");
    };

    api.set_function("add_wmo", [readPlacement](std::string const& path, sol::object pos, sol::optional<sol::table> opts) -> std::uint32_t
    {
      MapView* view = needMap("add_wmo");
      glm::vec3 p = toVec3(pos, "add_wmo", "pos");
      float scale; glm::vec3 rot;
      readPlacement(opts, scale, rot, "add_wmo");
      return worldEdit(view, Noggit::ActionFlags::eOBJECTS_ADDED, [&](World* world)
      {
        WMOInstance* inst = world->addWMOAndGetInstance(path, p, math::degrees::vec3(rot), scale, true);
        if (!inst)
          fail("add_wmo", "could not place " + path);
        return static_cast<std::uint32_t>(inst->uid);
      });
    });

    api.set_function("add_m2", [readPlacement](std::string const& path, sol::object pos, sol::optional<sol::table> opts) -> std::uint32_t
    {
      MapView* view = needMap("add_m2");
      glm::vec3 p = toVec3(pos, "add_m2", "pos");
      float scale; glm::vec3 rot;
      readPlacement(opts, scale, rot, "add_m2");
      return worldEdit(view, Noggit::ActionFlags::eOBJECTS_ADDED, [&](World* world)
      {
        Noggit::object_paste_params params;
        ModelInstance* inst = world->addM2AndGetInstance(path, p, scale, math::degrees::vec3(rot), &params, true, true);
        if (!inst)
          fail("add_m2", "could not place " + path);
        return static_cast<std::uint32_t>(inst->uid);
      });
    });

    api.set_function("delete_object", [](std::uint32_t uid) -> bool
    {
      MapView* view = needMap("delete_object");
      return worldEdit(view, Noggit::ActionFlags::eOBJECTS_REMOVED, [&](World* world)
      {
        if (!world->getObjectInstance(uid))
          return false;
        world->deleteInstance(static_cast<int>(uid), true);
        return true;
      });
    });

    auto describe = [](sol::state_view lv, SceneObject* obj)
    {
      sol::table t = lv.create_table();
      t["uid"] = obj->uid;
      t["kind"] = obj->which() == eWMO ? "wmo" : "m2";
      t["path"] = obj->instance_model() ? obj->instance_model()->file_key().stringRepr() : std::string();
      t["pos"] = fromVec3(lv, obj->pos);
      t["rot"] = fromVec3(lv, obj->dir);
      t["scale"] = obj->scale;
      return t;
    };

    api.set_function("selection", [describe](sol::this_state s)
    {
      sol::state_view lv(s);
      MapView* view = needMap("selection");
      sol::table list = lv.create_table();
      int i = 1;
      for (auto const& entry : view->_world->current_selection())
      {
        if (entry.index() != eEntry_Object)
          continue;
        SceneObject* obj = std::get<selected_object_type>(entry);
        if (obj)
          list[i++] = describe(lv, obj);
      }
      return list;
    });

    api.set_function("get_object", [describe](sol::this_state s, std::uint32_t uid) -> sol::object
    {
      MapView* view = needMap("get_object");
      SceneObject* obj = view->_world->getObjectInstance(uid);
      if (!obj)
        return sol::lua_nil;
      return describe(s, obj);
    });

    // --------------------------------------------------------- gated: menu + hooks
    api.set_function("add_menu_action", [m](std::string const& label, sol::protected_function fn)
    {
      needPermission(*m, m->manifest.permissions.menu, "add_menu_action", "menu");
      if (label.empty())
        fail("add_menu_action", "label must not be empty");
      m->actions.push_back({ label, fn });
    });

    for (std::string hook : { "save", "export", "map_open" })
    {
      api.set_function("on_" + hook, [m, hook](sol::protected_function fn)
      {
        needPermission(*m, m->manifest.permissions.hasHook(hook), "on_" + hook, "hooks: [\"" + hook + "\"]");
        m->hooks[hook].push_back(fn);
      });
    }

    // --------------------------------------------------------- gated: net
    api.set_function("http_get", [m](sol::this_state s, std::string const& url, sol::optional<sol::table> opts)
    {
      return httpRequest(*m, s, "http_get", "GET", url, nullptr, opts);
    });
    api.set_function("http_post", [m](sol::this_state s, std::string const& url, std::string const& body, sol::optional<sol::table> opts)
    {
      return httpRequest(*m, s, "http_post", "POST", url, &body, opts);
    });

    // --------------------------------------------------------- gated: exec
    api.set_function("run_helper", [m](sol::this_state s, std::string const& name, sol::optional<sol::table> args, sol::optional<sol::table> opts)
    {
      return runHelper(*m, s, name, args, opts);
    });

    // --------------------------------------------------------- gated: project_write_binary
    api.set_function("write_project_file", [m](std::string const& rel, std::string const& data) -> std::string
    {
      needPermission(*m, m->manifest.permissions.project_write_binary, "write_project_file", "project_write_binary");
      auto const& project = ModManager::instance().projectPath();
      if (project.empty())
        fail("write_project_file", "no project is open");
      if (!isSafeRelativePath(rel))
        fail("write_project_file", "'" + rel + "' must be a relative path without '..' or ':'");
      fs::path target = project / fs::path(rel);
      std::error_code ec;
      fs::create_directories(target.parent_path(), ec);
      if (!isInside(project, target))
        fail("write_project_file", "'" + rel + "' resolves outside the project folder");
      target = fs::weakly_canonical(target, ec);
      {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out)
          fail("write_project_file", "cannot write " + target.string());
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out)
          fail("write_project_file", "write failed for " + target.string());
      }
      m->addLog("wrote " + target.string() + " (" + std::to_string(data.size()) + " bytes)");
      return target.string();
    });
  }
}
