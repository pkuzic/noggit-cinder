// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/everwood/EverwoodSmoke.hpp>

#include <noggit/Action.hpp>
#include <noggit/DBC.h>
#include <noggit/tools/AreaTool.hpp>
#include <noggit/ui/ZoneIDBrowser.h>
#include <noggit/ui/tools/MapCreationWizard/Ui/MapCreationWizard.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/WMOInstance.h>
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
#include <noggit/mods/ModManager.hpp>
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
#include <QtWidgets/QAbstractButton>
#include <QtCore/QPointer>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
namespace Noggit::Everwood { std::string profileText(bool reset); bool profileGet(char const* name, long& n, double& ms); }
#include <QtWidgets/QDialog>
#include <QtWidgets/QInputDialog>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <map>
#include <set>
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

      // Only the script drives this window. A click or key from the desktop (the owner at work, another
      // session's automation on the same screen) would start a real stroke between the scripted ones and
      // shift every undo / redo after it; the script's own events are sent, never spontaneous.
      qApp->installEventFilter(this);

      connect(&_timer, &QTimer::timeout, this, [this] { step(); });
      _timer.start(20);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
      if (!event->spontaneous())
        return QObject::eventFilter(watched, event);
      switch (event->type())
      {
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonDblClick:
      case QEvent::KeyPress:
      case QEvent::TabletPress:
      case QEvent::TouchBegin:
        ++_realPresses;
        return true;
      case QEvent::MouseButtonRelease:
      case QEvent::MouseMove:
      case QEvent::Wheel:
      case QEvent::KeyRelease:
      case QEvent::ShortcutOverride:
      case QEvent::TabletMove:
      case QEvent::TabletRelease:
      case QEvent::TouchUpdate:
      case QEvent::TouchEnd:
      case QEvent::ContextMenu:
        ++_realOther;
        return true;
      default:
        return QObject::eventFilter(watched, event);
      }
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
      if (_realPresses || _realOther)
        report("note: ignored real desktop input: " + std::to_string(_realPresses) + " presses, "
               + std::to_string(_realOther) + " other events");
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

      if (_modlog_wait)
      {
        // wait_mod_log: a mod log line containing the text, or FAIL at the deadline
        auto* mod = Noggit::Mods::ModManager::instance().find(_modlog_id);
        bool hit = false;
        if (mod)
          for (auto const& l : mod->log)
            if (l.find(_modlog_text) != std::string::npos)
              hit = true;
        if (!hit && clock::now() < _modlog_deadline)
        {
          _busy = false;
          return;
        }
        _modlog_wait = false;
        expect(hit, "mod " + _modlog_id + " logged '" + _modlog_text + "'");
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
      else if (cmd == "save_changed")
      {
        // what Save does: every tile that was changed, so an object spanning tiles is saved in all of them
        World* world = _view->getWorld();
        _view->makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, _view->context());
        world->mapIndex.saveChanged(world);
        report("saved changed tiles");
      }
      else if (cmd == "seed_journal")
      {
        // seed_journal <file> [MapDirectory]: before `map`, the project's voxel journal becomes a copy of <file>
        // (edits a previous session left behind). Default map directory: Azeroth.
        if (!_project)
        {
          report("FAIL seed_journal needs a project");
          finish(9);
        }
        QString const map = a.size() > 2 ? a.value(2) : QString("Azeroth");
        QDir const dir(QString::fromStdString(_project->ProjectPath) + "/everwood/voxels");
        dir.mkpath(".");
        QString const target = dir.absoluteFilePath(map + ".mvoxj");
        QFile::remove(target);
        expect(QFile::copy(a.value(1), target), "seed_journal " + a.value(1).toStdString() + " -> " + target.toStdString());
      }
      else if (cmd == "report")
      {
        report(line.mid(cmd.size() + 1).toStdString());
      }
      else if (cmd == "exit")
      {
        finish(0);
      }
      else if (cmd == "modal")
      {
        // modal <png> <button text>: the next modal dialog (e.g. a Noggit Mods permission prompt) is
        // saved to <png>, its text reported, and the named button clicked. Arm it BEFORE the step that opens it.
        _modals.push_back({ a.value(1), line.section(' ', 2).trimmed() });
        armModalWatcher();
      }
      else if (cmd == "mods_menu")
      {
        // mods_menu <png prefix>: the main window, the Mods menu and each mod's sub-menu, grabbed off-screen
        modsMenuShot(a.value(1));
      }
      else if (cmd == "mod_action")
      {
        // mod_action <mod id> <menu label>: runs it as a click would
        QString const id = a.value(1);
        QString const label = line.section(' ', 2).trimmed();
        expect(Noggit::Mods::ModManager::instance().triggerAction(id.toStdString(), label.toStdString())
          , "mod_action " + id.toStdString() + " '" + label.toStdString() + "'");
      }
      else if (cmd == "modal_text")
      {
        // modal_text <png> <text>: the next modal is a mod.input / mod.choose dialog: saved to <png>, <text> entered
        // (an input field, or the item of a list), then OK. Arm it BEFORE the step that opens it.
        _modals.push_back({ a.value(1), QString(), true, line.section(' ', 2) });
        armModalWatcher();
      }
      else if (cmd == "mod_pick")
      {
        // mod_pick <path>: the next mod.pick_file answers this path (no native file dialog)
        Noggit::Mods::ModManager::instance().setNextPickedFile(line.mid(cmd.size() + 1).trimmed().toStdWString());
      }
      else if (cmd == "mod_hook")
      {
        // mod_hook <save|export|map_open>: fires that hook for every mod, as Noggit would
        Noggit::Mods::ModManager::instance().fireHook(a.value(1).toStdString());
      }
      else if (cmd == "wait_mod_log")
      {
        // wait_mod_log <mod id> <timeout ms> <text>: waits for a log line of the mod that contains <text>
        _modlog_id = a.value(1).toStdString();
        _modlog_deadline = clock::now() + std::chrono::milliseconds(a.value(2).toInt());
        _modlog_text = line.section(' ', 3).toStdString();
        _modlog_wait = true;
      }
      else if (cmd == "mod_report")
      {
        // mod_report <mod id>: state + log lines of one mod into the result file
        auto* mod = Noggit::Mods::ModManager::instance().find(a.value(1).toStdString());
        if (!mod)
        {
          expect(false, "mod " + a.value(1).toStdString() + " exists");
          return;
        }
        report("mod " + mod->manifest.id + " state " + std::to_string(int(mod->state)) + " actions " + std::to_string(mod->actions.size())
          + (mod->error.empty() ? "" : " last error: " + mod->error));
        for (auto const& l : mod->log)
          report("  log: " + l);
      }
      else if (cmd == "expect_file")
      {
        expect(QFile::exists(line.mid(cmd.size() + 1).trimmed()), "file exists " + line.mid(cmd.size() + 1).trimmed().toStdString());
      }
      else if (runEverwood(cmd, a, line))
      {
      }
      else if (runZone(cmd, a, line))
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

    std::string paintHashText()
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(requireTool()->layer()->paintHash()));
      return buf;
    }


    // ---- zone editor / map wizard automation (drives the same functions the buttons call) ----
    Noggit::AreaTool* areaTool()
    {
      if (!_view)
        return nullptr;
      for (auto& tool : _view->_tools)
        if (tool && tool->editingMode() == editing_mode::areaid)
          return static_cast<Noggit::AreaTool*>(tool.get());
      return nullptr;
    }

    Noggit::Ui::zone_id_browser* zoneBrowser()
    {
      auto* t = areaTool();
      if (!t || !t->zoneBrowser())
      {
        report("FAIL no Area tool in this map view");
        finish(8);
      }
      return t->zoneBrowser();
    }

    static size_t areaColumn(QString const& n, bool& ok)
    {
      ok = true;
      if (n == "id") return AreaDB::AreaID;
      if (n == "map") return AreaDB::Continent;
      if (n == "parent") return AreaDB::Region;
      if (n == "areabit") return AreaDB::AreaBit;
      if (n == "flags") return AreaDB::Flags;
      if (n == "sp") return AreaDB::SoundProviderPreferences;
      if (n == "usp") return AreaDB::UnderwaterSoundProviderPreferences;
      if (n == "ambience") return AreaDB::SoundAmbience;
      if (n == "music") return AreaDB::ZoneMusic;
      if (n == "intro") return AreaDB::ZoneIntroMusicTable;
      if (n == "exploration") return AreaDB::ExplorationLevel;
      if (n == "mask") return AreaDB::NameMask;
      if (n == "faction") return AreaDB::FactionGroup;
      if (n == "minelev") return AreaDB::MinElevation;
      if (n == "ambient") return AreaDB::AmbientMultiplier;
      if (n == "light") return AreaDB::LightId;
      bool num = false;
      size_t const v = n.toULongLong(&num);
      ok = num;
      return v;
    }

    std::string fileHashText(QString const& rel)
    {
      QFile f(QString::fromStdString(_project->ProjectPath) + "/" + rel);
      if (!f.open(QIODevice::ReadOnly))
        return "missing";
      QByteArray const d = f.readAll();
      unsigned long long h = 1469598103934665603ull;
      for (unsigned char c : d) { h ^= c; h *= 1099511628211ull; }
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%016llx size=%lld", h, static_cast<long long>(d.size()));
      return buf;
    }

    bool compareCell(DBCFile::Record& rec, size_t col, QString const& want)
    {
      if (want.contains('.'))
        return std::fabs(rec.getFloat(col) - want.toFloat()) < 1e-4f;
      return rec.getUInt(col) == static_cast<unsigned>(want.toLongLong());
    }

    bool runZone(QString const& cmd, QStringList const& a, QString const& line)
    {
      auto rest = [&](int from) { QStringList r = a.mid(from); return r.join(' '); };
      if (cmd == "zone_tool")
      {
        _view->set_editing_mode(editing_mode::areaid);
        report(std::string("active tool: ") + _view->activeTool()->name());
      }
      else if (cmd == "zone_create")
      {
        // zone_create <name...>  (id = first free, like the dialog default)
        int const id = gAreaDB.getEmptyRecordID();
        int const got = zoneBrowser()->create_zone(id, rest(1).toStdString());
        _last_area = got;
        report("created zone " + std::to_string(got));
        expect(got == id, "zone_create");
      }
      else if (cmd == "subzone_create")
      {
        // subzone_create <parentId|last> <name...>
        int const id = gAreaDB.getEmptyRecordID();
        int const parent = a.value(1) == "last" ? _last_area : a.value(1).toInt();
        int const got = zoneBrowser()->create_subzone(parent, id, rest(2).toStdString());
        _last_area = got;
        report("created subzone " + std::to_string(got));
        expect(got == id, "subzone_create");
      }
      else if (cmd == "zone_open")
      {
        int const id = a.value(1) == "last" ? _last_area : a.value(1).toInt();
        zoneBrowser()->select_area(id);
        zoneBrowser()->open_area_editor();
        report("opened area " + std::to_string(id));
      }
      else if (cmd == "zone_set")
      {
        expect(zoneBrowser()->area_editor()->set_value(a.value(1).toStdString(), a.value(2).toDouble()), "zone_set " + a.value(1).toStdString());
      }
      else if (cmd == "zone_set_name")
      {
        zoneBrowser()->area_editor()->set_name(rest(2).toStdString(), a.value(1).toInt());
      }
      else if (cmd == "zone_save")
      {
        zoneBrowser()->area_editor()->save_area();
        report("zone saved");
      }
      else if (cmd == "area_save_raw")
      {
        gAreaDB.save();
        report("AreaTable.dbc written unchanged (" + std::to_string(gAreaDB.getRecordCount()) + " rows, " + std::to_string(gAreaDB.getFieldCount()) + " fields)");
      }
      else if (cmd == "map_save_raw")
      {
        gMapDB.save();
        report("Map.dbc written unchanged (" + std::to_string(gMapDB.getRecordCount()) + " rows, " + std::to_string(gMapDB.getFieldCount()) + " fields)");
      }
      else if (cmd == "area_dbc_hash")
      {
        report("AreaTable.dbc " + fileHashText("DBFilesClient/AreaTable.dbc"));
      }
      else if (cmd == "map_dbc_hash")
      {
        report("Map.dbc " + fileHashText("DBFilesClient/Map.dbc"));
      }
      else if (cmd == "expect_area")
      {
        // expect_area <id|last> <field name|column> <value>   or   expect_area <id|last> name <slot> <text>
        int const id = a.value(1) == "last" ? _last_area : a.value(1).toInt();
        try
        {
          auto rec = gAreaDB.getByID(id);
          if (a.value(2) == "name")
          {
            std::string const got = rec.getLocalizedString(AreaDB::Name, a.value(3).toInt());
            std::string const want = rest(4).toStdString();
            expect(got == want, "area " + std::to_string(id) + " name[" + a.value(3).toStdString() + "] = [" + got + "] (want [" + want + "])");
          }
          else
          {
            bool ok = false;
            size_t const col = areaColumn(a.value(2), ok);
            expect(ok && compareCell(rec, col, a.value(3)), "area " + std::to_string(id) + " " + a.value(2).toStdString() + " (col " + std::to_string(col) + ") = " + a.value(3).toStdString()
                   + " (got u32 " + std::to_string(rec.getUInt(col)) + ")");
          }
        }
        catch (DBCFile::NotFound const&) { expect(false, "area " + std::to_string(id) + " exists"); }
      }
      else if (cmd == "expect_area_rows")
      {
        expect(static_cast<int>(gAreaDB.getRecordCount()) == a.value(1).toInt(), "AreaTable rows = " + a.value(1).toStdString() + " (got " + std::to_string(gAreaDB.getRecordCount()) + ")");
        expect(static_cast<int>(gAreaDB.getFieldCount()) == a.value(2).toInt(), "AreaTable fields = " + a.value(2).toStdString() + " (got " + std::to_string(gAreaDB.getFieldCount()) + ")");
      }
      else if (cmd == "map_create")
      {
        // map_create <directory> <area_table_id> <tile_x> <tile_z> <name...>
        _window->_map_creation_wizard->smokeCreateMap(a.value(1).toStdString(), rest(5).toStdString(), a.value(2).toInt(), a.value(3).toInt(), a.value(4).toInt());
        report("map created, Map.dbc rows " + std::to_string(gMapDB.getRecordCount()));
      }
      else if (cmd == "expect_map")
      {
        // expect_map <id> <column> <value>
        try
        {
          auto rec = gMapDB.getByID(a.value(1).toInt());
          size_t const col = a.value(2).toULongLong();
          expect(compareCell(rec, col, a.value(3)), "map " + a.value(1).toStdString() + " col " + a.value(2).toStdString() + " = " + a.value(3).toStdString() + " (got u32 " + std::to_string(rec.getUInt(col)) + ")");
        }
        catch (DBCFile::NotFound const&) { expect(false, "map exists"); }
      }
      else
        return false;
      return true;
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
      else if (cmd == "undo" || cmd == "redo")
      {
        auto const t0 = clock::now();
        if (cmd == "undo")
          requireTool()->undo();
        else
          requireTool()->redo();
        report(cmd.toStdString() + " took " + std::to_string(std::chrono::duration<double, std::milli>(clock::now() - t0).count()) + " ms");
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
      else if (cmd == "remove_floaters")
      {
        // remove_floaters [max]: the panel's "Remove floaters" button (whole edited area, one undo step)
        auto* tool = requireTool();
        if (a.size() > 1)
          tool->setOption("floater_max", a.value(1));
        report("remove_floaters: " + tool->removeFloaters());
      }
      else if (cmd == "expect_floaters")
      {
        // expect_floaters <n|>n> [max]: floaters in the edited area (touching ones included)
        uint32_t const max = a.size() > 2 ? a.value(2).toUInt() : 64u;
        size_t const n = requireTool()->layer()->findFloaters(max, Noggit::Everwood::Voxel::FloaterRegion{}).size();
        bool const atLeast = a.value(1).startsWith('>');
        size_t const want = a.value(1).mid(atLeast ? 1 : 0).toULongLong();
        expect(atLeast ? n > want : n == want, "floaters " + std::to_string(n) + " vs " + a.value(1).toStdString());
      }
      else if (cmd == "list_floaters")
      {
        // list_floaters [max]: each floater's size and centre (wow axes), diagnostics
        uint32_t const max = a.size() > 1 ? a.value(1).toUInt() : 64u;
        int n = 0;
        for (auto const& fl : requireTool()->layer()->findFloaters(max, Noggit::Everwood::Voxel::FloaterRegion{}))
        {
          double c[3] = { 0, 0, 0 };
          for (auto const& s : fl.samples)
            for (int i = 0; i < 3; ++i)
              c[i] += double(s[i]) * 0.5 / double(fl.samples.size());
          char b[160];
          std::snprintf(b, sizeof(b), "floater %d: %zu samples at %.2f %.2f %.2f%s", n++, fl.samples.size(), c[0], c[1], c[2], fl.touching ? " (touching)" : "");
          report(b);
        }
      }
      else if (cmd == "floater_snap")
      {
        // floater_snap <name> [max]: every density sample + the samples of the current floaters
        uint32_t const max = a.size() > 2 ? a.value(2).toUInt() : 64u;
        auto* layer = requireTool()->layer();
        FloaterSnap snap;
        snap.chunks = layer->field().ChunksRef();
        size_t floaters = 0;
        for (auto const& f : layer->findFloaters(max, Noggit::Everwood::Voxel::FloaterRegion{}))
        {
          ++floaters;
          for (auto const& s : f.samples)
            snap.floater.insert(s);
        }
        report("floater_snap " + a.value(1).toStdString() + ": " + std::to_string(snap.chunks.size()) + " chunks, " + std::to_string(floaters)
               + " floaters, " + std::to_string(snap.floater.size()) + " floater samples");
        _snaps[a.value(1).toStdString()] = std::move(snap);
      }
      else if (cmd == "expect_floater_removal")
      {
        // expect_floater_removal <name>: against floater_snap: every floater sample is air now, every sample that was solid
        // and is not a floater sample is byte-identical, and nothing outside the floaters was lowered (only the air next to
        // a floater may have been raised)
        auto it = _snaps.find(a.value(1).toStdString());
        if (it == _snaps.end())
        {
          expect(false, "expect_floater_removal: no snapshot " + a.value(1).toStdString());
          return true;
        }
        namespace V = Noggit::Everwood::Voxel;
        auto& world = requireTool()->layer()->field();
        FloaterSnap const& snap = it->second;
        size_t notRemoved = 0, solidChanged = 0, airChanged = 0, lowered = 0, inFloater = 0, samples = 0;
        for (auto const& [k, now] : world.ChunksRef())
        {
          auto before = snap.chunks.find(k);
          std::shared_ptr<V::ColumnSeed const> seed;
          if (before == snap.chunks.end())
            seed = world.Seed(V::ColKey{ k[0], k[1] });
          for (int32_t z = 0; z < V::C; ++z)
            for (int32_t y = 0; y < V::C; ++y)
              for (int32_t x = 0; x < V::C; ++x)
              {
                int8_t const nv = now[V::owned_index(x, y, z)];
                int32_t const gx = k[0] * V::C + x, gy = k[1] * V::C + y, gz = k[2] * V::C + z;
                int8_t const bv = before != snap.chunks.end() ? before->second[V::owned_index(x, y, z)] : seed->Value(gx, gy, gz);
                ++samples;
                bool const fl = snap.floater.count(V::Key{ gx, gy, gz }) != 0;
                if (fl && nv < 0)
                  ++notRemoved;
                if (nv == bv)
                  continue;
                if (fl)
                {
                  ++inFloater;
                  continue;
                }
                if (bv < 0)
                  ++solidChanged;
                else
                  ++airChanged;
                if (nv < bv)
                  ++lowered;
              }
        }
        report("floater removal check: " + std::to_string(samples) + " samples, floater samples changed " + std::to_string(inFloater)
               + ", still solid " + std::to_string(notRemoved) + ", solid ground changed " + std::to_string(solidChanged)
               + ", air changed outside " + std::to_string(airChanged) + " (lowered " + std::to_string(lowered) + ")");
        expect(notRemoved == 0 && solidChanged == 0 && lowered == 0, "floaters gone, no ground changed");
      }
      else if (cmd == "expect_solid")
      {
        // expect_solid <wow x> <wow y> <wow z> [1|0]: the field's sample nearest that point is solid (density < 0) or not
        auto& world = requireTool()->layer()->field();
        int const v = world.Sample(int32_t(std::lround(f(1) / 0.5f)), int32_t(std::lround(f(2) / 0.5f)), int32_t(std::lround(f(3) / 0.5f)));
        bool const want = a.size() > 4 ? a.value(4).toInt() != 0 : true;
        expect((v < 0) == want, "sample at " + a.value(1).toStdString() + " " + a.value(2).toStdString() + " " + a.value(3).toStdString()
               + " = " + std::to_string(v) + (want ? " is solid" : " is not solid"));
      }
      else if (cmd == "ramp_point")
      {
        // ramp_point <wow x> <wow y> [wow z]: a path point of the Ramp / Path tool; z = the surface when omitted
        auto* tool = requireTool();
        float z = 0.f;
        if (a.size() > 3)
          z = f(3);
        else if (auto const hit = tool->layer()->raycast(glm::vec3(f(1), f(2), 900.f), glm::vec3(0.f, 0.f, -1.f), 2000.f))
          z = hit->pos.z;
        else
          tool->layer()->rawGround(f(1), f(2), z);
        tool->addRampPoint(glm::vec3(f(1), f(2), z));
        _rampLog.push_back(glm::vec3(f(1), f(2), z));
        report("ramp point " + std::to_string(f(1)) + " " + std::to_string(f(2)) + " " + std::to_string(z));
      }
      else if (cmd == "ramp_apply")
      {
        // ramp_apply [ok|refused]: the "Build ramp" button
        auto* tool = requireTool();
        auto const t0 = clock::now();
        std::string const why = tool->confirmRamp();
        double const ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        if (why.empty())
          _lastRamp = _rampLog;
        _rampLog.clear();
        report(why.empty() ? "ramp built, " + std::to_string(tool->lastRampEdits()) + " edits in " + std::to_string(ms) + " ms, work " + std::to_string(tool->lastRampWork()) + " voxels, estimate " + std::to_string(tool->lastRampEstimateMs()) + " ms" : "ramp refused: " + why);
        if (a.size() > 1)
          expect(why.empty() == (a.value(1) == "ok"), "ramp_apply " + a.value(1).toStdString());
      }
      else if (cmd == "ramp_cancel")
      {
        requireTool()->cancelRamp();
        _rampLog.clear();
      }
      else if (cmd == "expect_ramp_line")
      {
        // expect_ramp_line follow|<deg> <tol> [skip]: the voxel surface (a ray straight down) along the last built ramp's
        // centre line (the clicked points' polyline; 'follow' = linear between the clicked heights, a number = that fixed
        // grade in degrees from the first point), every yard, ends left out by skip yd. Passes within tol yd.
        auto* layer = requireTool()->layer();
        if (_lastRamp.size() < 2)
        {
          expect(false, "expect_ramp_line: no ramp was built");
          return true;
        }
        bool const follow = a.value(1) == "follow";
        float const tanA = std::tan(a.value(1).toFloat() * 0.017453292f), tol = f(2), skip = f(3);
        std::vector<float> s{ 0.f };
        for (size_t i = 1; i < _lastRamp.size(); ++i)
          s.push_back(s.back() + glm::distance(glm::vec2(_lastRamp[i]), glm::vec2(_lastRamp[i - 1])));
        float worst = 0.f, bias = 0.f;
        int count = 0;
        std::string profile;
        for (float u = skip; u <= s.back() - skip + 1e-3f; u += 1.f)
        {
          size_t i = 0;
          while (i + 2 < _lastRamp.size() && u > s[i + 1])
            ++i;
          float const t = (u - s[i]) / (s[i + 1] - s[i]);
          glm::vec3 const q = _lastRamp[i] + (_lastRamp[i + 1] - _lastRamp[i]) * t;
          float const want = follow ? q.z : _lastRamp[0].z + u * tanA;
          auto const hit = layer->raycast(glm::vec3(q.x, q.y, std::max(want, q.z) + 60.f), glm::vec3(0.f, 0.f, -1.f), 400.f);
          if (!hit)
          {
            expect(false, "expect_ramp_line: no surface at " + std::to_string(q.x) + " " + std::to_string(q.y));
            return true;
          }
          float const err = hit->pos.z - want;
          worst = std::max(worst, std::fabs(err));
          bias += err;
          ++count;
          char b[48];
          std::snprintf(b, sizeof(b), "%.0f:%+.2f ", double(u), double(err));
          if (count % 2 == 1)
            profile += b;
        }
        report("ramp line error (yd, surface minus requested) " + profile);
        expect(count > 0 && worst <= tol, "ramp surface within " + std::to_string(tol) + " yd of the requested slope over " + std::to_string(count)
               + " points: worst " + std::to_string(worst) + ", mean " + std::to_string(count ? bias / float(count) : 0.f));
      }
      else if (cmd == "surface_line")
      {
        // surface_line <x0> <y0> <z0> <x1> <y1> <z1> <tol> [skip]: the voxel surface (a ray straight down) along the
        // line, against the straight line from z0 to z1 (skip yd at both ends left out); passes within tol yd
        auto* layer = requireTool()->layer();
        glm::vec3 const p0(f(1), f(2), f(3)), p1(f(4), f(5), f(6));
        float const tol = f(7), skip = f(8);
        float const len = glm::distance(glm::vec2(p0), glm::vec2(p1));
        float worst = 0.f, bias = 0.f;
        int count = 0;
        std::string profile;
        for (float s = skip; s <= len - skip + 1e-3f; s += 1.f)
        {
          float const t = len > 0.f ? s / len : 0.f;
          glm::vec3 const q = p0 + (p1 - p0) * t;
          auto const hit = layer->raycast(glm::vec3(q.x, q.y, std::max(p0.z, p1.z) + 60.f), glm::vec3(0.f, 0.f, -1.f), 400.f);
          if (!hit)
          {
            expect(false, "surface_line: no surface at " + std::to_string(q.x) + " " + std::to_string(q.y));
            return true;
          }
          float const err = hit->pos.z - q.z;
          worst = std::max(worst, std::fabs(err));
          bias += err;
          ++count;
          char b[48];
          std::snprintf(b, sizeof(b), "%.1f:%+.2f ", double(s), double(err));
          if (count % 2 == 1)
            profile += b;
        }
        report("surface_line error (yd, surface minus requested) " + profile);
        expect(count > 0 && worst <= tol, "surface within " + std::to_string(tol) + " yd of the requested line over " + std::to_string(count)
               + " points: worst " + std::to_string(worst) + ", mean " + std::to_string(count ? bias / float(count) : 0.f));
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
               + " hash " + hashText() + " painted " + std::to_string(layer->paintedChunkCount()) + " paint_hash " + paintHashText());
      }
      else if (cmd == "paint_ray")
      {
        // paint_ray <ox> <oy> <oz> <dx> <dy> <dz> [ok|refused]: one Paint (caves) dab where the ray (WoW axes)
        // meets the voxel surface, through the tool's own checks (texture, edited weight, 4-texture limit)
        std::string const why = requireTool()->paintRay(glm::vec3(f(1), f(2), f(3)), glm::vec3(f(4), f(5), f(6)));
        report(why.empty() ? std::string("paint_ray painted, ") + std::to_string(requireTool()->layer()->paintedChunkCount())
                               + " painted chunks, paint_hash " + paintHashText()
                           : "paint_ray refused: " + why);
        if (a.size() > 7)
          expect(why.empty() == (a.value(7) == "ok"), "paint_ray " + a.value(7).toStdString());
      }
      else if (cmd == "anchor")
      {
        // anchor: remember the last edit's position (WoW axes) for paint_rel / camera_anchor
        auto const* edit = requireTool()->layer()->lastEdit();
        if (!edit)
        {
          expect(false, "anchor: no edit");
          return true;
        }
        _anchor = glm::vec3(edit->pos[0], edit->pos[1], edit->pos[2]);
        report("anchor " + std::to_string(_anchor.x) + " " + std::to_string(_anchor.y) + " " + std::to_string(_anchor.z));
      }
      else if (cmd == "paint_rel")
      {
        // paint_rel <ox> <oy> <oz> <dx> <dy> <dz> [ok|refused]: paint_ray with the origin relative to the anchor
        std::string const why = requireTool()->paintRay(_anchor + glm::vec3(f(1), f(2), f(3)), glm::vec3(f(4), f(5), f(6)));
        report(why.empty() ? std::string("paint_rel painted, ") + std::to_string(requireTool()->layer()->paintedChunkCount())
                               + " painted chunks, paint_hash " + paintHashText()
                           : "paint_rel refused: " + why);
        if (a.size() > 7)
          expect(why.empty() == (a.value(7) == "ok"), "paint_rel " + a.value(7).toStdString());
      }
      else if (cmd == "camera_anchor")
      {
        // camera_anchor <distance> <height> [side]: camera <distance> yd along WoW +x of the anchor, <height> above
        // it, looking at it
        float const dist = f(1), height = f(2), side = f(3);
        _view->_camera.position = glm::vec3(ZEROPOINT - _anchor.y - side, _anchor.z + height, ZEROPOINT - _anchor.x - dist);
        _view->_camera.yaw(math::degrees(std::atan2(side, dist) * 57.29578f));
        _view->_camera.pitch(math::degrees(std::atan2(height, std::sqrt(dist * dist + side * side)) * 57.29578f));
        _view->_camera_moved_since_last_draw = true;
      }
      else if (cmd == "camera_look")
      {
        // camera_look <cx> <cy> <cz> <tx> <ty> <tz>: camera at anchor + c looking at anchor + t (WoW axes)
        glm::vec3 const cam = _anchor + glm::vec3(f(1), f(2), f(3));
        glm::vec3 const d = _anchor + glm::vec3(f(4), f(5), f(6)) - cam;
        _view->_camera.position = Noggit::Everwood::wowToNoggit(cam);
        _view->_camera.yaw(math::degrees(std::atan2(-d.y, -d.x) * 57.29578f));
        _view->_camera.pitch(math::degrees(std::atan2(-d.z, std::sqrt(d.x * d.x + d.y * d.y)) * 57.29578f));
        _view->_camera_moved_since_last_draw = true;
      }
      else if (cmd == "expect_painted")
      {
        size_t const n = requireTool()->layer()->paintedChunkCount();
        bool const atLeast = a.value(1).startsWith('>');
        size_t const want = a.value(1).mid(atLeast ? 1 : 0).toULongLong();
        expect(atLeast ? n > want : n == want, "painted chunks " + std::to_string(n) + " vs " + a.value(1).toStdString());
      }
      else if (cmd == "phash_save")
      {
        _hashes["paint:" + a.value(1).toStdString()] = paintHashText();
        report("paint hash " + a.value(1).toStdString() + " = " + paintHashText());
      }
      else if (cmd == "phash_same" || cmd == "phash_differs")
      {
        std::string const h = paintHashText();
        std::string const other = _hashes["paint:" + a.value(1).toStdString()];
        bool const same = h == other;
        expect(cmd == "phash_same" ? same : !same, "paint hash " + h + (cmd == "phash_same" ? " == " : " != ") + a.value(1).toStdString() + " " + other);
      }
      else if (cmd == "expect_edits")
      {
        size_t const n = requireTool()->layer()->liveEditCount();
        bool const atLeast = a.value(1).startsWith('>');
        size_t const want = a.value(1).mid(atLeast ? 1 : 0).toULongLong();
        expect(atLeast ? n > want : n == want, "live edits " + std::to_string(n) + " vs " + a.value(1).toStdString());
      }
      else if (cmd == "last_edit" || cmd == "expect_last")
      {
        // last_edit: print the newest live edit. expect_last <mode|-> <strength|-> <param hex|-> [mask hex]: check it
        auto const* le = requireTool()->layer()->lastEdit();
        if (!le)
        {
          expect(false, cmd.toStdString() + ": no edit");
          return true;
        }
        char b[160];
        std::snprintf(b, sizeof(b), "last edit mode %u strength %u radius %.2f param 0x%04x pos %.2f %.2f %.2f", unsigned(le->mode),
                      unsigned(le->strength), double(le->radius), unsigned(le->param), double(le->pos[0]), double(le->pos[1]), double(le->pos[2]));
        report(b);
        if (cmd == "expect_last")
        {
          bool ok = true;
          if (a.value(1) != "-")
            ok = ok && unsigned(le->mode) == a.value(1).toUInt();
          if (a.value(2) != "-")
          {
            // strength: exact, or "<=N" / ">=N"
            QString const t = a.value(2);
            unsigned const v = t.mid(t.startsWith('<') || t.startsWith('>') ? 2 : 0).toUInt();
            ok = ok && (t.startsWith("<=") ? le->strength <= v : (t.startsWith(">=") ? le->strength >= v : le->strength == v));
          }
          if (a.value(3) != "-")
          {
            unsigned const mask = a.size() > 4 ? a.value(4).toUInt(nullptr, 0) : 0xffffu;
            ok = ok && (unsigned(le->param) & mask) == (a.value(3).toUInt(nullptr, 0) & mask);
          }
          expect(ok, std::string(b));
        }
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
      else if (cmd == "terrain")
      {
        // terrain <wow x> <wow y> <radius> <change>: one dab of Noggit's own terrain tool (raise > 0, lower < 0,
        // smooth brush), as one undoable action, at the ground height under (x, y)
        auto* layer = requireTool()->layer();
        World* world = _view->getWorld();
        _view->makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, _view->context());
        glm::vec3 pos(ZEROPOINT - f(2), 0.0f, ZEROPOINT - f(1));
        float h = 0.0f;
        if (layer->rawGround(f(1), f(2), h))
          pos.y = h;
        NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TERRAIN);
        world->changeTerrain(pos, f(4), f(3), int(eTerrainType_Smooth), 0.0f);
        NOGGIT_ACTION_MGR->endAction();
        report("terrain dab");
      }
      else if (cmd == "live_seed")
      {
        // live_seed <tileX> <tileY> [max]: the live seed of a loaded tile against its ADT bytes on disk
        size_t samples = 0;
        auto* layer = requireTool()->layer();
        float const diff = layer->liveSeedDifference(a.value(1).toInt(), a.value(2).toInt(), samples);
        report("live seed vs file: max diff " + std::to_string(diff) + " over " + std::to_string(samples)
               + " points, live tiles " + std::to_string(layer->liveSeedTileCount()));
        if (a.size() > 3)
          expect(samples > 0 && diff <= f(3), "live seed within " + a.value(3).toStdString() + " of the file");
      }
      else if (cmd == "expect_live_tiles")
      {
        size_t const n = requireTool()->layer()->liveSeedTileCount();
        expect(n == size_t(a.value(1).toInt()), "live seed tiles " + std::to_string(n) + " vs " + a.value(1).toStdString());
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
      else if (cmd == "prof")
      {
        report("prof\n" + Noggit::Everwood::profileText(a.value(1) != "keep"));
      }
      else if (cmd == "perf_begin")
      {
        // perf_begin / perf_end: frames, FPS and the draw / brush draw cost between the two (reset the probes first)
        Noggit::Everwood::profileText(true);
        _perfT0 = clock::now();
      }
      else if (cmd == "perf_end")
      {
        double const secs = std::chrono::duration<double>(clock::now() - _perfT0).count();
        long n = 0, bn = 0;
        double ms = 0, bms = 0, mms = 0, sms = 0;
        long mn = 0, sn = 0;
        Noggit::Everwood::profileGet("draw", n, ms);
        Noggit::Everwood::profileGet("brush.draw", bn, bms);
        Noggit::Everwood::profileGet("buildMesh", mn, mms);
        Noggit::Everwood::profileGet("buildMesh.soft", sn, sms);
        char b[400];
        std::snprintf(b, sizeof b, "perf %.2f s: layer draws %ld (%.1f fps), draw avg %.3f ms, brush draws %ld avg %.4f ms, buildMesh %ld total %.1f ms, soft %ld total %.1f ms",
                      secs, n, secs > 0 ? double(n) / secs : 0.0, n ? ms / double(n) : 0.0, bn, bn ? bms / double(bn) : 0.0, mn, mms, sn, sms);
        report(b);
      }
      else if (cmd == "expect_brush_preview")
      {
        // expect_brush_preview <outer> <inner> <spray>: the cursor preview's radii for the current settings (yd)
        auto const pv = requireTool()->brushPreview();
        report("brush preview outer " + std::to_string(pv.outer) + " inner " + std::to_string(pv.inner) + " spray " + std::to_string(pv.spray));
        bool const ok = std::fabs(pv.outer - f(1)) < 0.01f && std::fabs(pv.inner - f(2)) < 0.01f && std::fabs(pv.spray - f(3)) < 0.01f;
        expect(ok, "brush preview " + a.value(1).toStdString() + " " + a.value(2).toStdString() + " " + a.value(3).toStdString());
      }
      else if (cmd == "journal")
      {
        // journal: the map's edit count, journal bytes and the last load's read / replay time
        auto* layer = requireTool()->layer();
        auto const& st = layer->loadStats();
        report("journal edits " + std::to_string(layer->liveEditCount()) + " bytes " + std::to_string(layer->journalBytes())
               + " | last load: " + (st.loaded ? "edits " + std::to_string(st.edits) + " bytes " + std::to_string(st.bytes)
                                                    + " read_ms " + std::to_string(st.readMs) + " replay_ms " + std::to_string(st.replayMs)
                                                  : std::string("none")));
      }
      else if (cmd == "expect_journal_edits")
      {
        auto* layer = requireTool()->layer();
        expect(layer->liveEditCount() == size_t(a.value(1).toLongLong()),
               "journal edits " + std::to_string(layer->liveEditCount()) + " vs " + a.value(1).toStdString());
      }
      else if (cmd == "expect_journal_label")
      {
        // expect_journal_label <normal|warn> [text...]: the panel's Map edits line (and whether it is in the warning colour)
        bool warn = false;
        QString const text = requireTool()->journalLine(&warn);
        report("journal label: " + text.toStdString() + (warn ? " [WARN]" : " [normal]"));
        bool ok = warn == (a.value(1) == "warn");
        if (a.size() > 2)
          ok = ok && text.contains(line.section(' ', 2));
        expect(ok, "journal label " + a.value(1).toStdString() + (a.size() > 2 ? " contains '" + line.section(' ', 2).toStdString() + "'" : std::string()));
      }
      else if (cmd == "reload_journal")
      {
        // reload_journal: read the journal file again and replay it (what a map open does); reports the timing
        auto* layer = requireTool()->layer();
        layer->load();
        auto const& st = layer->loadStats();
        report("reload_journal edits " + std::to_string(st.edits) + " bytes " + std::to_string(st.bytes) + " read_ms " + std::to_string(st.readMs)
               + " replay_ms " + std::to_string(st.replayMs));
      }
      else if (cmd == "edits_batch")
      {
        // edits_batch <n> <seed> <cx> <cy> <span>: ONE stroke of n pseudo-random ordinary edits (drag-like runs of dig / build /
        // smooth / flatten dabs, radius 1-6) on the ground within +-span yd of (cx, cy); reports how long they took
        auto* tool = requireTool();
        int const n = a.value(1).toInt();
        uint32_t x = uint32_t(a.value(2).toUInt()) * 2654435761u + 1u;
        auto next = [&x]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return float(x >> 8) / 16777216.f; };
        float const cx = f(3), cy = f(4), span = f(5);
        std::vector<Noggit::Everwood::Voxel::Edit> edits;
        edits.reserve(size_t(n));
        float px = cx, py = cy, dx = 1.f, dy = 0.f;
        int run = 0;
        while (int(edits.size()) < n)
        {
          if (run-- <= 0)
          {
            px = cx + (next() * 2.f - 1.f) * span;
            py = cy + (next() * 2.f - 1.f) * span;
            float const th = next() * 6.2831853f;
            dx = std::cos(th);
            dy = std::sin(th);
            run = 20 + int(next() * 60.f);
          }
          px += dx * 0.9f;
          py += dy * 0.9f;
          float gz = 0.f;
          if (!tool->layer()->rawGround(px, py, gz))
          {
            run = 0;
            continue;
          }
          Noggit::Everwood::Voxel::Edit e;
          float const k = next();
          e.pos[0] = px;
          e.pos[1] = py;
          e.pos[2] = gz + (next() - 0.5f) * 2.f;
          e.radius = 1.f + next() * 5.f;
          e.strength = uint8_t(60 + int(next() * 150.f));
          if (k < 0.4f) { e.mode = 0; e.param = Noggit::Everwood::Voxel::SoftParam(0, uint8_t(next() * 60.f)); }
          else if (k < 0.7f) { e.mode = 1; e.param = Noggit::Everwood::Voxel::SoftParam(0, uint8_t(next() * 60.f)); }
          else if (k < 0.9f) { e.mode = 2; e.param = Noggit::Everwood::Voxel::SoftParam(0, 0); }
          else { e.mode = 3; e.param = 0; }
          edits.push_back(e);
        }
        auto const t0 = clock::now();
        tool->applyStroke(edits);
        report("edits_batch " + std::to_string(n) + " edits in " + std::to_string(std::chrono::duration<double, std::milli>(clock::now() - t0).count()) + " ms");
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
      else if (cmd == "wmo_list")
      {
        // wmo_list [filter]: every loaded WMO instance whose path contains the filter
        QString const filter = a.value(1).toLower();
        _view->getWorld()->getModelInstanceStorage().for_each_wmo_instance([&](WMOInstance& wmo)
        {
          std::string const path = wmo.wmo->file_key().filepath();
          if (!filter.isEmpty() && !QString::fromStdString(path).toLower().contains(filter))
            return;
          auto const& e = wmo.getExtents();
          glm::vec3 const w = Noggit::Everwood::noggitToWow(wmo.pos);
          report("wmo uid " + std::to_string(wmo.uid) + " " + path + " wow " + std::to_string(w.x) + " " + std::to_string(w.y) + " " + std::to_string(w.z)
                 + " scale " + std::to_string(wmo.scale) + " raw " + std::to_string(WMOInstance::scaleToModf(wmo.scale))
                 + " extents " + std::to_string(e[0].x) + " " + std::to_string(e[0].y) + " " + std::to_string(e[0].z) + " "
                 + std::to_string(e[1].x) + " " + std::to_string(e[1].y) + " " + std::to_string(e[1].z));
        });
      }
      else if (cmd == "wmo_place")
      {
        // wmo_place <path> <wow x> <wow y> <wow z> <yaw>: add a WMO, as paste does
        glm::vec3 const p = Noggit::Everwood::wowToNoggit(glm::vec3(f(2), f(3), f(4)));
        _view->makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, _view->context());
        auto* wmo = _view->getWorld()->addWMOAndGetInstance(a.value(1).toStdString(), p, math::degrees::vec3(0.f, f(5), 0.f), 1.0f, false);
        wmo->wmo->wait_until_loaded();
        wmo->wmo->waitForChildrenLoaded();
        wmo->recalcExtents();
        report("placed wmo uid " + std::to_string(wmo->uid));
      }
      else if (cmd == "wmo_select")
      {
        World* world = _view->getWorld();
        // wmo_select <uid> [<uid> ...]: select these instances (several = multi-select)
        world->reset_selection();
        for (int i = 1; i < a.size(); ++i)
        {
          auto entry = world->get_model(a.value(i).toUInt());
          expect(entry.has_value(), "wmo " + a.value(i).toStdString() + " found");
          if (entry)
            world->add_to_selection(entry.value());
        }
      }
      else if (cmd == "wmo_scale")
      {
        // wmo_scale set|add|mult <v>: what the object editor's scale field and the mouse shortcut call
        World* world = _view->getWorld();
        _view->makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, _view->context());
        NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
        auto const type = a.value(1) == "add" ? World::object_scaling_type::add
                        : a.value(1) == "mult" ? World::object_scaling_type::mult : World::object_scaling_type::set;
        world->scale_selected_models(f(2), type);
        NOGGIT_ACTION_MGR->endAction();
      }
      else if (cmd == "wmo_delete")
      {
        // delete the selection as the Delete key does, as one undoable action
        _view->makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, _view->context());
        NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eOBJECTS_REMOVED);
        _view->getWorld()->delete_selected_models();
        NOGGIT_ACTION_MGR->endAction();
      }
      else if (cmd == "wmo_expect_scale")
      {
        // wmo_expect_scale <uid> <scale>
        auto* obj = _view->getWorld()->getObjectInstance(a.value(1).toUInt());
        expect(obj && std::abs(obj->scale - f(2)) < 1e-4f, "wmo " + a.value(1).toStdString() + " scale "
               + (obj ? std::to_string(obj->scale) : std::string("missing")) + " vs " + a.value(2).toStdString());
      }
      else if (cmd == "wmo_look")
      {
        // wmo_look <uid> <distance> <height>: camera in front of the WMO, looking at its centre
        auto* obj = _view->getWorld()->getObjectInstance(a.value(1).toUInt());
        if (!obj)
        {
          expect(false, "wmo_look: no such uid");
          return true;
        }
        auto const& e = obj->getExtents();
        glm::vec3 const c = (e[0] + e[1]) * 0.5f;
        float const dist = f(2), height = f(3);
        _view->_camera.position = glm::vec3(c.x, c.y + height, c.z - dist);
        _view->_camera.yaw(math::degrees(0.f));
        _view->_camera.pitch(math::degrees(std::atan2(height, dist) * 57.29578f));
        _view->_camera_moved_since_last_draw = true;
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

    struct PendingModal { QString png; QString button; bool text_input = false; QString text; };

    void armModalWatcher()
    {
      if (_modal_timer)
        return;
      _modal_timer = new QTimer(this);
      connect(_modal_timer, &QTimer::timeout, this, [this]
      {
        if (_modals.empty())
          return;
        QWidget* w = QApplication::activeModalWidget();
        if (!w || !w->isVisible() || w == _last_modal)
          return;
        _last_modal = w;
        PendingModal const pending = _modals.front();
        _modals.erase(_modals.begin());
        std::string text = w->windowTitle().toStdString();
        if (auto* box = qobject_cast<QMessageBox*>(w))
          text += " | " + box->text().toStdString() + " | " + box->informativeText().toStdString();
        if (auto* dlg = qobject_cast<QInputDialog*>(w))
          text += " | " + dlg->labelText().toStdString();
        std::replace(text.begin(), text.end(), '\n', ' ');
        bool const saved = w->grab().save(pending.png);
        report(std::string(saved ? "modal shot " : "FAIL modal shot not written ") + pending.png.toStdString() + " : " + text);
        if (pending.text_input)
        {
          auto* dlg = qobject_cast<QInputDialog*>(w);
          if (!dlg)
          {
            report("FAIL modal is not an input dialog, closing it");
            _failed = true;
            w->close();
            return;
          }
          dlg->setTextValue(pending.text);
          // never echo a secret field into the result file
          report(dlg->textEchoMode() == QLineEdit::Password ? "modal input (secret) entered" : "modal input '" + pending.text.toStdString() + "'");
          dlg->done(QDialog::Accepted);
          return;
        }
        QAbstractButton* hit = nullptr;
        for (auto* b : w->findChildren<QAbstractButton*>())
          if (QString(b->text()).remove('&') == pending.button)
            hit = b;
        if (hit)
        {
          report("modal click '" + pending.button.toStdString() + "'");
          hit->click();
        }
        else
        {
          report("FAIL modal has no button '" + pending.button.toStdString() + "', closing it");
          _failed = true;
          w->close();
        }
      });
      _modal_timer->start(150);
    }

    void modsMenuShot(QString const& prefix)
    {
      QMenu* menu = Noggit::Mods::ModManager::instance().menu();
      if (!menu || !_window)
      {
        expect(false, "Mods menu exists");
        return;
      }
      _window->grab().save(prefix + "_window.png");
      menu->aboutToShow();  // fills it, as opening it would (no popup: nothing grabs the desktop's mouse)
      menu->adjustSize();
      expect(menu->grab().save(prefix + "_menu.png"), "Mods menu shot " + prefix.toStdString() + "_menu.png");
      int n = 0;
      for (QAction* act : menu->actions())
      {
        std::string entries;
        if (QMenu* sub = act->menu())
        {
          sub->adjustSize();
          QString const file = prefix + "_sub" + QString::number(n++) + ".png";
          sub->grab().save(file);
          for (QAction* s2 : sub->actions())
            if (!s2->isSeparator())
              entries += " [" + s2->text().toStdString() + "]";
          report("menu " + act->text().toStdString() + " ->" + entries + " (" + file.toStdString() + ")");
        }
        else if (!act->isSeparator())
          report("menu " + act->text().toStdString());
      }
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
    std::vector<PendingModal> _modals;
    QTimer* _modal_timer = nullptr;
    QPointer<QWidget> _last_modal;

    std::shared_ptr<Noggit::Project::NoggitProject> _project;
    Noggit::Ui::Windows::NoggitWindow* _window = nullptr;
    MapView* _view = nullptr;
    int _map_id = 0;
    int _last_area = 0;
    Qt::MouseButtons _held = Qt::NoButton;
    std::size_t _realPresses = 0;
    std::size_t _realOther = 0;
    float _scratch = 0.f;
    glm::vec3 _anchor{ 0.f };
    std::map<std::string, std::string> _hashes;
    struct FloaterSnap
    {
      Noggit::Everwood::Voxel::Chunks chunks;
      std::set<std::array<int32_t, 3>> floater;
    };
    std::map<std::string, FloaterSnap> _snaps;
    std::vector<glm::vec3> _rampLog, _lastRamp;
    clock::time_point _perfT0 = clock::now();

    int _left = 2580, _top = 20, _width = 1500, _height = 950;

    clock::time_point _wait_until = clock::now();
    int _tile_wait_x = -1, _tile_wait_z = -1;
    bool _modlog_wait = false;
    std::string _modlog_id, _modlog_text;
    clock::time_point _modlog_deadline;
    clock::time_point _tile_deadline;
  };

  void smokeStart(Noggit::Application::NoggitApplication* application)
  {
    new SmokeRunner(application);
  }
}
