// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/everwood/EverwoodSmoke.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/everwood/EverwoodLayer.hpp>
#include <noggit/everwood/VoxelField.hpp>
#include <noggit/tools/EverwoodTool.hpp>
#include <noggit/tools/TexturingTool.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/rendering/WorldRender.hpp>
#include <noggit/TextureManager.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/Log.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/MapChunk.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/ui/tools/UiCommon/StackedWidget.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/projectCreation/NoggitProjectCreationDialog.h>
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
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
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

      {
        // An unattended window in the background must keep drawing at full rate.
        QSettings settings;
        settings.setValue("background_fps_limit", false);
        settings.setValue("updater/enabled", false);
        settings.setValue("uid_startup_check", false);
      }

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
      else if (runEverwood(cmd, a, line))
      {
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

    Noggit::EverwoodTool* everwoodTool()
    {
      if (!_view)
        return nullptr;
      for (auto& tool : _view->_tools)
        if (tool && tool->editingMode() == editing_mode::everwood)
          return static_cast<Noggit::EverwoodTool*>(tool.get());
      return nullptr;
    }

    Noggit::EverwoodTool* requireTool()
    {
      auto* tool = everwoodTool();
      if (!tool)
      {
        report("FAIL no Everwood tool in this map view");
        finish(8);
      }
      return tool;
    }

    void mouse(QEvent::Type type, int x, int y, Qt::MouseButton button, Qt::MouseButtons held)
    {
      QPointF const pos(x, y);
      QMouseEvent event(type, pos, pos, _view->mapToGlobal(pos.toPoint()), button, held, Qt::NoModifier);
      QApplication::sendEvent(_view, &event);
    }

    void expect(bool ok, std::string const& what)
    {
      report((ok ? "PASS " : "FAIL ") + what);
      if (!ok)
        _failed = true;
    }

    std::string hashText()
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(requireTool()->layer()->stateHash()));
      return buf;
    }

    bool runEverwood(QString const& cmd, QStringList const& a, QString const& line)
    {
      auto f = [&](int i) { return a.value(i).toFloat(); };

      if (cmd == "golden")
      {
        std::string text;
        bool const ok = Noggit::Everwood::Voxel::RunGolden(line.mid(cmd.size() + 1).trimmed().toStdString().c_str(), text);
        report(text);
        expect(ok, "golden vectors inside noggit.exe");
      }
      else if (cmd == "project_everwood")
      {
        _project->setEverwoodMode(a.value(1).toInt() != 0);
        report(std::string("project EverwoodMode set to ") + (_project->EverwoodMode ? "true" : "false"));
      }
      else if (cmd == "expect_project_everwood")
      {
        expect(_project->EverwoodMode == (a.value(1).toInt() != 0), "project EverwoodMode == " + a.value(1).toStdString());
      }
      else if (cmd == "expect_no_everwood")
      {
        expect(everwoodTool() == nullptr, "no Everwood tool, panel or toolbar entry");
      }
      else if (cmd == "tool")
      {
        requireTool();
        _view->set_editing_mode(editing_mode::everwood);
        report(std::string("active tool: ") + _view->activeTool()->name());
      }
      else if (cmd == "set")
      {
        expect(requireTool()->setOption(a.value(1), a.value(2)), "option " + a.value(1).toStdString() + " = " + a.value(2).toStdString());
      }
      else if (cmd == "move")
      {
        mouse(QEvent::MouseMove, a.value(1).toInt(), a.value(2).toInt(), Qt::NoButton, _held);
      }
      else if (cmd == "press")
      {
        mouse(QEvent::MouseMove, a.value(1).toInt(), a.value(2).toInt(), Qt::NoButton, Qt::NoButton);
        _held = Qt::LeftButton;
        mouse(QEvent::MouseButtonPress, a.value(1).toInt(), a.value(2).toInt(), Qt::LeftButton, _held);
      }
      else if (cmd == "release")
      {
        _held = Qt::NoButton;
        mouse(QEvent::MouseButtonRelease, a.value(1).toInt(), a.value(2).toInt(), Qt::LeftButton, _held);
      }
      else if (cmd == "undo")
      {
        requireTool()->undo();
      }
      else if (cmd == "redo")
      {
        requireTool()->redo();
      }
      else if (cmd == "edit")
      {
        // edit <mode> <x> <y> <z> <radius> <strength> <param>: one stroke of one exact edit
        Noggit::Everwood::Voxel::Edit e;
        e.mode = uint8_t(a.value(1).toInt());
        e.pos[0] = f(2);
        e.pos[1] = f(3);
        e.pos[2] = f(4);
        e.radius = f(5);
        e.strength = uint8_t(a.value(6).toInt());
        e.param = uint16_t(a.value(7).toInt(nullptr, 0));
        requireTool()->applyStroke({ e });
      }
      else if (cmd == "bridge")
      {
        // bridge <ax> <ay> <az> <bx> <by> <bz>: the two picks + Build
        auto* tool = requireTool();
        tool->cancelBridge();
        tool->setBridgePoint(glm::vec3(f(1), f(2), f(3)));
        tool->setBridgePoint(glm::vec3(f(4), f(5), f(6)));
        tool->confirmBridge();
      }
      else if (cmd == "ground")
      {
        float h = 0.f;
        bool const ok = requireTool()->layer()->rawGround(f(1), f(2), h);
        report("raw ground at " + a.value(1).toStdString() + " " + a.value(2).toStdString() + (ok ? " = " + std::to_string(h) : " = none"));
      }
      else if (cmd == "status")
      {
        auto* layer = requireTool()->layer();
        report("edits " + std::to_string(layer->liveEditCount()) + " strokes " + std::to_string(layer->strokeCount())
               + " active chunks " + std::to_string(layer->activeChunkCount()) + " hidden cells " + std::to_string(layer->hiddenCellCount())
               + " hash " + hashText());
      }
      else if (cmd == "expect_edits")
      {
        size_t const n = requireTool()->layer()->liveEditCount();
        bool const atLeast = a.value(1).startsWith('>');
        size_t const want = a.value(1).mid(atLeast ? 1 : 0).toULongLong();
        expect(atLeast ? n > want : n == want, "live edits " + std::to_string(n) + " vs " + a.value(1).toStdString());
      }
      else if (cmd == "expect_hidden")
      {
        size_t const n = requireTool()->layer()->hiddenCellCount();
        bool const atLeast = a.value(1).startsWith('>');
        size_t const want = a.value(1).mid(atLeast ? 1 : 0).toULongLong();
        expect(atLeast ? n > want : n == want, "hidden terrain cells " + std::to_string(n) + " vs " + a.value(1).toStdString());
      }
      else if (cmd == "hash_save")
      {
        // hash_save <name>: remember the state hash in this run and in a file next to the script
        std::string const h = hashText();
        _hashes[a.value(1).toStdString()] = h;
        QFile file(QFileInfo(scriptPath()).absoluteDir().absoluteFilePath("hash_" + a.value(1) + ".txt"));
        if (file.open(QIODevice::WriteOnly))
          file.write(h.c_str());
        report("hash " + a.value(1).toStdString() + " = " + h);
      }
      else if (cmd == "hash_same" || cmd == "hash_differs")
      {
        std::string const h = hashText();
        std::string other;
        auto it = _hashes.find(a.value(1).toStdString());
        if (it != _hashes.end())
          other = it->second;
        else
        {
          QFile file(QFileInfo(scriptPath()).absoluteDir().absoluteFilePath("hash_" + a.value(1) + ".txt"));
          if (file.open(QIODevice::ReadOnly))
            other = file.readAll().toStdString();
        }
        bool const same = h == other;
        expect(cmd == "hash_same" ? same : !same, "state hash " + h + (cmd == "hash_same" ? " == " : " != ") + a.value(1).toStdString() + " " + other);
      }
      else if (cmd == "export")
      {
        std::string const path = requireTool()->exportEdits();
        expect(!path.empty(), "export written: " + path);
      }
      else if (cmd == "reseed")
      {
        requireTool()->layer()->reseed();
      }
      else if (cmd == "camera_last")
      {
        // camera_last <distance> <height over the ground> [side]: look at the last edit
        auto const* edit = requireTool()->layer()->lastEdit();
        if (!edit)
        {
          expect(false, "camera_last: no edit");
          return true;
        }
        float const dist = f(1), height = f(2), side = f(3);
        // Yaw 0 looks along Noggit +z (WoW -x), positive pitch looks down.
        _view->_camera.position = glm::vec3(ZEROPOINT - edit->pos[1] - side, height, ZEROPOINT - edit->pos[0] - dist);
        float const ground = requireTool()->layer()->rawGround(edit->pos[0], edit->pos[1], _scratch) ? _scratch : edit->pos[2];
        _view->_camera.position.y = ground + height;
        _view->_camera.yaw(math::degrees(std::atan2(side, dist) * 57.29578f));
        _view->_camera.pitch(math::degrees(std::atan2(height, std::sqrt(dist * dist + side * side)) * 57.29578f));
        _view->_camera_moved_since_last_draw = true;
        report("camera at edit " + std::to_string(edit->pos[0]) + " " + std::to_string(edit->pos[1]) + " " + std::to_string(edit->pos[2]));
      }
      else if (cmd == "settings_dialog_shot")
      {
        QString const path = line.mid(cmd.size() + 1).trimmed();
        QTimer::singleShot(600, [this, path]
        {
          if (QWidget* dialog = QApplication::activeModalWidget())
          {
            expect(dialog->grab().save(path), "dialog screenshot " + path.toStdString());
            static_cast<QDialog*>(dialog)->reject();
          }
          else
          {
            expect(false, "no modal dialog to grab");
          }
        });
        _window->showProjectSettings();
      }
      else if (cmd == "creation_dialog_shot")
      {
        ProjectInformation information;
        NoggitProjectCreationDialog dialog(information, _window);
        dialog.show();
        QApplication::processEvents();
        expect(dialog.grab().save(line.mid(cmd.size() + 1).trimmed()), "creation dialog screenshot");
        dialog.hide();
      }
      else if (cmd == "paint_tool")
      {
        // paint_tool <texture path> <radius> <pressure 0..1> <hardness 0..1>: Texture Paint, ready to paint
        _view->set_editing_mode(editing_mode::paint);
        Noggit::TexturingTool* tool = nullptr;
        for (auto& t : _view->_tools)
          if (t && t->editingMode() == editing_mode::paint)
            tool = static_cast<Noggit::TexturingTool*>(t.get());
        if (!tool)
        {
          expect(false, "no Texture Paint tool");
          return true;
        }
        Noggit::Ui::selected_texture::set(scoped_blp_texture_reference(a.value(1).toStdString(), _view->_context));
        tool->texturingPanel()->setRadius(f(2));
        tool->texturingPanel()->set_pressure(f(3));
        tool->texturingPanel()->setHardness(f(4));
        report(std::string("active tool: ") + _view->activeTool()->name());
      }
      else if (cmd == "everwood_tool")
      {
        _view->set_editing_mode(editing_mode::everwood);
      }
      else if (cmd == "shift")
      {
        _view->_smoke_hold_shift = a.value(1).toInt() != 0;
      }
      else if (cmd == "cursor")
      {
        // cursor [expect_voxel 0|1]: where the cursor is, WoW axes, and whether it sits on the voxel surface
        glm::vec3 const p = Noggit::Everwood::noggitToWow(_view->_cursor_pos);
        report("cursor " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z)
               + " on_voxel " + std::to_string(int(_view->_cursor_on_voxel_surface))
               + " under_map " + std::to_string(int(_view->getWorld()->isUnderMap(_view->_cursor_pos))));
        if (a.size() > 1)
          expect(_view->_cursor_on_voxel_surface == (a.value(1).toInt() != 0), "cursor on voxel == " + a.value(1).toStdString());
      }
      else if (cmd == "overlay")
      {
        // overlay shadows|mccv|lines 0|1
        auto* block = _view->getWorld()->renderer()->getTerrainParamsUniformBlock();
        int const on = a.value(2).toInt();

        if (a.value(1) == "shadows") block->draw_shadows = on;
        else if (a.value(1) == "mccv") block->draw_vertex_color = on;
        else if (a.value(1) == "lines") block->draw_lines = on;
        _view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
      }
      else if (cmd == "tint")
      {
        // tint <wow x> <wow y> <radius> <r> <g> <b>: vertex colours 0..2 on the ADT at a spot, as vertex paint does
        glm::vec3 const center = Noggit::Everwood::wowToNoggit(glm::vec3(f(1), f(2), 0.f));
        _view->makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, _view->context());
        _view->getWorld()->for_all_chunks_in_range(center, f(3), [&](MapChunk* chunk)
        {
          chunk->initMCCV();
          glm::vec3 const color(f(4), f(5), f(6));
          for (int i = 0; i < mapbufsize; ++i)
            chunk->mccv[i] = color;
          chunk->registerChunkUpdate(ChunkUpdateFlags::MCCV);
          return true;
        });
      }
      else if (cmd == "action_undo")
      {
        NOGGIT_ACTION_MGR->undo();
      }
      else if (cmd == "action_redo")
      {
        NOGGIT_ACTION_MGR->redo();
      }
      else if (cmd == "return_to_menu")
      {
        // What the exit prompt's "Return to menu" does: the whole map view goes, GL resources included.
        _window->_stack_widget->setCurrentIndex(0);
        _window->_stack_widget->removeLast();
        delete _window->_map_view;
        _window->_map_view = nullptr;
        _window->_minimap->world(nullptr);
        _window->map_loaded = false;
        _view = nullptr;
        report("returned to the map list");
      }
      else if (cmd == "objects")
      {
        // objects 0|1: draw WMOs and doodads, for an unobstructed view of the ground
        bool const on = a.value(1).toInt() != 0;
        _view->_draw_wmo.set(on);
        _view->_draw_models.set(on);
      }
      else if (cmd == "panel_shot")
      {
        QPixmap const pixmap = _window->grab();
        expect(pixmap.save(line.mid(cmd.size() + 1).trimmed()), "window screenshot " + line.mid(cmd.size() + 1).trimmed().toStdString());
      }
      else
      {
        return false;
      }
      return true;
    }

    void shot(QString const& path)
    {
      _view->_needs_redraw = true;
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
    Qt::MouseButtons _held = Qt::NoButton;
    float _scratch = 0.f;
    std::map<std::string, std::string> _hashes;

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
