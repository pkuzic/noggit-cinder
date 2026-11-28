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
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
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
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
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

  // One in-flight mod.http_*_async request. Lives in Mod::http_jobs until its callback ran (or stopAsync dropped it), so
  // the Lua functions are always released while their Lua state is still alive.
  struct AsyncHttpJob
  {
    QPointer<QNetworkReply> reply;
    sol::protected_function callback;
    sol::protected_function progress;
    std::string method;
    std::string host;
    bool timed_out = false;
    bool too_big = false;
    bool done = false;
    // the finished result, kept as plain C++ until the callback runs (a Lua table would pin the state)
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;
    QElapsedTimer since_progress;
  };

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

    constexpr std::size_t MAX_ASYNC_JOBS = 8;

    // The checks every HTTP call makes (permission, URL, host, scheme) + the request with its headers and timeout.
    QNetworkRequest prepareRequest(Mod& mod, std::string const& fn, std::string const& url, sol::optional<sol::table>& opts
      , int& timeout_ms, std::string& host)
    {
      auto const& perms = mod.manifest.permissions;
      needPermission(mod, !perms.net.empty(), fn, "net");

      QUrl qurl(QString::fromStdString(url), QUrl::StrictMode);
      if (!qurl.isValid() || qurl.host().isEmpty())
        fail(fn, "invalid URL '" + url + "'");
      std::string scheme = lower(qurl.scheme().toStdString());
      host = lower(qurl.host().toStdString());
      if (!qurl.userInfo().isEmpty())
        fail(fn, "URLs with user:password@ are not allowed");
      if (!hostAllowed(perms.net, host))
        fail(fn, "host '" + host + "' is not in permissions.net of " + mod.manifest.id + "/mod.json");
      bool local = host == "localhost" || host == "127.0.0.1";
      if (scheme != "https" && !(scheme == "http" && local))
        fail(fn, "only https:// is allowed (http:// only for a declared localhost / 127.0.0.1)");

      timeout_ms = 30000;
      QNetworkRequest req(qurl);
      req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
      req.setRawHeader("User-Agent", "Noggit-Mods/" + QByteArray::number(API_VERSION));
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
      return req;
    }

    QNetworkAccessManager& sharedNetwork()
    {
      // one manager for every async request of every mod (deliberately leaked, like the ModManager)
      static QNetworkAccessManager* nam = new QNetworkAccessManager();
      return *nam;
    }

    QNetworkReply* sendRequest(QNetworkAccessManager& nam, QNetworkRequest req, std::string const& method, std::string const* body)
    {
      if (method == "POST")
      {
        if (!req.header(QNetworkRequest::ContentTypeHeader).isValid() && req.rawHeader("Content-Type").isEmpty())
          req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        return nam.post(req, QByteArray::fromStdString(*body));
      }
      return nam.get(req);
    }

    // Reads a finished reply into plain C++ (status, body, lower-case headers, error).
    void readReply(QNetworkReply* reply, bool timed_out, bool too_big, int& status, std::string& body
      , std::vector<std::pair<std::string, std::string>>& headers, std::string& error)
    {
      status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      QByteArray data = too_big ? QByteArray() : reply->readAll();
      body.assign(data.constData(), static_cast<std::size_t>(data.size()));
      headers.clear();
      for (auto const& pair : reply->rawHeaderPairs())
        headers.emplace_back(lower(pair.first.toStdString()), pair.second.toStdString());
      error.clear();
      if (timed_out)
        error = "timeout";
      else if (too_big)
        error = "response larger than 64 MB";
      else if (reply->error() != QNetworkReply::NoError && status == 0)
        error = reply->errorString().toStdString();
    }

    sol::table resultTable(sol::state_view lua, int status, std::string const& body
      , std::vector<std::pair<std::string, std::string>> const& headers, std::string const& error)
    {
      sol::table result = lua.create_table();
      result["status"] = status;
      result["body"] = body;
      sol::table h = lua.create_table();
      for (auto const& kv : headers)
        h[kv.first] = kv.second;
      result["headers"] = h;
      if (!error.empty())
        result["error"] = error;
      return result;
    }

    sol::table httpRequest(Mod& mod, sol::state_view lua, std::string const& fn, std::string const& method
      , std::string const& url, std::string const* body, sol::optional<sol::table> opts)
    {
      int timeout_ms = 30000;
      std::string host;
      QNetworkRequest req = prepareRequest(mod, fn, url, opts, timeout_ms, host);

      WaitCursor wait;
      QNetworkAccessManager nam;
      QNetworkReply* reply = sendRequest(nam, req, method, body);

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

      int status = 0;
      std::string data, error;
      std::vector<std::pair<std::string, std::string>> headers;
      readReply(reply, timed_out, too_big, status, data, headers, error);
      reply->deleteLater();
      mod.addLog(method + " " + host + " -> " + std::to_string(status));
      return resultTable(lua, status, data, headers, error);
    }

    // mod.http_get_async / mod.http_post_async: returns a handle at once; callback(result) runs later on the UI thread
    // (inside the mod's error barrier, after any open dialog of the mod closed). opts.progress(received, total) is
    // called at most every 250 ms while the body arrives (never while the mod is busy).
    int httpAsync(Mod& mod, std::string const& fn, std::string const& method, std::string const& url
      , std::string const* body, sol::optional<sol::table> opts, sol::protected_function callback)
    {
      if (!callback.valid())
        fail(fn, "a callback function is required");
      int timeout_ms = 30000;
      std::string host;
      QNetworkRequest req = prepareRequest(mod, fn, url, opts, timeout_ms, host);
      if (mod.http_jobs.size() >= MAX_ASYNC_JOBS)
        fail(fn, "too many requests in flight (" + std::to_string(MAX_ASYNC_JOBS) + "); wait for one to finish");

      auto job = std::make_shared<AsyncHttpJob>();
      job->callback = callback;
      job->method = method;
      job->host = host;
      if (opts)
      {
        sol::object p = (*opts)["progress"];
        if (p.get_type() == sol::type::function)
          job->progress = p.as<sol::protected_function>();
      }
      job->since_progress.start();

      QNetworkReply* reply = sendRequest(sharedNetwork(), req, method, body);
      job->reply = reply;
      int const handle = mod.next_handle++;
      mod.http_jobs[handle] = job;
      Mod* m = &mod;
      int const gen = mod.generation;

      auto* timer = new QTimer(reply);  // dies with the reply
      timer->setSingleShot(true);
      QObject::connect(timer, &QTimer::timeout, reply, [m, gen, handle]
      {
        auto it = m->http_jobs.find(handle);
        if (m->generation != gen || it == m->http_jobs.end() || !it->second->reply)
          return;
        it->second->timed_out = true;
        it->second->reply->abort();
      });
      timer->start(timeout_ms);

      QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [m, gen, handle](qint64 received, qint64 total)
      {
        auto it = m->http_jobs.find(handle);
        if (m->generation != gen || it == m->http_jobs.end())
          return;
        auto job = it->second;
        if (received > MAX_TRANSFER)
        {
          job->too_big = true;
          if (job->reply) job->reply->abort();
          return;
        }
        if (!job->progress.valid() || m->busy || m->state != ModState::Loaded)
          return;
        if (received != total && job->since_progress.elapsed() < 250)
          return;
        job->since_progress.restart();
        sol::protected_function progress = job->progress;
        auto r = progress(static_cast<double>(received), static_cast<double>(total));
        if (!r.valid())
        {
          sol::error err = r;
          m->addLog(std::string("ERROR progress callback: ") + err.what() + " (progress callbacks of this request stop)");
          job->progress = sol::protected_function();
        }
      });

      QObject::connect(reply, &QNetworkReply::finished, reply, [m, gen, handle, reply]
      {
        reply->deleteLater();
        auto it = m->http_jobs.find(handle);
        if (m->generation != gen || it == m->http_jobs.end())
          return;
        auto job = it->second;
        readReply(reply, job->timed_out, job->too_big, job->status, job->body, job->headers, job->error);
        job->reply = nullptr;
        job->done = true;
        m->addLog(job->method + " " + job->host + " -> " + std::to_string(job->status) + " (async)");
        ModManager::instance().runDeferred(*m, "http callback", [m, gen, handle]
        {
          auto jt = m->http_jobs.find(handle);
          if (m->generation != gen || jt == m->http_jobs.end() || !m->lua)
            return;
          auto done = jt->second;
          m->http_jobs.erase(jt);
          sol::protected_function cb = done->callback;
          auto r = cb(resultTable(*m->lua, done->status, done->body, done->headers, done->error));
          if (!r.valid())
          {
            sol::error err = r;
            throw std::runtime_error(err.what());
          }
        });
      });
      return handle;
    }

    std::string settingsKeyFor(Mod const& mod, std::string const& fn, std::string const& key)
    {
      static std::regex const re("[A-Za-z0-9_.-]{1,64}");
      if (!std::regex_match(key, re))
        fail(fn, "setting names are 1-64 characters of A-Z a-z 0-9 _ . -");
      return "mods/settings/" + mod.manifest.id + "/" + key;
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

  void stopAsync(Mod& mod)
  {
    ++mod.generation;  // late Qt signals and deferred callbacks of the old state see a new generation and do nothing
    for (auto& [handle, job] : mod.http_jobs)
    {
      if (job->reply)
      {
        QObject::disconnect(job->reply, nullptr, nullptr, nullptr);
        job->reply->abort();
        job->reply->deleteLater();
        job->reply = nullptr;
      }
    }
    mod.http_jobs.clear();  // releases the Lua callbacks while the state is still alive
    mod.timers.clear();
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
    api["noggit_version"] = std::string("Noggit Cinder (mods API ") + std::to_string(API_VERSION) + ", built " + __DATE__ + ")";
    api["RESERVED_UID_FIRST"] = RESERVED_UID_FIRST;
    api["RESERVED_UID_END"] = RESERVED_UID_END;

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
    api.set_function("input", [m](std::string const& prompt, sol::optional<std::string> def, sol::optional<sol::table> opts) -> sol::optional<std::string>
    {
      bool const secret = opts && opts->get_or("secret", false);
      bool ok = false;
      QString v = QInputDialog::getText(ModManager::instance().dialogParent(), QString::fromStdString(m->manifest.name)
        , QString::fromStdString(prompt), secret ? QLineEdit::Password : QLineEdit::Normal, QString::fromStdString(def.value_or("")), &ok);
      if (!ok)
        return sol::nullopt;
      return v.toStdString();
    });
    api.set_function("choose", [m](std::string const& prompt, sol::table items, sol::optional<int> current) -> sol::optional<std::string>
    {
      QStringList list;
      for (std::size_t i = 1; i <= items.size(); ++i)
      {
        sol::object v = items[i];
        if (v.get_type() != sol::type::string)
          fail("choose", "items must be a list of strings");
        list << QString::fromStdString(v.as<std::string>());
      }
      if (list.isEmpty())
        fail("choose", "items must not be empty");
      bool ok = false;
      int const index = std::clamp(current.value_or(1), 1, static_cast<int>(list.size())) - 1;
      QString v = QInputDialog::getItem(ModManager::instance().dialogParent(), QString::fromStdString(m->manifest.name)
        , QString::fromStdString(prompt), list, index, false, &ok);
      if (!ok)
        return sol::nullopt;
      return v.toStdString();
    });
    api.set_function("pick_file", [m](sol::optional<std::string> title, sol::optional<std::string> filter) -> sol::optional<std::string>
    {
      auto const& project = ModManager::instance().projectPath();
      fs::path scripted = ModManager::instance().takeNextPickedFile();  // smoke-test hook (no native dialog)
      if (!scripted.empty())
      {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(scripted, ec);
        m->picked_files.insert(p);
        return p.string();
      }
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

    // --------------------------------------------------------- per-mod settings (core)
    // Stored in Noggit's settings under mods/settings/<mod id>/<name>; values are never written to a log.
    api.set_function("settings_get", [m](sol::this_state s, std::string const& key, sol::object def) -> sol::object
    {
      QSettings settings;
      QVariant v = settings.value(QString::fromStdString(settingsKeyFor(*m, "settings_get", key)));
      if (!v.isValid())
        return def;
      std::string raw = v.toString().toStdString();
      if (raw.size() < 2 || raw[1] != ':')
        return def;
      std::string value = raw.substr(2);
      sol::state_view lv(s);
      switch (raw[0])
      {
      case 's': return sol::make_object(lv, value);
      case 'n': return sol::make_object(lv, std::strtod(value.c_str(), nullptr));
      case 'i': return sol::make_object(lv, static_cast<lua_Integer>(std::strtoll(value.c_str(), nullptr, 10)));
      case 'b': return sol::make_object(lv, value == "1");
      default: return def;
      }
    });
    api.set_function("settings_set", [m](std::string const& key, sol::object value)
    {
      QString const k = QString::fromStdString(settingsKeyFor(*m, "settings_set", key));
      QSettings settings;
      std::string stored;
      switch (value.get_type())
      {
      case sol::type::lua_nil:
      case sol::type::none:
        settings.remove(k);
        settings.sync();
        m->addLog("setting '" + key + "' cleared");
        return;
      case sol::type::string: stored = "s:" + value.as<std::string>(); break;
      case sol::type::boolean: stored = std::string("b:") + (value.as<bool>() ? "1" : "0"); break;
      case sol::type::number:
        if (double const d = value.as<double>(); d == std::floor(d) && std::fabs(d) < 9.0e15)
          stored = "i:" + std::to_string(static_cast<long long>(d));
        else
        {
          std::ostringstream ss;
          ss.precision(17);
          ss << value.as<double>();
          stored = "n:" + ss.str();
        }
        break;
      default:
        fail("settings_set", "a setting is a string, number, boolean or nil");
      }
      if (stored.size() > 65536)
        fail("settings_set", "a setting is at most 64 KB");
      settings.setValue(k, QString::fromStdString(stored));
      settings.sync();
      m->addLog("setting '" + key + "' saved");  // the value is never logged (it may be a token)
    });

    // --------------------------------------------------------- hashes, time, dev environment (core)
    api.set_function("hash", [](std::string const& algo, std::string const& data) -> std::string
    {
      QCryptographicHash::Algorithm a = QCryptographicHash::Sha256;
      std::string const l = lower(algo);
      if (l == "sha256") a = QCryptographicHash::Sha256;
      else if (l == "sha1") a = QCryptographicHash::Sha1;
      else if (l == "md5") a = QCryptographicHash::Md5;
      else fail("hash", "algorithm must be sha256, sha1 or md5");
      QByteArray h = QCryptographicHash::hash(QByteArray::fromRawData(data.data(), static_cast<int>(data.size())), a);
      return h.toHex().toStdString();
    });
    api.set_function("time", []() -> lua_Integer { return static_cast<lua_Integer>(QDateTime::currentSecsSinceEpoch()); });
    api.set_function("utc_iso", [](sol::optional<lua_Integer> secs) -> std::string
    {
      QDateTime t = secs ? QDateTime::fromSecsSinceEpoch(*secs, Qt::UTC) : QDateTime::currentDateTimeUtc();
      return t.toString("yyyy-MM-ddTHH:mm:ssZ").toStdString();
    });
    api.set_function("utc_stamp", [](sol::optional<lua_Integer> secs) -> std::string
    {
      QDateTime t = secs ? QDateTime::fromSecsSinceEpoch(*secs, Qt::UTC) : QDateTime::currentDateTimeUtc();
      return t.toString("yyyyMMddTHHmmss").toStdString();
    });
    // Only NOGGIT_MODS_DEV_* variables are readable: a development switch, never a general environment read.
    api.set_function("getenv", [](std::string const& name) -> sol::optional<std::string>
    {
      static std::regex const re("NOGGIT_MODS_DEV_[A-Z0-9_]{1,64}");
      if (!std::regex_match(name, re))
        fail("getenv", "only NOGGIT_MODS_DEV_* variables can be read");
      char const* v = std::getenv(name.c_str());
      if (!v)
        return sol::nullopt;
      return std::string(v);
    });

    // --------------------------------------------------------- timers (core)
    api.set_function("after", [m](int ms, sol::protected_function fn) -> int
    {
      if (!fn.valid())
        fail("after", "a function is required");
      if (m->timers.size() >= 64)
        fail("after", "too many timers pending (64)");
      int const handle = m->next_handle++;
      int const gen = m->generation;
      m->timers[handle] = fn;
      QTimer::singleShot(std::clamp(ms, 0, 3600000), [m, gen, handle]
      {
        if (m->generation != gen || !m->timers.count(handle))
          return;
        ModManager::instance().runDeferred(*m, "timer", [m, gen, handle]
        {
          auto it = m->timers.find(handle);
          if (m->generation != gen || it == m->timers.end())
            return;
          sol::protected_function f = it->second;
          m->timers.erase(it);
          auto r = f();
          if (!r.valid())
          {
            sol::error err = r;
            throw std::runtime_error(err.what());
          }
        });
      });
      return handle;
    });
    // Cancels a pending timer or async HTTP request; true when there was one.
    api.set_function("cancel", [m](int handle) -> bool
    {
      if (m->timers.erase(handle))
        return true;
      auto it = m->http_jobs.find(handle);
      if (it == m->http_jobs.end())
        return false;
      auto job = it->second;
      m->http_jobs.erase(it);
      if (job->reply)
      {
        QObject::disconnect(job->reply, nullptr, nullptr, nullptr);
        job->reply->abort();
        job->reply->deleteLater();
      }
      return true;
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
    api.set_function("map_info", [](sol::this_state s)
    {
      MapView* view = needMap("map_info");
      sol::state_view lv(s);
      return lv.create_table_with("id", view->_world->getMapID(), "directory", view->_world->basename);
    });
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
      std::optional<std::uint32_t> fixed_uid;
      if (opts)
      {
        sol::object u = (*opts)["uid"];
        if (u.valid() && u.get_type() != sol::type::lua_nil)
        {
          if (u.get_type() != sol::type::number || u.as<double>() != std::floor(u.as<double>()))
            fail("add_wmo", "opts.uid must be an integer");
          double const v = u.as<double>();
          if (v < static_cast<double>(RESERVED_UID_FIRST) || v >= static_cast<double>(RESERVED_UID_END))
            fail("add_wmo", "opts.uid must be inside the reserved block mod.RESERVED_UID_FIRST .. mod.RESERVED_UID_END - 1");
          fixed_uid = static_cast<std::uint32_t>(v);
          if (view->_world->getObjectInstance(*fixed_uid))
            fail("add_wmo", "uid " + std::to_string(*fixed_uid) + " is already used by another object");
        }
      }
      return worldEdit(view, Noggit::ActionFlags::eOBJECTS_ADDED, [&](World* world)
      {
        WMOInstance* inst = world->addWMOAndGetInstance(path, p, math::degrees::vec3(rot), scale, true, fixed_uid);
        if (!inst)
          fail("add_wmo", "could not place " + path);
        if (fixed_uid && inst->uid != *fixed_uid)
        {
          // the storage moved it to a new uid (the fixed one is taken by an object not loaded yet): take it back out
          world->deleteInstance(static_cast<int>(inst->uid), true);
          fail("add_wmo", "uid " + std::to_string(*fixed_uid) + " is already used on this map");
        }
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

    api.set_function("http_get_async", [m](std::string const& url, sol::optional<sol::table> opts, sol::protected_function cb)
    {
      return httpAsync(*m, "http_get_async", "GET", url, nullptr, opts, cb);
    });
    api.set_function("http_post_async", [m](std::string const& url, std::string const& body, sol::optional<sol::table> opts, sol::protected_function cb)
    {
      return httpAsync(*m, "http_post_async", "POST", url, &body, opts, cb);
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
