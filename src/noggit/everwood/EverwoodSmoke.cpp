// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/everwood/EverwoodSmoke.hpp>

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/Log.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <opengl/context.hpp>
#include <opengl/context.inl>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QObject>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace Noggit::Everwood
{
  namespace
  {
    QString scriptPath()
    {
      return QString::fromLocal8Bit(qgetenv("NOGGIT_EVERWOOD_SMOKE"));
    }

    using clock = std::chrono::steady_clock;
  }

  bool smokeEnabled()
  {
    return !scriptPath().isEmpty();
  }

  void smokePrepareSettings()
  {
    if (!smokeEnabled())
      return;

    QDir const dir = QFileInfo(scriptPath()).absoluteDir();
    dir.mkpath("settings");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.absoluteFilePath("settings"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.absoluteFilePath("settings"));
  }

  class SmokeRunner : public QObject
  {
  public:
    explicit SmokeRunner(Noggit::Application::NoggitApplication* application)
      : _application(application)
      , _results(QFileInfo(scriptPath()).absoluteDir().absoluteFilePath(
            QFileInfo(scriptPath()).completeBaseName() + "_result.txt").toStdString())
    {
      QFile::remove(QString::fromStdString(_results));

      QFile script(scriptPath());
      if (!script.open(QIODevice::ReadOnly | QIODevice::Text))
      {
        report("FAIL cannot open script " + scriptPath().toStdString());
        finish(2);
      }
      QTextStream in(&script);
      while (!in.atEnd())
      {
        QString const line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith('#'))
          _lines.push_back(line);
      }

      connect(&_timer, &QTimer::timeout, this, [this] { step(); });
      _timer.start(20);
    }

  private:
    void report(std::string const& text)
    {
      Log << "[everwood-smoke] " << text << std::endl;
      if (std::FILE* f = std::fopen(_results.c_str(), "a"))
      {
        std::fprintf(f, "%s\n", text.c_str());
        std::fclose(f);
      }
    }

    [[noreturn]] void finish(int code)
    {
      report(code == 0 && !_failed ? "RESULT PASS" : "RESULT FAIL");
      // Ends here on purpose: a smoke run has nothing to save, and a normal shutdown would ask
      // about unsaved work on a window nobody is watching.
      std::fflush(nullptr);
      std::_Exit(code != 0 ? code : (_failed ? 1 : 0));
    }

    void placeWindow()
    {
      if (!_window)
        return;
      // The second screen, without taking the focus from whoever is at the keyboard.
      _window->resize(_width, _height);
      _window->move(_left, _top);
    }

    void step()
    {
      if (_busy)
        return;
      _busy = true;

      if (clock::now() < _wait_until)
      {
        _busy = false;
        return;
      }

      if (_tile_wait_x >= 0)
      {
        if (!tileReady())
        {
          _busy = false;
          return;
        }
        _tile_wait_x = -1;
      }

      if (_next >= _lines.size())
      {
        finish(0);
      }

      QString const line = _lines[_next++];
      report("> " + line.toStdString());

      try
      {
        run(line);
      }
      catch (std::exception const& e)
      {
        report(std::string("FAIL exception: ") + e.what());
        finish(3);
      }

      _busy = false;
    }

    bool tileReady()
    {
      if (clock::now() > _tile_deadline)
      {
        report("FAIL tile did not load in time");
        finish(4);
      }
      World* world = _view ? _view->getWorld() : nullptr;
      if (!world)
        return false;
      TileIndex const tile(_tile_wait_x, _tile_wait_z);
      return world->mapIndex.tileLoaded(tile);
    }

    void run(QString const& line)
    {
      QStringList const a = line.split(' ', Qt::SkipEmptyParts);
      QString const& cmd = a[0];
      auto f = [&](int i) { return a.value(i).toFloat(); };

      if (cmd == "window")
      {
        _left = a.value(1).toInt();
        _top = a.value(2).toInt();
        _width = a.value(3).toInt();
        _height = a.value(4).toInt();
      }
      else if (cmd == "project")
      {
        openProject(line.mid(cmd.size() + 1).trimmed());
      }
      else if (cmd == "map")
      {
        _map_id = a.value(1).toInt();
      }
      else if (cmd == "enter")
      {
        // enter <wow x> <wow y> <wow z> <yaw deg> <pitch deg>
        enterMap(f(1), f(2), f(3), f(4), f(5));
      }
      else if (cmd == "camera")
      {
        // camera <wow x> <wow y> <wow z> <yaw deg> <pitch deg>
        _view->_camera.position = glm::vec3(ZEROPOINT - f(2), f(3), ZEROPOINT - f(1));
        _view->_camera.yaw(math::degrees(f(4)));
        _view->_camera.pitch(math::degrees(f(5)));
        _view->_camera_moved_since_last_draw = true;
      }
      else if (cmd == "wait")
      {
        _wait_until = clock::now() + std::chrono::milliseconds(a.value(1).toInt());
      }
      else if (cmd == "waittile")
      {
        _tile_wait_x = a.value(1).toInt();
        _tile_wait_z = a.value(2).toInt();
        _tile_deadline = clock::now() + std::chrono::seconds(120);
      }
      else if (cmd == "shot")
      {
        shot(line.mid(cmd.size() + 1).trimmed());
      }
      else if (cmd == "save_tile")
      {
        World* world = _view->getWorld();
        TileIndex const tile(a.value(1).toInt(), a.value(2).toInt());
        _view->makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, _view->context());
        world->mapIndex.saveTile(tile, world);
        report("saved tile");
      }
      else if (cmd == "report")
      {
        report(line.mid(cmd.size() + 1).toStdString());
      }
      else if (cmd == "exit")
      {
        finish(0);
      }
      else
      {
        report("FAIL unknown command " + cmd.toStdString());
        finish(5);
      }
    }

    void openProject(QString const& path)
    {
      auto service = Noggit::Project::ApplicationProject(_application->getConfiguration());
      _project = service.loadProject(path.toStdString());
      if (!_project)
      {
        report("FAIL project did not load: " + path.toStdString());
        finish(6);
      }
      Noggit::Application::NoggitApplication::instance()->setClientData(_project->ClientData);
      Noggit::Project::CurrentProject::initialize(_project.get());

      _window = new Noggit::Ui::Windows::NoggitWindow(_application->getConfiguration(), _project);
      _window->setAttribute(Qt::WA_ShowWithoutActivating, true);
      _window->setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
      placeWindow();
      _window->show();
      placeWindow();
    }

    void enterMap(float x, float y, float z, float yaw, float pitch)
    {
      _window->loadMap(_map_id);
      World* world = _window->getWorld();
      if (!world)
      {
        report("FAIL map did not load");
        finish(7);
      }
      if (!uid_storage::hasMaxUIDStored(world->getMapID()))
        uid_storage::saveMaxUID(world->getMapID(), 0x7FFFFF);

      glm::vec3 const pos(ZEROPOINT - y, z, ZEROPOINT - x);
      _window->check_uid_then_enter_map(pos, math::degrees(pitch), math::degrees(yaw), false);
      _view = _window->_map_view;
      placeWindow();
    }

    void shot(QString const& path)
    {
      QImage const image = _view->grabFramebuffer();
      if (image.isNull() || !image.save(path))
      {
        report("FAIL screenshot not written: " + path.toStdString());
        _failed = true;
        return;
      }
      report("screenshot " + path.toStdString());
    }

    Noggit::Application::NoggitApplication* _application;
    std::string _results;
    std::vector<QString> _lines;
    std::size_t _next = 0;
    QTimer _timer;
    bool _busy = false;
    bool _failed = false;

    std::shared_ptr<Noggit::Project::NoggitProject> _project;
    Noggit::Ui::Windows::NoggitWindow* _window = nullptr;
    MapView* _view = nullptr;
    int _map_id = 0;

    int _left = 2580, _top = 20, _width = 1500, _height = 950;

    clock::time_point _wait_until = clock::now();
    int _tile_wait_x = -1, _tile_wait_z = -1;
    clock::time_point _tile_deadline;
  };

  void smokeStart(Noggit::Application::NoggitApplication* application)
  {
    new SmokeRunner(application);
  }
}
