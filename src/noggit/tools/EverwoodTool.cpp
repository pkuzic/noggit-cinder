// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "EverwoodTool.hpp"

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/ChunkWater.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/TabletManager.hpp>
#include <noggit/TileWater.hpp>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/Log.h>
#include <noggit/everwood/PaintPalette.hpp>
#include <noggit/everwood/VoxelPaint3.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <opengl/context.hpp>
#include <opengl/context.inl>

#include <QtCore/QElapsedTimer>
#include <QtCore/QLocale>
#include <QtCore/QString>
#include <QtGui/QGuiApplication>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace Noggit::Everwood { void profileAdd(char const* name, double ms); }

namespace Noggit
{
  using namespace Everwood;
  using namespace Everwood::Voxel;

  namespace
  {
    constexpr float BRIDGE_MAX_LENGTH = 160.f;
    constexpr int BRIDGE_MAX_EDITS = 400;
    constexpr float AIM_RANGE = 800.f;
    constexpr float RAMP_MAX_LENGTH = 300.f;
    // Ramp limits (measured in the lane-4 smoke runs on the lane-3 hillside, EVERWOOD-MODE.md): applying costs about
    // 0.1 ms per edit plus 0.026 ms per 1000 voxels of the edits' boxes (4 r + 3 voxels a side), so big dabs cost far
    // more than small ones (a loaded PC takes up to 1.8x as long). A ramp is refused above ~3 s of estimated apply
    // time, or above 10000 edits (what the map journal replays at every load).
    constexpr int RAMP_MAX_EDITS = 10000;
    constexpr double RAMP_MAX_MS = 3000.0;
    constexpr double RAMP_MS_PER_EDIT = 0.1;
    constexpr double RAMP_MS_PER_VOXEL = 2.6e-5;
    constexpr double RAMP_FEEDBACK_MS = 400.0;    // from this estimate on: wait cursor + "Building ramp..."
    // Journal replay cost per edit when no measured load exists (ms; measured 0.3-1.5 ms per brush edit, 0.8 for
    // 2k-10k edits, EVERWOOD-MODE.md) and the load time that turns the panel line amber (~2500 edits at 0.8 ms).
    constexpr double JOURNAL_MS_PER_EDIT = 0.8;
    constexpr double JOURNAL_WARN_MS = 2000.0;

    // sculpt.rs strength_u8: percent (1-100) to the wire strength /255.
    uint8_t strengthU8(float pct)
    {
      float const v = std::round(std::clamp(pct, 1.f, 100.f) * 2.55f);
      return uint8_t(std::clamp(v, 1.f, 255.f));
    }

    // pushpull.rs step_yd
    float stepYd(float radius, float strengthPct)
    {
      return std::clamp(radius * std::clamp(strengthPct, 1.f, 100.f) / 100.f * 0.5f, 0.25f, std::max(radius, 0.25f));
    }

    void setPos(Edit& e, glm::vec3 const& p)
    {
      e.pos[0] = p.x;
      e.pos[1] = p.y;
      e.pos[2] = p.z;
    }

    glm::vec3 getPos(Edit const& e)
    {
      return { e.pos[0], e.pos[1], e.pos[2] };
    }

    // Water flow limits (EVERWOOD-WATER-FLOW.md): velocity 0..8 yd/s, turbulence 0..1.
    constexpr float FLOW_MIN_STEP = 0.5f;    // drag samples shorter than this (yd) are ignored
    constexpr float FLOW_RADIUS_MIN = 2.f;
    constexpr float FLOW_RADIUS_MAX = 100.f;

    // CONTRACT v3.10 paint (caves): the tool's limits (eme tools/paint.rs; the 8 yd cap is mode 7's validity).
    constexpr float PAINT_RADIUS_MIN = 0.5f;
    constexpr float PAINT_EDITED_MIN = 0.5f;   // a hit less edited than this is untouched ground

    // A texture path as the palette spells it: lower case, backslashes.
    std::string paletteForm(std::string path)
    {
      for (char& c : path)
        c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
      return path;
    }

    int paletteIdOf(std::string const& path)
    {
      std::string const p = paletteForm(path);
      for (size_t i = 0; i < PAINT_PALETTE_LEN; ++i)
        if (p == PAINT_PALETTE[i])
          return int(i);
      return -1;
    }

    // eme tools/paint.rs texture_label: the path without "tileset\\" and ".blp".
    QString paletteLabel(size_t id)
    {
      QString s = QString::fromLatin1(PAINT_PALETTE[id]);
      if (s.startsWith("tileset\\"))
        s = s.mid(8);
      if (s.endsWith(".blp"))
        s.chop(4);
      return s;
    }
  }

  // ------------------------------------------------------------------------------------- stroke builder
  bool EverwoodStrokeBuilder::idempotent(Sample const& s)
  {
    return s.mode <= MODE_BUILD && !(s.param & SOFT_FLAG);
  }

  EverwoodStrokeBuilder::Sample EverwoodStrokeBuilder::capped(Sample s) const
  {
    if (idempotent(s))
    {
      float const reach = std::max(_config.hardDepthRadii, 0.f) * s.radius;
      if (s.mode == MODE_DIG)
        s.pos[2] = std::max(s.pos[2], _startZ - reach);
      else if (s.mode == MODE_BUILD)
        s.pos[2] = std::min(s.pos[2], _startZ + reach);
    }
    return s;
  }

  bool EverwoodStrokeBuilder::rateAllows(uint64_t nowMs) const
  {
    return !_lastSentMs || nowMs - *_lastSentMs >= _config.minIntervalMs;
  }

  bool EverwoodStrokeBuilder::due(Sample const& s) const
  {
    if (!_lastSent)
      return true;
    Sample const& last = *_lastSent;
    if (last.mode != s.mode || last.strength != s.strength || last.param != s.param || last.radius != s.radius)
      return true;
    bool const hard = idempotent(s);
    float const dx = last.pos[0] - s.pos[0], dy = last.pos[1] - s.pos[1], dz = last.pos[2] - s.pos[2];
    float const d = hard && _config.planeMoves ? std::sqrt(dx * dx + dy * dy) : std::sqrt(dx * dx + dy * dy + dz * dz);
    float const r = std::max(s.radius, std::numeric_limits<float>::epsilon());
    if (d >= _config.moveFraction * r)
      return true;
    return !(hard && d < _config.stationaryFraction * r);
  }

  void EverwoodStrokeBuilder::begin(EverwoodStrokeConfig const& config, uint64_t nowMs, Sample const& first, std::vector<Sample>& out)
  {
    _config = config;
    _active = true;
    _startZ = first.pos[2];
    _lastSent.reset();
    _lastSentMs.reset();
    _pending = first;
    tick(nowMs, out);
  }

  void EverwoodStrokeBuilder::beginEmpty(EverwoodStrokeConfig const& config)
  {
    _config = config;
    _active = true;
    _startZ = 0.f;
    _lastSent.reset();
    _lastSentMs.reset();
    _pending.reset();
  }

  void EverwoodStrokeBuilder::update(uint64_t nowMs, Sample sample, std::vector<Sample>& out)
  {
    if (!_active)
      return;
    _pending = capped(sample);
    tick(nowMs, out);
  }

  void EverwoodStrokeBuilder::tick(uint64_t nowMs, std::vector<Sample>& out)
  {
    if (!_active || !_pending || !rateAllows(nowMs) || !due(*_pending))
      return;
    out.push_back(*_pending);
    _lastSent = *_pending;
    _lastSentMs = nowMs;
    _pending.reset();
  }

  void EverwoodStrokeBuilder::end(uint64_t nowMs, std::optional<Sample> sample, std::vector<Sample>& out, bool cursorMoved, bool softenOnly)
  {
    if (!_active)
      return;
    if (!cursorMoved)
    {
      sample.reset();
      if (_pending && !(softenOnly && _pending->mode == MODE_SMOOTH))
        _pending.reset();
    }
    if (sample)
      _pending = capped(*sample);
    tick(nowMs, out);
    // The game holds a rate-limited release edit back and sends it a moment later; nothing is rate limited
    // here, so it goes straight into the stroke.
    if (_pending && due(*_pending))
      out.push_back(*_pending);
    _pending.reset();
    _active = false;
  }

  // --------------------------------------------------------------------------------------- bridge spec
  int EverwoodTool::BridgeSpec::points() const
  {
    float const len = glm::distance(a, b) + 4.f * std::fabs(arch);
    return std::max(int(std::ceil(len / (radius * 0.5f))), 1) + 1;
  }

  glm::vec3 EverwoodTool::BridgeSpec::point(int i, int n) const
  {
    float const t = n <= 1 ? 0.f : float(i) / float(n - 1);
    glm::vec3 p = a + (b - a) * t;
    p.z += arch * 4.f * t * (1.f - t);
    return p;
  }

  bool EverwoodTool::BridgeSpec::carvesUnderside() const
  {
    return !tunnel && thickness < 2.f * radius - 0.01f;
  }

  EverwoodTool::BridgeSpec EverwoodTool::bridgeSpec() const
  {
    return { _bridgeA.value_or(glm::vec3(0.f)), _bridgeB.value_or(glm::vec3(0.f)), _bridgeRadius, _bridgeShape, _tunnel, _arch
           , std::clamp(_thickness, 0.5f, 2.f * _bridgeRadius) };
  }

  // ---------------------------------------------------------------------------------------------- tool
  EverwoodTool::EverwoodTool(MapView* mapView)
    : Tool{ mapView }
  {
    ::World* world = mapView->getWorld();
    _layer = std::make_shared<EverwoodLayer>(world
                                            , Noggit::Project::CurrentProject::get()->ProjectPath
                                            , world->basename
                                            , uint16_t(world->getMapID()));
  }

  EverwoodTool::~EverwoodTool() = default;

  char const* EverwoodTool::name() const
  {
    return "Everwood";
  }

  editing_mode EverwoodTool::editingMode() const
  {
    return editing_mode::everwood;
  }

  Ui::FontNoggit::Icons EverwoodTool::icon() const
  {
    return Ui::FontNoggit::VISIBILITY_TERRAIN;
  }

  ToolDrawParameters EverwoodTool::drawParameters() const
  {
    ToolDrawParameters params;
    // The brush is previewed in postRender on the voxel surface; the terrain shader's circle only lands on
    // the real ADT, which is hidden where it matters.
    params.cursor_type = CursorType::NONE;
    return params;
  }

  float EverwoodTool::brushRadius() const
  {
    if (_kind == Kind::Flow)
      return _flowRadius;
    if (_kind == Kind::Ramp)
      return std::clamp(_rampWidth * 0.5f, 0.5f, 16.f);
    return _kind == Kind::Bridge ? _bridgeRadius : (_kind == Kind::Paint ? _paintRadius : _radius);
  }

  void EverwoodTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
  {
    _panel = new QWidget(mapView());
    auto* outer = new QVBoxLayout(_panel);
    outer->setAlignment(Qt::AlignTop);

    auto* intro = new QLabel("Voxel edits, the same as the game's EME map editor. ADT files are never changed.", _panel);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* paintNote = new QLabel(
      "Texturing: paint voxel ground with the normal Texture Paint tool (Shift + left mouse). It edits the ADT's own "
      "layers, so it shows on the voxel surface at once and is saved with the ADT. Tops and gentle slopes take the "
      "paint. Walls, undersides and cave floors (anything dug or built) take Tool > Paint (caves): it paints the "
      "voxel volume itself, so a cave floor never paints the ground above it.", _panel);
    paintNote->setWordWrap(true);
    outer->addWidget(paintNote);

    _form = new QFormLayout();
    outer->addLayout(_form);

    auto spin = [this](double min, double max, double step, double value, int decimals = 1)
    {
      auto* box = new QDoubleSpinBox(_panel);
      box->setRange(min, max);
      box->setSingleStep(step);
      box->setDecimals(decimals);
      box->setValue(value);
      return box;
    };
    auto combo = [this](QStringList const& items)
    {
      auto* box = new QComboBox(_panel);
      box->addItems(items);
      return box;
    };

    _kindBox = combo({ "Sculpt", "Flatten", "Bridge / Tunnel", "Paint (caves)", "Water flow", "Ramp / Path" });
    _modeBox = combo({ "Lower / Dig", "Raise / Build", "Smooth", "Push out", "Pull in" });
    _radiusBox = spin(0.5, 16.0, 0.5, _radius);
    _strengthBox = spin(1.0, 100.0, 1.0, _strength, 0);
    _shapeBox = combo({ "Sphere", "Cube", "Upright cylinder" });
    _hardBox = new QCheckBox("Hard brush (CSG)", _panel);
    _falloffBox = combo({ "Smooth", "Linear", "Flat" });
    _passesBox = combo({ "1", "2", "4", "8" });
    _passesBox->setCurrentIndex(_smoothPasses);
    _passesBox->setToolTip("How many smoothing passes one dab runs (CONTRACT v3.11). More passes smooth a steep slope "
                           "in a single stroke; the game replays the same bytes.");
    _pressureBox = new QCheckBox("Use tablet pressure", _panel);
    _pressureBox->setChecked(_usePressure);
    _pressureBox->setToolTip("With a pen tablet, the pen pressure scales the sculpt strength and the paint opacity. "
                             "No effect without a tablet.");
    _hardnessBox = spin(0.0, 100.0, 5.0, _hardness, 0);
    _offsetBox = spin(-16.0, 16.0, 0.25, _offset, 2);
    _pushEdgeBox = combo({ "Hard edge", "Smooth", "Linear" });
    _pushDirBox = combo({ "Surface normal", "View direction" });
    _bridgeKindBox = combo({ "Bridge (build)", "Tunnel (dig)" });
    _archBox = spin(-32.0, 32.0, 0.5, _arch);
    _thicknessBox = spin(0.5, 32.0, 0.25, _thickness, 2);

    // Ramp / Path
    _rampWidthBox = spin(1.0, 32.0, 0.5, _rampWidth);
    _rampWidthBox->setToolTip("Flat width of the path in yards. The edge softness adds a bank on each side.");
    _rampSlopeBox = combo({ "Follow the clicked heights", "Fixed angle" });
    _rampSlopeBox->setToolTip("Follow the clicked heights: the path rises or falls in a straight line between the points you click.\n"
                              "Fixed angle: it leaves the first point at the angle below, whatever the other points' heights.");
    _rampAngleBox = spin(-60.0, 60.0, 1.0, _rampAngle, 1);
    _rampAngleBox->setToolTip("Fixed angle in degrees from the first point: positive rises along the path, negative falls.");
    _rampEdgeBox = combo({ "Sharp", "Normal", "Soft" });
    _rampEdgeBox->setCurrentIndex(_rampEdge);
    _rampEdgeBox->setToolTip("Edge softness: how the path banks into the surrounding ground. Sharp = a short steep bank (60 deg, about 1 yd), "
                             "Normal = 45 deg, about 2 yd, Soft = a gentle wide bank (30 deg, about 4 yd).");
    _rampSmoothBox = new QCheckBox("Smooth edges", _panel);
    _rampSmoothBox->setChecked(_rampSmooth);
    _rampSmoothBox->setToolTip("After levelling, one light smooth pass along the strip rounds the edges and evens out the surface. "
                               "Same stroke, same undo step.");
    _smoothFloatersBox = new QCheckBox("Also remove floaters under the brush", _panel);
    _smoothFloatersBox->setChecked(_smoothFloaters);
    _smoothFloatersBox->setToolTip("When a Smooth stroke ends, tiny islands of ground floating in the air under the brush are dug away "
                                   "(ordinary dig edits, same undo step as the stroke). Nothing but the floaters is touched.");

    _form->addRow("Tool", _kindBox);
    _form->addRow("Mode", _modeBox);
    _form->addRow("Bridge", _bridgeKindBox);
    _form->addRow("Radius (yd)", _radiusBox);
    _form->addRow("Shape", _shapeBox);
    _form->addRow("Strength (%)", _strengthBox);
    _form->addRow("Passes", _passesBox);
    _form->addRow("", _pressureBox);
    _form->addRow("", _hardBox);
    _form->addRow("Falloff", _falloffBox);
    _form->addRow("Hardness (%)", _hardnessBox);
    _form->addRow("Plane offset (yd)", _offsetBox);
    _form->addRow("Edge", _pushEdgeBox);
    _form->addRow("Direction", _pushDirBox);
    _form->addRow("Arch (yd)", _archBox);
    _form->addRow("Deck thickness (yd)", _thicknessBox);
    _form->addRow("Width (yd)", _rampWidthBox);
    _form->addRow("Slope", _rampSlopeBox);
    _form->addRow("Angle (deg)", _rampAngleBox);
    _form->addRow("Edge softness", _rampEdgeBox);
    _form->addRow("", _rampSmoothBox);
    _form->addRow("", _smoothFloatersBox);

    // CONTRACT v3.10 Paint (caves): only the shared palette's textures (PaintPalette.hpp, the wire ids).
    _paintBox = new QWidget(_panel);
    auto* paintForm = new QFormLayout(_paintBox);
    paintForm->setContentsMargins(0, 0, 0, 0);
    _paintPreview = new Ui::current_texture(true, _paintBox);
    _paintPreview->setToolTip("Drop a texture here from the texture palette (it must be one of the shared palette's).");
    _paintTextureBox = new QComboBox(_paintBox);
    _paintTextureBox->setEditable(true);
    _paintTextureBox->setInsertPolicy(QComboBox::NoInsert);
    _paintTextureBox->addItem("(none)");
    for (size_t i = 0; i < PAINT_PALETTE_LEN; ++i)
      _paintTextureBox->addItem(paletteLabel(i));
    _paintTextureBox->completer()->setFilterMode(Qt::MatchContains);
    _paintTextureBox->completer()->setCompletionMode(QCompleter::PopupCompletion);
    _paintPickButton = new QPushButton("Use the Texture Paint texture", _paintBox);
    _paintPickButton->setToolTip("Takes the texture selected in the Texture Paint tool, if it is in the shared palette.");
    _opacityBox = spin(1.0, 100.0, 1.0, _paintOpacity, 0);
    _paintFalloffBox = combo({ "Smooth", "Linear", "Flat" });
    _paintHardBox = combo({ "Off (use falloff)", "Core 25 %", "Core 50 %", "Core 75 %", "Core 90 %" });
    _paintHardBox->setToolTip("Hard brush (CONTRACT v3.11): full opacity inside the core, a smooth falloff from the "
                              "core to the edge. When on, the Falloff choice is ignored.");
    _sprayBox = new QCheckBox("Spray", _paintBox);
    _sprayBox->setToolTip("Spray can: every dab becomes several small dabs (the brush Radius) scattered at random inside "
                          "the spray size. Noggit only: each one is an ordinary cave-paint dab.");
    _spraySizeBox = spin(0.5, 16.0, 0.5, _spraySize);
    _spraySizeBox->setToolTip("Radius (yd) of the disc the small dabs are scattered in.");
    _sprayDensityBox = spin(1.0, 32.0, 1.0, _sprayDensity, 0);
    _sprayDensityBox->setToolTip("Small dabs per spray tick.");
    _eraseBox = new QCheckBox("Erase this texture", _paintBox);
    _revealBox = new QCheckBox("Reveal (fade every cave texture)", _paintBox);
    paintForm->addRow(_paintPreview);
    paintForm->addRow("Texture", _paintTextureBox);
    paintForm->addRow(_paintPickButton);
    paintForm->addRow("Opacity (%)", _opacityBox);
    paintForm->addRow("Hardness", _paintHardBox);
    paintForm->addRow("Falloff", _paintFalloffBox);
    paintForm->addRow("", _sprayBox);
    paintForm->addRow("Spray size (yd)", _spraySizeBox);
    paintForm->addRow("Spray density (dabs)", _sprayDensityBox);
    paintForm->addRow("", _eraseBox);
    paintForm->addRow("", _revealBox);
    auto* paintHelp = new QLabel("Paints dug or built voxel faces (cave floors, walls, ceilings) through the air the brush "
                                 "reaches. Up to 4 cave textures per 16 yd block. Untouched ground: use Texture Paint.", _paintBox);
    paintHelp->setWordWrap(true);
    paintForm->addRow(paintHelp);
    outer->addWidget(_paintBox);

    // Water flow: EVERWOOD-WATER-FLOW.md. Written into the ADT's own MCLQ layers (saved with the ADT).
    _flowBox = new QWidget(_panel);
    auto* flowColumn = new QVBoxLayout(_flowBox);
    flowColumn->setContentsMargins(0, 0, 0, 0);
    auto* flowForm = new QFormLayout();
    flowForm->setContentsMargins(0, 0, 0, 0);
    flowColumn->addLayout(flowForm);
    _flowModeBox = combo({ "Paint", "Erase" });
    _flowRadiusBox = spin(FLOW_RADIUS_MIN, FLOW_RADIUS_MAX, 1.0, _flowRadius, 0);
    _flowVelocityBox = spin(0.0, 8.0, 0.1, _flowVelocity, 1);
    _flowTurbulenceBox = spin(0.0, 1.0, 0.05, _flowTurbulence, 2);
    flowForm->addRow("Mode", _flowModeBox);
    flowForm->addRow("Radius (yd)", _flowRadiusBox);
    flowForm->addRow("Velocity (yd/s)", _flowVelocityBox);
    flowForm->addRow("Turbulence", _flowTurbulenceBox);
    auto* flowHelp = new QLabel("Drag over water to paint its current (arrows); Erase clears it. "
                                "Stored in the ADT, not the voxel journal: Ctrl+S saves it, Ctrl+Z undoes it.", _flowBox);
    flowHelp->setWordWrap(true);
    // a word-wrapped label in a QFormLayout row gets one line of height: the text was cut off
    flowColumn->addWidget(flowHelp);
    outer->addWidget(_flowBox);

    _bridgeLabel = new QLabel(_panel);
    _bridgeLabel->setWordWrap(true);
    outer->addWidget(_bridgeLabel);
    auto* bridgeRow = new QHBoxLayout();
    _confirmButton = new QPushButton("Build", _panel);
    _cancelButton = new QPushButton("Cancel", _panel);
    bridgeRow->addWidget(_confirmButton);
    bridgeRow->addWidget(_cancelButton);
    outer->addLayout(bridgeRow);

    // Remove floaters: tiny islands of ground that hang in the air (VoxelFloaters.hpp).
    _floaterBox = new QWidget(_panel);
    auto* floaterColumn = new QVBoxLayout(_floaterBox);
    floaterColumn->setContentsMargins(0, 0, 0, 0);
    auto* floaterForm = new QFormLayout();
    floaterForm->setContentsMargins(0, 0, 0, 0);
    floaterColumn->addLayout(floaterForm);
    _floaterMaxBox = spin(1.0, 4096.0, 8.0, _floaterMax, 0);
    _floaterMaxBox->setToolTip("A floating island of at most this many voxels (0.5 yd each) counts as a floater; 64 = about 8 cubic yards.");
    floaterForm->addRow("Max floater size (voxels)", _floaterMaxBox);
    auto* floaterButton = new QPushButton("Remove floaters", _floaterBox);
    floaterButton->setToolTip("Finds every small piece of ground that is not connected to the ground (anywhere in the edited area) and "
                              "digs it away with ordinary dig edits. Ground, overhangs, arches and bridges are never touched. One undo step.");
    floaterColumn->addWidget(floaterButton);
    _floaterLabel = new QLabel(_floaterBox);
    _floaterLabel->setWordWrap(true);
    _floaterLabel->setVisible(false);
    floaterColumn->addWidget(_floaterLabel);
    outer->addWidget(_floaterBox);

    // The voxel journal's buttons; hidden for Water flow (its strokes are ordinary Noggit actions: Ctrl+Z).
    _voxelButtons = new QWidget(_panel);
    auto* voxelButtons = new QVBoxLayout(_voxelButtons);
    voxelButtons->setContentsMargins(0, 0, 0, 0);
    auto* undoRow = new QHBoxLayout();
    auto* undoButton = new QPushButton("Undo stroke", _voxelButtons);
    auto* redoButton = new QPushButton("Redo stroke", _voxelButtons);
    undoRow->addWidget(undoButton);
    undoRow->addWidget(redoButton);
    voxelButtons->addLayout(undoRow);

    auto* exportButton = new QPushButton("Export voxel edits...", _voxelButtons);
    exportButton->setToolTip("Writes every live edit to <project>/everwood/export for the server import (EVERWOOD-MODE.md).");
    voxelButtons->addWidget(exportButton);
    auto* reseedButton = new QPushButton("Re-seed from saved ADTs", _voxelButtons);
    reseedButton->setToolTip("Replays every edit on the ADT heights as saved on disk now. Use it after saving terrain changes under voxel edits.");
    voxelButtons->addWidget(reseedButton);
    outer->addWidget(_voxelButtons);

    _hint = new QLabel(_panel);
    _hint->setWordWrap(true);
    _hint->setStyleSheet("color: #e0a040;");
    _hint->setVisible(false);
    outer->addWidget(_hint);

    _journalLabel = new QLabel(_panel);
    _journalLabel->setWordWrap(true);
    outer->addWidget(_journalLabel);

    _status = new QLabel(_panel);
    _status->setWordWrap(true);
    outer->addWidget(_status);

    auto changed = [this] { updatePanel(); };

    QObject::connect(_kindBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this, changed](int i)
    {
      _kind = Kind(i);
      changed();
    });
    QObject::connect(_modeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this, changed](int i)
    {
      _mode = SculptMode(i);
      changed();
    });
    QObject::connect(_bridgeKindBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this, changed](int i)
    {
      _tunnel = i == 1;
      changed();
    });
    QObject::connect(_radiusBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v)
    {
      if (!_updating)
        (_kind == Kind::Bridge ? _bridgeRadius : (_kind == Kind::Paint ? _paintRadius : _radius)) = float(v);
    });
    QObject::connect(_strengthBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v)
    {
      if (_updating)
        return;
      if (_kind == Kind::Ramp)
        _rampStrength = float(v);
      else if (_kind == Kind::Flatten)
        _flattenStrength = float(v);
      else if (_mode == SculptMode::Push || _mode == SculptMode::Pull)
        _pushStrength = float(v);
      else
        _strength = float(v);
    });
    QObject::connect(_shapeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i)
    {
      if (!_updating)
        (_kind == Kind::Bridge ? _bridgeShape : _shape) = uint8_t(i);
    });
    QObject::connect(_hardBox, &QCheckBox::toggled, _panel, [this, changed](bool on)
    {
      _hard = on;
      changed();
    });
    QObject::connect(_falloffBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i) { _falloff = uint8_t(i); });
    QObject::connect(_hardnessBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _hardness = float(v); });
    QObject::connect(_offsetBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _offset = float(v); });
    QObject::connect(_pushEdgeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i) { _pushEdge = i; });
    QObject::connect(_pushDirBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i) { _pushAlongView = i == 1; });
    QObject::connect(_archBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _arch = float(v); });
    QObject::connect(_thicknessBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _thickness = float(v); });

    QObject::connect(_rampWidthBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _rampWidth = float(v); });
    QObject::connect(_rampSlopeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this, changed](int i)
    {
      _rampSlope = i;
      changed();
    });
    QObject::connect(_rampAngleBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _rampAngle = float(v); });
    QObject::connect(_rampEdgeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i) { _rampEdge = std::clamp(i, 0, 2); });
    QObject::connect(_rampSmoothBox, &QCheckBox::toggled, _panel, [this](bool on) { _rampSmooth = on; });
    QObject::connect(_smoothFloatersBox, &QCheckBox::toggled, _panel, [this](bool on) { _smoothFloaters = on; });
    QObject::connect(_floaterMaxBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _floaterMax = std::clamp(int(std::lround(v)), 1, 4096); });
    QObject::connect(floaterButton, &QPushButton::clicked, _panel, [this] { removeFloaters(); });

    QObject::connect(_flowModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i) { _flowErase = i == 1; });
    QObject::connect(_flowRadiusBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _flowRadius = float(v); });
    QObject::connect(_flowVelocityBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _flowVelocity = float(v); });
    QObject::connect(_flowTurbulenceBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _flowTurbulence = float(v); });

    QObject::connect(_paintTextureBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i)
    {
      if (!_updating)
        setPaintTexture(i - 1);
    });
    QObject::connect(_paintPreview, &Ui::current_texture::texture_dropped, _panel, [this](std::string const& file)
    {
      if (!selectPaintText(QString::fromStdString(file)))
      {
        setHint("That texture is not in the shared cave-paint palette.");
        setPaintTexture(_paintTexture);
      }
    });
    QObject::connect(_paintPickButton, &QPushButton::clicked, _panel, [this]
    {
      auto const selected = Ui::selected_texture::get();
      int const id = selected ? paletteIdOf((*selected)->file_key().filepath()) : -1;
      if (id < 0)
        setHint(selected ? "The Texture Paint texture is not in the shared cave-paint palette." : "No texture is selected in Texture Paint.");
      else
        setPaintTexture(id);
    });
    QObject::connect(_opacityBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _paintOpacity = float(v); });
    QObject::connect(_paintFalloffBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i) { _paintFalloff = uint8_t(i); });
    QObject::connect(_passesBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this](int i) { _smoothPasses = std::clamp(i, 0, 3); });
    QObject::connect(_pressureBox, &QCheckBox::toggled, _panel, [this](bool on) { _usePressure = on; });
    QObject::connect(_paintHardBox, QOverload<int>::of(&QComboBox::currentIndexChanged), _panel, [this, changed](int i)
    {
      _paintHard = i - 1;
      changed();
    });
    QObject::connect(_sprayBox, &QCheckBox::toggled, _panel, [this, changed](bool on)
    {
      _spray = on;
      changed();
    });
    QObject::connect(_spraySizeBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _spraySize = float(v); });
    QObject::connect(_sprayDensityBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), _panel, [this](double v) { _sprayDensity = std::clamp(int(std::lround(v)), 1, 32); });
    QObject::connect(_eraseBox, &QCheckBox::toggled, _panel, [this](bool on) { _paintErase = on; });
    QObject::connect(_revealBox, &QCheckBox::toggled, _panel, [this, changed](bool on)
    {
      _paintReveal = on;
      changed();
    });

    QObject::connect(_confirmButton, &QPushButton::clicked, _panel, [this]
    {
      if (_kind == Kind::Ramp)
        confirmRamp();
      else
        confirmBridge();
    });
    QObject::connect(_cancelButton, &QPushButton::clicked, _panel, [this]
    {
      if (_kind == Kind::Ramp)
        cancelRamp();
      else
        cancelBridge();
    });
    QObject::connect(undoButton, &QPushButton::clicked, _panel, [this] { undo(); });
    QObject::connect(redoButton, &QPushButton::clicked, _panel, [this] { redo(); });
    QObject::connect(exportButton, &QPushButton::clicked, _panel, [this]
    {
      std::string const path = exportEdits();
      if (path.empty())
        QMessageBox::warning(_panel, "Everwood", "The export file could not be written.");
      else
        QMessageBox::information(_panel, "Everwood", "Exported to:\n" + QString::fromStdString(path));
    });
    QObject::connect(reseedButton, &QPushButton::clicked, _panel, [this]
    {
      _layer->reseed();
      updateStatus();
    });

    updatePanel();
    toolPanel->registerTool(this, _panel);
  }

  void EverwoodTool::postUiSetup()
  {
    _layer->load();
    mapView()->getWorld()->renderer()->setEverwoodLayer(_layer.get());
    updateStatus();
  }

  void EverwoodTool::updatePanel()
  {
    _updating = true;
    bool const sculpt = _kind == Kind::Sculpt;
    bool const flatten = _kind == Kind::Flatten;
    bool const bridge = _kind == Kind::Bridge;
    bool const pushPull = sculpt && (_mode == SculptMode::Push || _mode == SculptMode::Pull);
    bool const shapeMatters = bridge || pushPull || (sculpt && (_mode == SculptMode::Dig || _mode == SculptMode::Build));
    bool const softOptions = sculpt && !pushPull && !_hard;
    bool const paint = _kind == Kind::Paint;
    bool const flow = _kind == Kind::Flow;
    bool const ramp = _kind == Kind::Ramp;

    auto show = [this](QWidget* field, bool visible)
    {
      field->setVisible(visible);
      if (auto* label = _form->labelForField(field))
        label->setVisible(visible);
    };
    show(_modeBox, sculpt);
    show(_bridgeKindBox, bridge);
    show(_strengthBox, !bridge && !paint && !flow);
    show(_radiusBox, !flow && !ramp);
    show(_rampWidthBox, ramp);
    show(_rampSlopeBox, ramp);
    show(_rampAngleBox, ramp && _rampSlope == 1);
    show(_rampEdgeBox, ramp);
    show(_rampSmoothBox, ramp);
    show(_smoothFloatersBox, sculpt && _mode == SculptMode::Smooth);
    _floaterBox->setVisible(!flow);
    _floaterLabel->setText(_floaterText);
    _floaterLabel->setVisible(!_floaterText.isEmpty());
    show(_shapeBox, shapeMatters);
    show(_hardBox, sculpt && !pushPull);
    show(_falloffBox, softOptions);
    show(_passesBox, sculpt && _mode == SculptMode::Smooth);
    show(_pressureBox, !bridge && !flow && !ramp);
    show(_hardnessBox, softOptions);
    show(_offsetBox, flatten);
    show(_pushEdgeBox, pushPull);
    show(_pushDirBox, pushPull);
    show(_archBox, bridge);
    show(_thicknessBox, bridge && !_tunnel);
    _bridgeLabel->setVisible(bridge || ramp);
    _confirmButton->setVisible(bridge || ramp);
    _cancelButton->setVisible(bridge || ramp);
    _confirmButton->setText(ramp ? "Build ramp" : "Build");
    _paintBox->setVisible(paint);
    _flowBox->setVisible(flow);
    _voxelButtons->setVisible(!flow);
    _eraseBox->setEnabled(!_paintReveal);
    _paintFalloffBox->setEnabled(_paintHard < 0);                 // a hard core replaces the falloff
    _spraySizeBox->setEnabled(_spray);
    _sprayDensityBox->setEnabled(_spray);
    if (!paint)
      setHint({});

    // Mode 7 is valid up to 16 yd (CONTRACT v3.11; 8 yd in v3.10).
    _radiusBox->setRange(paint ? PAINT_RADIUS_MIN : 0.5, paint ? PAINT3_MAX_RADIUS : 16.0);
    _radiusBox->setValue(bridge ? _bridgeRadius : (paint ? _paintRadius : _radius));
    _shapeBox->setCurrentIndex(bridge ? _bridgeShape : _shape);
    _strengthBox->setValue(ramp ? _rampStrength : (flatten ? _flattenStrength : (pushPull ? _pushStrength : _strength)));

    if (bridge)
    {
      if (!_bridgeA)
        _bridgeLabel->setText("Click point A.");
      else if (!_bridgeB)
        _bridgeLabel->setText("Click point B.");
      else
      {
        BridgeSpec const spec = bridgeSpec();
        _bridgeLabel->setText(QString("Length %1 yd, about %2 edits. Click again or press Build.")
                              .arg(double(glm::distance(spec.a, spec.b)), 0, 'f', 1).arg(spec.points()));
      }
    }
    if (ramp)
    {
      if (_rampPoints.empty())
        _bridgeLabel->setText("Click the ground to place the first point of the path.");
      else if (_rampPoints.size() == 1)
        _bridgeLabel->setText("Click the next point of the path.");
      else
      {
        float len = 0.f;
        for (size_t i = 1; i < _rampPoints.size(); ++i)
          len += glm::distance(glm::vec2(_rampPoints[i]), glm::vec2(_rampPoints[i - 1]));
        _bridgeLabel->setText(QString("%1 points, %2 yd long. Click more points, then click the last point again or press Build ramp.")
                              .arg(_rampPoints.size()).arg(double(len), 0, 'f', 1));
      }
    }
    _updating = false;
  }

  // Journal size and load time. Every stroke rewrites <project>/everwood/voxels/<map>.mvoxj, and Noggit and the game
  // replay all of it when the map loads: the time is the measured read + replay of the last map open, scaled up
  // by the edits added since (an estimate, marked "~"); with no journal at open, the per-edit estimate.
  QString EverwoodTool::journalLine(bool* warn) const
  {
    size_t const edits = _layer->liveEditCount();
    uint64_t const bytes = _layer->journalBytes();
    Everwood::JournalStats const& st = _layer->loadStats();
    double ms = double(edits) * JOURNAL_MS_PER_EDIT;
    bool estimated = true;
    if (st.loaded && st.edits > 0)
    {
      ms = (st.readMs + st.replayMs) * double(edits) / double(st.edits);
      estimated = edits != st.edits;
    }
    if (warn)
      *warn = ms >= JOURNAL_WARN_MS;
    QString size;
    if (bytes >= 1024ull * 1024ull)
      size = QString("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    else
      size = QString("%1 KB").arg(std::max<uint64_t>(1, (bytes + 512) / 1024));
    QString const time = ms < 100.0 ? QString("under 0.1 s") : QString("%1%2 s").arg(estimated ? "~" : "").arg(ms / 1000.0, 0, 'f', 1);
    return QString("Map edits: %1 (%2), loads in %3").arg(QLocale(QLocale::English).toString(qulonglong(edits))).arg(size).arg(time);
  }

  void EverwoodTool::updateStatus()
  {
    if (_journalLabel)
    {
      bool warn = false;
      _journalLabel->setText(journalLine(&warn));
      _journalLabel->setStyleSheet(warn ? "color: #e0a040;" : "");
      _journalLabel->setToolTip(warn ? "This map's edit history is large. Noggit and the game replay all of it every time the map "
                                       "loads, so the map will load slower in the editor and in the game."
                                     : "Every voxel edit is kept in this map's journal and replayed when the map loads.");
    }
    if (!_status)
      return;
    _status->setText(QString("%1 edits in %2 strokes, %3 cave-painted blocks\nSaved after every stroke to:\n%4")
                     .arg(_layer->liveEditCount()).arg(_layer->strokeCount()).arg(_layer->paintedChunkCount())
                     .arg(QString::fromStdString(_layer->filePath())));
  }

  void EverwoodTool::setHint(QString const& text)
  {
    if (!_hint)
      return;
    if (_hint->text() != text)
      _hint->setText(text);
    _hint->setVisible(!text.isEmpty());
  }

  void EverwoodTool::setPaintTexture(int id)
  {
    _paintTexture = id >= 0 && size_t(id) < PAINT_PALETTE_LEN ? id : -1;
    _updating = true;
    _paintTextureBox->setCurrentIndex(_paintTexture + 1);
    _updating = false;
    if (_paintTexture >= 0)
    {
      _paintPreview->set_texture(PAINT_PALETTE[_paintTexture]);
      setHint({});
    }
  }

  // eme tools/paint.rs select_text: a palette id, a full path, or a part of a path.
  bool EverwoodTool::selectPaintText(QString const& text)
  {
    QString const t = text.trimmed();
    bool number = false;
    int const id = t.toInt(&number);
    if (number && id >= 0 && size_t(id) < PAINT_PALETTE_LEN)
    {
      setPaintTexture(id);
      return true;
    }
    int exact = paletteIdOf(t.toStdString());
    if (exact < 0)
    {
      QString const needle = QString::fromStdString(paletteForm(t.toStdString()));
      for (size_t i = 0; i < PAINT_PALETTE_LEN && exact < 0; ++i)
        if (!needle.isEmpty() && QString::fromLatin1(PAINT_PALETTE[i]).contains(needle))
          exact = int(i);
    }
    if (exact < 0)
      return false;
    setPaintTexture(exact);
    return true;
  }

  bool EverwoodTool::setOption(QString const& key, QString const& value)
  {
    auto pick = [&](QComboBox* box, QStringList const& names)
    {
      int const i = names.indexOf(value);
      if (i < 0)
        return false;
      box->setCurrentIndex(i);
      return true;
    };
    bool const on = value == "1" || value == "on" || value == "true";
    if (key == "tool") return pick(_kindBox, { "sculpt", "flatten", "bridge", "paint", "flow", "ramp" });
    if (key == "floaters") { _smoothFloatersBox->setChecked(on); return true; }
    if (key == "floater_max") { _floaterMaxBox->setValue(value.toDouble()); return true; }
    if (key == "ramp_width") { _rampWidthBox->setValue(value.toDouble()); return true; }
    if (key == "ramp_slope") return pick(_rampSlopeBox, { "follow", "angle" });
    if (key == "ramp_angle") { _rampAngleBox->setValue(value.toDouble()); return true; }
    if (key == "ramp_edge") return pick(_rampEdgeBox, { "sharp", "normal", "soft" });
    if (key == "ramp_cap") { _rampCap = value.toInt(); return true; }   // smoke: measure with another cap
    if (key == "ramp_smooth") { _rampSmoothBox->setChecked(on); return true; }
    if (key == "flow_mode") return pick(_flowModeBox, { "paint", "erase" });
    if (key == "flow_radius") { _flowRadiusBox->setValue(value.toDouble()); return true; }
    if (key == "flow_velocity") { _flowVelocityBox->setValue(value.toDouble()); return true; }
    if (key == "flow_turbulence") { _flowTurbulenceBox->setValue(value.toDouble()); return true; }
    if (key == "texture") return selectPaintText(value);
    if (key == "opacity") { _opacityBox->setValue(value.toDouble()); return true; }
    if (key == "paint_falloff") return pick(_paintFalloffBox, { "smooth", "linear", "flat" });
    if (key == "passes")
    {
      int const i = QStringList({ "1", "2", "4", "8" }).indexOf(value);
      if (i < 0)
        return false;
      _passesBox->setCurrentIndex(i);
      return true;
    }
    if (key == "paint_hard")
    {
      int const i = QStringList({ "off", "25", "50", "75", "90" }).indexOf(value);
      if (i < 0)
        return false;
      _paintHardBox->setCurrentIndex(i);
      return true;
    }
    if (key == "spray") { _sprayBox->setChecked(on); return true; }
    if (key == "spray_size") { _spraySizeBox->setValue(value.toDouble()); return true; }
    if (key == "spray_density") { _sprayDensityBox->setValue(value.toDouble()); return true; }
    if (key == "pressure") { _pressureBox->setChecked(on); return true; }
    if (key == "fake_pressure") { _fakePressure = std::clamp(value.toFloat(), 0.f, 1.f); return true; }   // smoke: pretend a tablet
    if (key == "erase") { _eraseBox->setChecked(on); return true; }
    if (key == "reveal") { _revealBox->setChecked(on); return true; }
    if (key == "mode") return pick(_modeBox, { "dig", "build", "smooth", "push", "pull" });
    if (key == "shape") return pick(_shapeBox, { "sphere", "cube", "cylinder" });
    if (key == "falloff") return pick(_falloffBox, { "smooth", "linear", "flat" });
    if (key == "edge") return pick(_pushEdgeBox, { "hard", "smooth", "linear" });
    if (key == "direction") return pick(_pushDirBox, { "normal", "view" });
    if (key == "kind") return pick(_bridgeKindBox, { "bridge", "tunnel" });
    if (key == "hard") { _hardBox->setChecked(on); return true; }
    if (key == "radius") { _radiusBox->setValue(value.toDouble()); return true; }
    if (key == "strength") { _strengthBox->setValue(value.toDouble()); return true; }
    if (key == "hardness") { _hardnessBox->setValue(value.toDouble()); return true; }
    if (key == "offset") { _offsetBox->setValue(value.toDouble()); return true; }
    if (key == "arch") { _archBox->setValue(value.toDouble()); return true; }
    if (key == "thickness") { _thicknessBox->setValue(value.toDouble()); return true; }
    return false;
  }

  void EverwoodTool::onSelected()
  {
    updatePanel();
    updateStatus();
  }

  void EverwoodTool::onDeselected()
  {
    if (_flowHeld)
      flowEnd();
    _flowHit.reset();
    if (_strokeHeld)
      endStroke();
  }

  void EverwoodTool::onFocusLost()
  {
    if (_flowHeld)
      flowEnd();
    if (_strokeHeld)
      endStroke();
  }

  void EverwoodTool::unload()
  {
    mapView()->getWorld()->renderer()->setEverwoodLayer(nullptr);
    _layer->releaseGl();
    _sphere.unload();
    _box.unload();
    _line.unload();
  }

  // ---------------------------------------------------------------------------------------------- aim
  void EverwoodTool::updateAim()
  {
    math::ray const ray = mapView()->cursorRay();
    glm::vec3 const origin = noggitToWow(ray.origin());
    glm::vec3 const end = noggitToWow(ray.position(1.f));
    _viewDirWow = glm::normalize(end - origin);
    _cursorRay = { origin, _viewDirWow };
    _aim = _layer->raycast(origin, _viewDirWow, AIM_RANGE);
  }

  uint16_t EverwoodTool::strengthWire() const
  {
    return strengthU8(_strength);
  }

  // sculpt.rs sample + effective_strength
  Edit EverwoodTool::sculptSample(AimHit const& hit) const
  {
    Edit e;
    setPos(e, hit.pos);
    e.radius = _radius;

    if (_kind == Kind::Flatten)
    {
      e.mode = MODE_FLATTEN;
      e.strength = strengthU8(_flattenStrength);
      float const v = std::clamp(std::round((_flattenPlane - hit.pos.z) * 100.f), -32768.f, 32767.f);
      e.param = uint16_t(int16_t(v));
      return e;
    }

    e.mode = uint8_t(_mode);
    uint8_t const s = strengthU8(std::clamp(_strength * pressureScale(), 1.f, 100.f));
    if (_hard)
    {
      e.strength = _mode == SculptMode::Smooth ? std::min<uint8_t>(s, 128) : 255;
      e.param = 0;
    }
    else
    {
      e.strength = s;
      e.param = SoftParam(_falloff, uint8_t(std::clamp(std::round(_hardness), 0.f, 100.f)));
    }
    if (_mode == SculptMode::Dig || _mode == SculptMode::Build)
      e.param = WithShape(e.param, _shape);
    else if (_mode == SculptMode::Smooth)
      e.param = WithSmoothPasses(e.param, uint8_t(_smoothPasses));      // CONTRACT v3.11: bits 9-10
    return e;
  }

  // pushpull.rs csg
  Edit EverwoodTool::pushPullSample(AimHit const& hit) const
  {
    bool const pull = _mode == SculptMode::Pull;
    glm::vec3 const dir = _pushAlongView ? -_viewDirWow : hit.normal;
    float const step = stepYd(_radius, _pushStrength);
    float const k = pull ? _radius - step : step - _radius;
    Edit e;
    setPos(e, hit.pos + dir * k);
    e.radius = _radius;
    e.mode = pull ? MODE_DIG : MODE_BUILD;
    e.strength = 255;
    e.param = WithShape(0, _shape);
    return e;
  }

  // pushpull.rs sample, the softening pass
  Edit EverwoodTool::softenSample(glm::vec3 const& centre) const
  {
    Edit e;
    setPos(e, centre);
    e.radius = std::clamp(_radius * 1.25f, 0.5f, 16.f);
    e.mode = MODE_SMOOTH;
    e.strength = strengthU8(60.f);
    e.param = SoftParam(_pushEdge == 2 ? FALLOFF_LINEAR : FALLOFF_SMOOTH, 0);
    return e;
  }

  // eme tools/paint.rs sample, Voxel3D: CONTRACT v3.10 mode 7 from the air side of the hit
  std::optional<Edit> EverwoodTool::paintSample(AimHit const& hit, QString* why)
  {
    if (!_paintReveal && _paintTexture < 0)
    {
      *why = "Pick a cave texture first.";
      return std::nullopt;
    }
    if (_layer->editedWeightAt(hit) < PAINT_EDITED_MIN)
    {
      *why = "Untouched ground: use the normal Texture Paint tool there. Paint (caves) paints dug or built voxel faces.";
      return std::nullopt;
    }
    Edit e;
    setPos(e, hit.pos + hit.normal * 0.5f);
    e.radius = std::clamp(_paintRadius, PAINT_RADIUS_MIN, PAINT3_MAX_RADIUS);
    e.mode = MODE_PAINT3;
    e.strength = strengthU8(std::clamp(_paintOpacity * pressureScale(), 1.f, 100.f));
    Paint3Param p;
    // REVEAL ignores the id: it works with no texture selected.
    p.id = uint16_t(std::max(_paintTexture, 0));
    p.falloff = _paintFalloff;
    p.erase = _paintErase && !_paintReveal;
    p.reveal = _paintReveal;
    p.hard = _paintHard;           // v3.11: -1 = the falloff above, 0..3 = the hardness core
    e.param = p.ToParam();
    return e;
  }

  float EverwoodTool::pressureScale() const
  {
    if (!_usePressure)
      return 1.f;
    if (_fakePressure > 0.f)
      return _fakePressure;
    auto* tablet = Noggit::TabletManager::instance();
    if (!tablet->isActive())
      return 1.f;
    // A pen hovering reports 0: keep a floor so a light touch still leaves a mark.
    return std::clamp(float(tablet->pressure()), 0.05f, 1.f);
  }

  // Paint (caves) spray: the dab's centre sits 0.5 yd off the surface along its normal; the small dabs are scattered
  // uniformly (sqrt radial law) in a disc of _spraySize on the plane through that centre, then snapped to the surface. A xorshift32 seeded per
  // stroke, so a stroke replays the same. Every emitted dab is an ordinary valid mode-7 edit.
  void EverwoodTool::sprayDabs(Edit const& dab, std::vector<Edit>& out)
  {
    auto next = [this]
    {
      uint32_t x = _sprayState;
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      _sprayState = x ? x : 0x9E3779B9u;
      return float(_sprayState >> 8) / 16777216.f;     // [0, 1)
    };
    glm::vec3 n = _aim ? _aim->normal : glm::vec3(0.f, 0.f, 1.f);
    if (!(glm::length(n) > 1e-4f))
      n = glm::vec3(0.f, 0.f, 1.f);
    n = glm::normalize(n);
    glm::vec3 const helper = std::fabs(n.z) < 0.9f ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(1.f, 0.f, 0.f);
    glm::vec3 const u = glm::normalize(glm::cross(n, helper));
    glm::vec3 const v = glm::cross(n, u);
    glm::vec3 const centre = getPos(dab);
    for (int i = 0; i < _sprayDensity; ++i)
    {
      float const rho = _spraySize * std::sqrt(next());
      float const th = 6.2831853f * next();
      // The point on the aimed plane, snapped to the surface below it (a dab hangs 0.5 yd above the voxel face it
      // paints; on a curved cave floor the plane itself would float out of reach). A scatter point with no
      // painted-edit surface within reach is dropped: it would be an empty dab.
      glm::vec3 const p = centre + (u * std::cos(th) + v * std::sin(th)) * rho;
      auto const hit = _layer->raycast(p + n * 3.f, -n, 8.f);
      if (!hit || _layer->editedWeightAt(*hit) < PAINT_EDITED_MIN)
        continue;
      Edit e = dab;
      setPos(e, hit->pos + hit->normal * 0.5f);
      out.push_back(e);
    }
  }

  // paint3.rs would_refuse3: the whole dab is refused when a block it touches already has 4 other textures.
  bool EverwoodTool::paintRefused(Edit const& e, QString* why)
  {
    std::vector<Key> const refusing = _layer->refusingChunks3(e);
    if (refusing.empty())
      return false;
    *why = QString("This 16 yd block already has 4 cave textures (the limit). Erase one, or paint one of its own. Block %1 %2 %3.")
             .arg(refusing[0][0]).arg(refusing[0][1]).arg(refusing[0][2]);
    return true;
  }

  std::string EverwoodTool::paintRay(glm::vec3 const& origin, glm::vec3 const& dir)
  {
    auto const hit = _layer->raycast(origin, dir, AIM_RANGE);
    if (!hit)
      return "no voxel surface on the ray";
    QString why;
    auto const e = paintSample(*hit, &why);
    if (!e)
      return why.toStdString();
    if (paintRefused(*e, &why))
      return why.toStdString();
    applyStroke({ *e });
    return {};
  }

  // ------------------------------------------------------------------------------------------- strokes
  void EverwoodTool::openAction(int modality)
  {
    // One stroke, one undo step: beginAction hands back an action that is still open (a previous stroke's,
    // closed only on the next tick), and the stroke would join its undo step.
    if (NOGGIT_CUR_ACTION)
      NOGGIT_ACTION_MGR->endAction();
    Action* action = NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eEVERWOOD_VOXEL, modality);
    // beginAction dropped Noggit's redo steps; the layer drops the strokes they would have brought back.
    _layer->dropUndone();
    action->addFlags(ActionFlags::eEVERWOOD_VOXEL);
    std::weak_ptr<EverwoodLayer> weak = _layer;
    uint16_t const stroke = _strokeId;
    action->addEverwoodStroke([weak, stroke, this](bool redo)
    {
      if (auto layer = weak.lock())
      {
        layer->setStrokeUndone(stroke, !redo);
        updateStatus();
      }
    });
  }

  void EverwoodTool::commit(std::vector<Edit> const& editsIn)
  {
    std::vector<Edit> sprayed;
    bool const spraying = _spray && _kind == Kind::Paint;
    if (spraying)
    {
      for (Edit const& e : editsIn)
      {
        if (e.mode == MODE_PAINT3 && _strokeHeld)
          sprayDabs(e, sprayed);
        else
          sprayed.push_back(e);
      }
    }
    std::vector<Edit> const& edits = spraying ? sprayed : editsIn;
    for (Edit const& e : edits)
    {
      if (e.mode == MODE_PAINT3 && _strokeHeld)
      {
        QString why;
        if (paintRefused(e, &why))
        {
          setHint(why);
          continue;
        }
        // A paint stroke opens its undo step with its first dab: a stroke of refused dabs leaves none.
        if (!_actionOpen)
        {
          openAction(ActionModalityControllers::eLMB);
          _actionOpen = true;
        }
        setHint({});
      }
      _layer->addEdit(e, _strokeId);
      if (_strokeHeld && e.mode == MODE_SMOOTH && _kind == Kind::Sculpt && _mode == SculptMode::Smooth)
        extendFloaterBox(e);
    }
  }

  void EverwoodTool::beginStroke(AimHit const& hit)
  {
    _strokeHeld = true;
    _strokeId = _layer->beginStroke();
    _sprayState = 0x9E3779B9u ^ (uint32_t(_strokeId) * 2654435761u);     // seeded per stroke
    _flattenPlane = hit.pos.z + _offset;
    _sentRay.reset();
    _strokeStartMs = _nowMs;
    _strokeTicks = 0;
    _lastCentre.reset();
    _softenNext = false;
    _floaterBoxValid = false;
    _floaterBalls.clear();

    if (_kind == Kind::Paint)
    {
      // Like sculpt (eme stroke.rs: resend when the centre moved radius / 2 or every 125 ms held still), but
      // a refused dab sends nothing and the stroke goes on.
      _actionOpen = false;
      std::vector<Edit> out;
      QString why;
      if (auto const first = paintSample(hit, &why))
        _builder.begin(EverwoodStrokeConfig{}, _nowMs, *first, out);
      else
      {
        setHint(why);
        _builder.beginEmpty(EverwoodStrokeConfig{});
      }
      if (!out.empty())
        _sentRay = _cursorRay;
      commit(out);
      return;
    }

    openAction(ActionModalityControllers::eLMB);

    bool const pushPull = _kind == Kind::Sculpt && (_mode == SculptMode::Push || _mode == SculptMode::Pull);
    EverwoodStrokeConfig config;
    if (pushPull)
    {
      config.planeMoves = false;
      config.hardDepthRadii = 8.f;
    }

    Edit const first = pushPull ? pushPullSample(hit) : sculptSample(hit);
    if (pushPull)
    {
      _lastCentre = getPos(first);
      _softenNext = _pushEdge != 0;
    }
    std::vector<Edit> out;
    _builder.begin(config, _nowMs, first, out);
    if (!out.empty())
      _sentRay = _cursorRay;
    commit(out);
  }

  void EverwoodTool::endStroke()
  {
    std::vector<Edit> out;
    bool const pushPull = _kind == Kind::Sculpt && (_mode == SculptMode::Push || _mode == SculptMode::Pull);
    std::optional<Edit> last;
    if (_aim && _kind == Kind::Paint)
    {
      QString why;
      last = paintSample(*_aim, &why);
    }
    else if (_aim && !pushPull)
      last = sculptSample(*_aim);
    bool const moved = !_sentRay || *_sentRay != _cursorRay;
    _builder.end(_nowMs, last, out, moved, pushPull);
    commit(out);
    // Sculpt > Smooth: the floaters under the brush go in the same stroke (one undo step).
    if (_floaterBoxValid && _smoothFloaters && _kind == Kind::Sculpt && _mode == SculptMode::Smooth)
      removeFloatersUnderBrush();
    _floaterBoxValid = false;
    _floaterBalls.clear();
    _strokeHeld = false;
    _actionOpen = false;
    _layer->endStroke();
    updateStatus();
  }

  void EverwoodTool::onTick(float deltaTime, TickParameters const& params)
  {
    _nowMs += uint64_t(std::max(deltaTime, 0.f) * 1000.f);
    if (params.displayMode != display_mode::in_3D)
      return;

    if (_kind == Kind::Flow)
    {
      flowTick(params);
      return;
    }
    if (_flowHeld)
      flowEnd();

    updateAim();

    if (_kind == Kind::Bridge || _kind == Kind::Ramp)
      return;

    if (!_strokeHeld)
    {
      if (params.left_mouse && _aim)
        beginStroke(*_aim);
      return;
    }

    if (!params.left_mouse)
    {
      endStroke();
      return;
    }

    // A click: the first dab went out at press and the cursor has not moved since. The frame after an edit can
    // outlast the 125 ms resend interval, and re-aiming the same ray then lands in the dab's own crater (or on
    // its own paint) and sent a second dab per click. Held still past the click window, the stroke goes on.
    constexpr uint64_t CLICK_MS = 250;
    bool const pushPull = _kind == Kind::Sculpt && (_mode == SculptMode::Push || _mode == SculptMode::Pull);
    // Frames count too: one stalled frame (tile streaming, a GPU flush) can outlast the window by itself.
    ++_strokeTicks;
    if (!pushPull && _sentRay && *_sentRay == _cursorRay && (_nowMs - _strokeStartMs < CLICK_MS || _strokeTicks < 4))
      return;

    std::vector<Edit> out;
    if (_aim)
    {
      if (pushPull)
      {
        Edit const sample = (_pushEdge != 0 && _softenNext && _lastCentre) ? softenSample(*_lastCentre) : pushPullSample(*_aim);
        _builder.update(_nowMs, sample, out);
        for (Edit const& e : out)
        {
          if (e.mode == MODE_SMOOTH)
            _softenNext = false;
          else
          {
            _lastCentre = getPos(e);
            _softenNext = _pushEdge != 0;
          }
        }
      }
      else if (_kind == Kind::Paint)
      {
        QString why;
        if (auto const sample = paintSample(*_aim, &why))
          _builder.update(_nowMs, *sample, out);
        else
        {
          setHint(why);
          _builder.tick(_nowMs, out);
        }
      }
      else
      {
        _builder.update(_nowMs, sculptSample(*_aim), out);
      }
    }
    else
    {
      _builder.tick(_nowMs, out);
    }
    if (!out.empty())
      _sentRay = _cursorRay;
    commit(out);
    if (!out.empty())
      updateStatus();
  }

  void EverwoodTool::onMousePress(MousePressParameters const& params)
  {
    if (params.button != Qt::LeftButton || (_kind != Kind::Bridge && _kind != Kind::Ramp))
      return;
    updateAim();
    if (!_aim)
      return;
    if (_kind == Kind::Ramp)
    {
      // A click on the last point again finishes the path (like the bridge's second click).
      if (_rampPoints.size() >= 2 && glm::distance(glm::vec2(_aim->pos), glm::vec2(_rampPoints.back())) < 1.5f)
        confirmRamp();
      else
        addRampPoint(_aim->pos);
      return;
    }
    if (_bridgeA && _bridgeB)
      confirmBridge();
    else
      setBridgePoint(_aim->pos);
  }

  void EverwoodTool::setBridgePoint(glm::vec3 const& wow)
  {
    if (!_bridgeA)
      _bridgeA = wow;
    else
      _bridgeB = wow;
    updatePanel();
  }

  void EverwoodTool::cancelBridge()
  {
    _bridgeA.reset();
    _bridgeB.reset();
    updatePanel();
  }

  // bridge.rs EmeBridgeSpec::edits + start
  void EverwoodTool::confirmBridge()
  {
    if (!_bridgeA || !_bridgeB)
      return;
    BridgeSpec const spec = bridgeSpec();
    float const len = glm::distance(spec.a, spec.b);
    if (len < 1.f || len > BRIDGE_MAX_LENGTH)
    {
      _bridgeLabel->setText(QString("The path is %1 yd long; it must be 1 to %2 yd.").arg(double(len), 0, 'f', 0).arg(double(BRIDGE_MAX_LENGTH)));
      return;
    }

    std::vector<Edit> edits;
    int const n = spec.points();
    float const r = spec.radius;
    uint16_t const param = WithShape(0, spec.shape);
    auto sample = [&](glm::vec3 const& p, uint8_t mode)
    {
      Edit e;
      setPos(e, p);
      e.radius = r;
      e.mode = mode;
      e.strength = 255;
      e.param = param;
      return e;
    };
    for (int i = 0; i < n; ++i)
    {
      glm::vec3 const p = spec.point(i, n);
      edits.push_back(spec.tunnel ? sample(p + glm::vec3(0, 0, r), MODE_DIG) : sample(p - glm::vec3(0, 0, r), MODE_BUILD));
    }
    if (spec.carvesUnderside())
    {
      for (int i = 0; i < n; ++i)
      {
        glm::vec3 const p = spec.point(i, n);
        float const zc = p.z - spec.thickness - r;
        // The carve spans zc +- r: keep it clear of the real ground, over the span only, never into the banks.
        float ground;
        if (_layer->rawGround(p.x, p.y, ground) && ground < zc - r - 0.5f)
          edits.push_back(sample({ p.x, p.y, zc }, MODE_DIG));
      }
    }
    if (int(edits.size()) > BRIDGE_MAX_EDITS)
    {
      _bridgeLabel->setText(QString("The path needs %1 edits; the limit is %2. Use a larger radius or a shorter path.").arg(edits.size()).arg(BRIDGE_MAX_EDITS));
      return;
    }

    applyStroke(edits);
    cancelBridge();
  }

  // ------------------------------------------------------------------------------------ remove floaters
  namespace
  {
    QString floaterText(FloaterPlan const& plan)
    {
      if (!plan.found)
        return "No floaters found.";
      QString s = QString("Removed %1 floaters (%2 edits)").arg(plan.removable).arg(plan.digs.size());
      if (plan.skippedUnsafe)
        s += QString(", left %1 (no safe way to dig without touching the ground)").arg(plan.skippedUnsafe);
      return s + ".";
    }
  }

  std::string EverwoodTool::removeFloaters()
  {
    if (_strokeHeld)
      return "busy";
    FloaterPlan const plan = _layer->planFloaters(uint32_t(std::max(_floaterMax, 1)), FloaterRegion{});
    if (!plan.digs.empty())
      applyStroke(plan.digs);
    _floaterText = floaterText(plan);
    updatePanel();
    return _floaterText.toStdString();
  }

  void EverwoodTool::extendFloaterBox(Edit const& e)
  {
    Key lo, hi;
    e.Bbox(lo, hi);
    _floaterBalls.push_back({ e.pos[0] / VOXEL_SIZE, e.pos[1] / VOXEL_SIZE, e.pos[2] / VOXEL_SIZE, e.radius / VOXEL_SIZE });
    if (!_floaterBoxValid)
    {
      _floaterLo = lo;
      _floaterHi = hi;
      _floaterBoxValid = true;
      return;
    }
    for (int a = 0; a < 3; ++a)
    {
      _floaterLo[a] = std::min(_floaterLo[a], lo[a]);
      _floaterHi[a] = std::max(_floaterHi[a], hi[a]);
    }
  }

  // The smooth stroke's own undo step: the dig edits carry the stroke's id.
  void EverwoodTool::removeFloatersUnderBrush()
  {
    FloaterRegion region;
    region.all = false;
    region.lo = _floaterLo;
    region.hi = _floaterHi;
    region.balls = _floaterBalls;
    FloaterPlan const plan = _layer->planFloaters(uint32_t(std::max(_floaterMax, 1)), region);
    for (Edit const& dig : plan.digs)
      _layer->addEdit(dig, _strokeId);
    _floaterText = floaterText(plan);
    updatePanel();
  }

  // ------------------------------------------------------------------------------------- ramp / path
  // The polyline of clicked ground points (WoW axes) with its arclength parameter. Heights: follow the clicked
  // points' own heights (linear between them) or a fixed grade from the first point.
  struct EverwoodTool::RampPath
  {
    std::vector<glm::vec3> p;
    std::vector<float> s;                 // arclength at each point (xy)
    std::vector<glm::vec2> dir;           // unit xy direction of each segment
    float length = 0.f;
    bool fixedAngle = false;
    float tanA = 0.f;

    bool build(std::vector<glm::vec3> const& pts, bool fixed, float angleDeg)
    {
      p.clear();
      s.clear();
      dir.clear();
      for (glm::vec3 const& q : pts)
        if (p.empty() || glm::distance(glm::vec2(q), glm::vec2(p.back())) >= 0.25f)
          p.push_back(q);
      if (p.size() < 2)
        return false;
      s.push_back(0.f);
      for (size_t i = 1; i < p.size(); ++i)
      {
        glm::vec2 const d = glm::vec2(p[i]) - glm::vec2(p[i - 1]);
        float const l = glm::length(d);
        s.push_back(s.back() + l);
        dir.push_back(d / l);
      }
      length = s.back();
      fixedAngle = fixed;
      tanA = std::tan(angleDeg * 0.017453292f);
      return true;
    }

    size_t segment(float u) const
    {
      size_t i = 0;
      while (i + 2 < p.size() && u > s[i + 1])
        ++i;
      return i;
    }

    // The path's xy at arclength u (extended along the first / last segment beyond the ends) and its direction.
    glm::vec2 xyAt(float u, glm::vec2& d) const
    {
      size_t const i = segment(u);
      d = dir[i];
      return glm::vec2(p[i]) + d * (u - s[i]);
    }

    float heightAt(float u) const
    {
      u = std::clamp(u, 0.f, length);
      if (fixedAngle)
        return p[0].z + u * tanA;
      size_t const i = segment(u);
      float const t = s[i + 1] > s[i] ? (u - s[i]) / (s[i + 1] - s[i]) : 0.f;
      return p[i].z + (p[i + 1].z - p[i].z) * t;
    }
  };

  bool EverwoodTool::rampGround(float x, float y, float zHint, float& z) const
  {
    if (auto const hit = _layer->raycast(glm::vec3(x, y, zHint + 80.f), glm::vec3(0.f, 0.f, -1.f), 240.f))
    {
      z = hit->pos.z;
      return true;
    }
    return _layer->rawGround(x, y, z);
  }

  void EverwoodTool::addRampPoint(glm::vec3 const& wow)
  {
    _rampPoints.push_back(wow);
    updatePanel();
  }

  void EverwoodTool::cancelRamp()
  {
    _rampPoints.clear();
    updatePanel();
  }

  // The edit scheme. Every edit is an ordinary mode 6 (LEVEL) or mode 2 (smooth) edit the game already replays.
  //
  // LEVEL clamps the field into a window around a HORIZONTAL plane (offset = param, in cm from pos.z): flat inside
  // the dab's core (50 % of its radius), then a bank whose slope is the edge softness (60 / 45 / 30 deg) out to
  // the radius. It cuts high ground and fills low ground, so dabs laid along the path, each with the plane height
  // of its own place, make a ramp. Dabs applied later overwrite the ground of earlier ones inside their core, so
  // the surface at path position s is owned by the dab centred `inner` (the core radius) ahead of it; every
  // dab's plane is therefore the path height at (its arclength - inner), and the dabs run from arclength `inner`
  // to L + inner: the surface is exactly the requested slope from A to B (a flat landing of about `inner` follows
  // B; the strip starts at A). Pass 1: two wide bank tracks (radius 2 * bank width) just outside the flat strip;
  // pass 2: the flat strip itself with small dabs (radius W / 2 clamped 2..6) on an odd number of tracks across
  // the width, centre line included; so the centre line is not biased and the cross-slope error stays under
  // 0.2 * inner * tan(slope). A dab's vertical box is pos.z +- r: where the ground under a dab is farther from the
  // plane than that, the dab is stacked (same plane, same xy, one per vertical slab) so the whole cut or fill is
  // reached. Last: one light smooth pass along the strip.
  std::vector<Edit> EverwoodTool::rampEdits(std::vector<glm::vec3> const& points, QString* why) const
  {
    RampPath path;
    if (!path.build(points, _rampSlope == 1, _rampAngle))
    {
      *why = "A ramp needs two points at least 0.25 yd apart.";
      return {};
    }
    float const L = path.length;
    if (L < 2.f || L > RAMP_MAX_LENGTH)
    {
      *why = QString("The path is %1 yd long; it must be 2 to %2 yd.").arg(double(L), 0, 'f', 0).arg(double(RAMP_MAX_LENGTH));
      return {};
    }
    float const W = std::clamp(_rampWidth, 1.f, 32.f);
    float const bw = _rampEdge == 0 ? 1.f : (_rampEdge == 1 ? 2.f : 4.f);
    uint16_t const bankCode = _rampEdge == 0 ? 2 : (_rampEdge == 1 ? 0 : 1);   // v3.9 slope bits: 0 = 45, 1 = 30, 2 = 60 deg
    uint8_t const strength = strengthU8(_rampStrength);

    std::vector<Edit> edits;
    auto level = [&](float u, float lateral, float r, float inner)
    {
      glm::vec2 d;
      glm::vec2 c = path.xyAt(u, d);
      c += glm::vec2(-d.y, d.x) * lateral;
      float const plane = path.heightAt(u - inner);
      // The ground under the dab (its centre and four points of its disc): LEVEL only reaches pos.z +- r, so a deep
      // cut or fill is laid out as a stack of dabs of the same plane and xy, one per vertical slab (the clamp is
      // per sample, so stacked slabs are the same as one tall dab).
      float gLo = plane, gHi = plane;
      glm::vec2 const side(-d.y, d.x);
      glm::vec2 const probes[5] = { c, c + d * (0.7f * r), c - d * (0.7f * r), c + side * (0.7f * r), c - side * (0.7f * r) };
      for (glm::vec2 const& q : probes)
      {
        float g;
        if (rampGround(q.x, q.y, plane, g))
        {
          gLo = std::min(gLo, g);
          gHi = std::max(gHi, g);
        }
      }
      float const zlo = std::max(gLo - 0.75f, plane - 18.f), zhi = std::min(gHi + 0.75f, plane + 18.f);
      float const slab = 2.f * (r - 0.5f);
      int const slabs = std::max(1, int(std::ceil((zhi - zlo) / slab)));
      for (int j = 0; j < slabs; ++j)
      {
        float const zc = slabs == 1 ? std::clamp((zlo + zhi) * 0.5f, plane - (r - 0.5f), plane + (r - 0.5f)) : zlo + (zhi - zlo) * (float(j) + 0.5f) / float(slabs);
        int const off = std::clamp(int(std::lround((plane - zc) * 100.f)), -2047, 2047);
        Edit e;
        e.pos[0] = c.x;
        e.pos[1] = c.y;
        e.pos[2] = zc;
        e.radius = r;
        e.mode = MODE_LEVEL;
        e.strength = strength;
        e.param = uint16_t((off & 0x0fff) | (bankCode << 12));     // core bits 14-15 = 0: 50 %
        edits.push_back(e);
      }
    };
    auto pass = [&](float r, float inner, float spacing, std::vector<float> const& lats)
    {
      int const n = std::max(1, int(std::ceil(L / (spacing * inner))));
      for (int k = 0; k <= n; ++k)
      {
        float const u = inner + L * float(k) / float(n);
        for (float lat : lats)
          level(u, lat, r, inner);
      }
    };

    // Pass 1: the banks.
    float const latBank = std::max(W * 0.5f - bw, 0.f);
    pass(2.f * bw, bw, 0.8f, latBank < 0.05f ? std::vector<float>{ 0.f } : std::vector<float>{ -latBank, latBank });
    // Pass 2: the flat strip.
    float const rm = std::clamp(W * 0.5f, 2.f, 6.f);
    float const innerM = rm * 0.5f;
    float const half = std::max(W * 0.5f - innerM, 0.f);
    std::vector<float> lats{ 0.f };
    if (half >= 0.05f)
    {
      int const tracks = 2 * int(std::ceil(half / (1.2f * innerM))) + 1;
      lats.clear();
      for (int j = 0; j < tracks; ++j)
        lats.push_back(-half + 2.f * half * float(j) / float(tracks - 1));
    }
    pass(rm, innerM, 0.6f, lats);

    if (_rampSmooth)
    {
      float const rs = std::clamp(W * 0.5f + bw + 1.f, 3.f, 16.f);
      int const n = std::max(1, int(std::ceil(L / (rs * 0.5f))));
      for (int k = 0; k <= n; ++k)
      {
        float const u = L * float(k) / float(n);
        glm::vec2 d;
        glm::vec2 const c = path.xyAt(u, d);
        Edit e;
        e.pos[0] = c.x;
        e.pos[1] = c.y;
        e.pos[2] = path.heightAt(u);
        e.radius = rs;
        e.mode = MODE_SMOOTH;
        e.strength = strengthU8(50.f);
        e.param = WithSmoothPasses(SoftParam(FALLOFF_SMOOTH, 0), 0);
        edits.push_back(e);
      }
    }
    int const cap = _rampCap > 0 ? _rampCap : RAMP_MAX_EDITS;
    double work = 0.0;       // voxels in the edits' boxes: the cost of applying them follows this and the count
    for (Edit const& e : edits)
    {
      double const side = 4.0 * double(e.radius) + 3.0;
      work += side * side * side;
    }
    _rampWork = work;
    _rampEstimateMs = double(edits.size()) * RAMP_MS_PER_EDIT + work * RAMP_MS_PER_VOXEL;
    if (int(edits.size()) > cap || (_rampCap <= 0 && _rampEstimateMs > RAMP_MAX_MS))
    {
      *why = QString("The path needs %1 edits (about %2 s to build); the limit is %3 edits or about %4 s. "
                     "Split it into shorter paths, or use a narrower width.")
               .arg(edits.size()).arg(_rampEstimateMs / 1000.0, 0, 'f', 1).arg(cap).arg(RAMP_MAX_MS / 1000.0, 0, 'f', 0);
      return {};
    }
    return edits;
  }

  std::string EverwoodTool::confirmRamp()
  {
    if (_rampPoints.size() < 2)
      return "Click at least two points first.";
    QString why;
    std::vector<Edit> const edits = rampEdits(_rampPoints, &why);
    if (edits.empty())
    {
      if (_bridgeLabel)
        _bridgeLabel->setText(why);
      return why.toStdString();
    }
    // A big ramp takes a moment: say so (wait cursor + the panel line) so the editor does not look frozen.
    bool const slow = _rampEstimateMs >= RAMP_FEEDBACK_MS;
    if (slow)
    {
      QGuiApplication::setOverrideCursor(Qt::WaitCursor);
      if (_bridgeLabel)
      {
        _bridgeLabel->setText(QString("Building ramp... %1 edits").arg(edits.size()));
        _bridgeLabel->repaint();
      }
    }
    applyStroke(edits);
    if (slow)
      QGuiApplication::restoreOverrideCursor();
    _lastRampEdits = edits.size();
    _rampPoints.clear();
    updatePanel();
    if (_bridgeLabel)
      _bridgeLabel->setText(QString("Ramp built: %1 edits, one undo step.").arg(edits.size()));
    return {};
  }

  void EverwoodTool::drawRamp(glm::mat4x4 const& mvp)
  {
    glm::vec4 const color(0.3f, 0.8f, 1.f, 1.f);
    std::vector<glm::vec3> pts = _rampPoints;
    if (_aim && (pts.empty() || glm::distance(glm::vec2(_aim->pos), glm::vec2(pts.back())) >= 0.25f))
      pts.push_back(_aim->pos);
    for (glm::vec3 const& q : _rampPoints)
      _sphere.draw(mvp, wowToNoggit(q), color, 0.4f, 10, 8, 1.f);
    if (_aim)
      _sphere.draw(mvp, wowToNoggit(_aim->pos), glm::vec4(1.f), 0.3f, 8, 6, 1.f);

    RampPath path;
    if (!path.build(pts, _rampSlope == 1, _rampAngle))
      return;
    float const W = std::clamp(_rampWidth, 1.f, 32.f);
    float const bw = _rampEdge == 0 ? 1.f : (_rampEdge == 1 ? 2.f : 4.f);
    std::vector<glm::vec3> centre, flatL, flatR, bankL, bankR;
    int const n = std::max(1, int(std::ceil(path.length)));
    for (int k = 0; k <= n; ++k)
    {
      float const u = path.length * float(k) / float(n);
      glm::vec2 d;
      glm::vec2 const c = path.xyAt(u, d);
      glm::vec2 const nrm(-d.y, d.x);
      float const z = path.heightAt(u) + 0.25f;
      auto at = [&](float lat) { return wowToNoggit(glm::vec3(c + nrm * lat, z)); };
      centre.push_back(at(0.f));
      flatL.push_back(at(W * 0.5f));
      flatR.push_back(at(-W * 0.5f));
      bankL.push_back(at(W * 0.5f + bw));
      bankR.push_back(at(-W * 0.5f - bw));
    }
    std::vector<glm::vec3> seg;
    auto strip = [&](std::vector<glm::vec3> const& line)
    {
      for (size_t i = 1; i < line.size(); ++i)
      {
        seg.push_back(line[i - 1]);
        seg.push_back(line[i]);
      }
    };
    strip(centre);
    strip(flatL);
    strip(flatR);
    _line.drawSegments(mvp, seg, color);
    seg.clear();
    strip(bankL);
    strip(bankR);
    _line.drawSegments(mvp, seg, glm::vec4(0.3f, 0.8f, 1.f, 0.45f));
  }

  void EverwoodTool::applyStroke(std::vector<Edit> const& edits)
  {
    if (NOGGIT_CUR_ACTION)
      NOGGIT_ACTION_MGR->endAction();
    _strokeId = _layer->beginStroke();
    openAction(ActionModalityControllers::eNONE);
    _layer->beginBatch();
    commit(edits);
    _layer->endBatch();
    NOGGIT_ACTION_MGR->endAction();
    _layer->endStroke();
    updateStatus();
  }

  void EverwoodTool::undo()
  {
    if (!NOGGIT_CUR_ACTION)
      NOGGIT_ACTION_MGR->undo();
    updateStatus();
  }

  void EverwoodTool::redo()
  {
    if (!NOGGIT_CUR_ACTION)
      NOGGIT_ACTION_MGR->redo();
    updateStatus();
  }

  std::string EverwoodTool::exportEdits()
  {
    return _layer->exportEdits(Noggit::Project::CurrentProject::get()->ProjectName);
  }

  void EverwoodTool::onMouseWheel(MouseWheelParameters const& params)
  {
    if (!params.mod_alt_down)
      return;
    if (_kind == Kind::Flow)
    {
      _flowRadiusBox->setValue(std::clamp(double(_flowRadius + (params.event.angleDelta().y() > 0 ? 2.f : -2.f)),
                                          double(FLOW_RADIUS_MIN), double(FLOW_RADIUS_MAX)));
      return;
    }
    float const step = params.event.angleDelta().y() > 0 ? 0.5f : -0.5f;
    if (_kind == Kind::Ramp)
    {
      _rampWidthBox->setValue(std::clamp(double(_rampWidth + step * 2.f), 1.0, 32.0));
      return;
    }
    _radiusBox->setValue(std::clamp(double(brushRadius() + step), 0.5, _kind == Kind::Paint ? double(PAINT3_MAX_RADIUS) : 16.0));
  }

  // Where the brush's weight is full: sculpt soft brushes (Dig / Build / Smooth, falloff smooth or linear) weigh 1 up
  // to radius * hardness %, Flatten up to radius - max(radius / 4, 1) (world.rs MODE_FLATTEN band), Paint (caves)
  // with a hard core up to radius * core. A flat falloff, a hard edge, Push / Pull and the smooth Paint falloff
  // have no such radius: 0.
  EverwoodTool::BrushPreview EverwoodTool::brushPreview() const
  {
    BrushPreview b;
    if (_kind == Kind::Paint)
    {
      b.outer = std::clamp(_paintRadius, PAINT_RADIUS_MIN, PAINT3_MAX_RADIUS);
      if (_paintHard >= 0 && _paintHard < 4)
        b.inner = b.outer * PAINT3_CORES[_paintHard];
      if (_spray)
        b.spray = _spraySize;
    }
    else if (_kind == Kind::Sculpt)
    {
      b.outer = _radius;
      bool const soft = !_hard && (_mode == SculptMode::Dig || _mode == SculptMode::Build || _mode == SculptMode::Smooth);
      float const h = std::clamp(std::round(_hardness), 0.f, 100.f);
      if (soft && _falloff != FALLOFF_FLAT && h >= 1.f)
        b.inner = _radius * h / 100.f;
    }
    else if (_kind == Kind::Flatten)
    {
      b.outer = _radius;
      float const core = _radius - std::max(_radius * 0.25f, 1.f);
      if (core > 0.05f)
        b.inner = core;
    }
    else if (_kind == Kind::Bridge)
    {
      b.outer = bridgeSpec().radius;
    }
    return b;
  }

  void EverwoodTool::postRender()
  {
    if (mapView()->get_editing_mode() != editing_mode::everwood)
      return;

    struct DrawProbe   // smoke "prof": the cost of the cursor / path drawing per frame
    {
      std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
      ~DrawProbe() { profileAdd("brush.draw", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
    } const drawProbe;

    glm::mat4x4 const modelView = mapView()->model_view();
    glm::mat4x4 const projection = mapView()->projection();
    glm::mat4x4 const mvp = projection * modelView;

    auto drawBrush = [&](glm::vec3 const& wow, float radius, uint8_t shape, glm::vec4 const& color, float alpha = 0.25f)
    {
      glm::vec3 const p = wowToNoggit(wow);
      if (shape == SHAPE_SPHERE)
      {
        _sphere.draw(mvp, p, color, radius, 24, 14, alpha);
        return;
      }
      // Both other shapes are boxes in WoW axes; for the cylinder this is its bounding box.
      _box.draw(modelView, projection, glm::mat4x4(1.f), color, p - glm::vec3(radius), p + glm::vec3(radius));
    };

    if (_kind == Kind::Flow)
    {
      renderFlow(mvp);
      return;
    }

    if (_kind == Kind::Ramp)
    {
      drawRamp(mvp);
      return;
    }

    if (_kind == Kind::Bridge)
    {
      BridgeSpec const spec = bridgeSpec();
      glm::vec4 const color(0.3f, 0.8f, 1.f, 1.f);
      if (_bridgeA)
        drawBrush(*_bridgeA, spec.radius, spec.shape, color);
      if (_bridgeB)
      {
        drawBrush(*_bridgeB, spec.radius, spec.shape, color);
        int const n = spec.points();
        for (int i = 0; i < n; i += std::max(1, n / 12))
          _sphere.draw(mvp, wowToNoggit(spec.point(i, n)), color, 0.25f, 8, 6, 1.f);
      }
      if (_aim && !(_bridgeA && _bridgeB))
        drawBrush(_aim->pos, spec.radius, spec.shape, glm::vec4(1.f, 1.f, 1.f, 1.f));
      return;
    }

    if (_aim && _kind == Kind::Paint)
    {
      // The dab's flood origin, on the air side of the face; grey where the hit would be refused.
      bool const ok = _layer->editedWeightAt(*_aim) >= PAINT_EDITED_MIN && (_paintReveal || _paintTexture >= 0);
      glm::vec4 const color = !ok ? glm::vec4(0.6f, 0.6f, 0.6f, 1.f)
                            : (_paintErase || _paintReveal ? glm::vec4(1.f, 0.5f, 0.9f, 1.f) : glm::vec4(0.75f, 0.45f, 1.f, 1.f));
      BrushPreview const pv = brushPreview();
      glm::vec3 const centre = _aim->pos + _aim->normal * 0.5f;
      drawBrush(centre, pv.outer, SHAPE_SPHERE, color);
      if (pv.inner > 0.f)
        drawBrush(centre, pv.inner, SHAPE_SPHERE, glm::vec4(glm::min(glm::vec3(color) * 1.25f + 0.1f, glm::vec3(1.f)), 1.f), 0.6f);
      if (pv.spray > 0.f)
      {
        // The spray's scatter disc: the plane through the dab centre, perpendicular to the aimed normal (the same
        // basis sprayDabs scatters in), as a ring of line segments that follows slopes and walls.
        glm::vec3 n = _aim->normal;
        if (!(glm::length(n) > 1e-4f))
          n = glm::vec3(0.f, 0.f, 1.f);
        n = glm::normalize(n);
        glm::vec3 const helper = std::fabs(n.z) < 0.9f ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(1.f, 0.f, 0.f);
        glm::vec3 const u = glm::normalize(glm::cross(n, helper));
        glm::vec3 const v = glm::cross(n, u);
        constexpr int RING = 48;
        _ring.clear();
        glm::vec3 prev = wowToNoggit(centre + u * pv.spray);
        for (int i = 1; i <= RING; ++i)
        {
          float const th = 6.2831853f * float(i) / float(RING);
          glm::vec3 const q = wowToNoggit(centre + (u * std::cos(th) + v * std::sin(th)) * pv.spray);
          _ring.push_back(prev);
          _ring.push_back(q);
          prev = q;
        }
        _line.drawSegments(mvp, _ring, glm::vec4(1.f, 1.f, 1.f, 1.f));
      }
    }
    else if (_aim)
    {
      bool const usesShape = _kind == Kind::Sculpt && _mode != SculptMode::Smooth;
      glm::vec4 color(1.f, 0.85f, 0.3f, 1.f);
      if (_kind == Kind::Sculpt)
      {
        if (_mode == SculptMode::Dig || _mode == SculptMode::Pull)
          color = glm::vec4(1.f, 0.4f, 0.3f, 1.f);
        else if (_mode == SculptMode::Build || _mode == SculptMode::Push)
          color = glm::vec4(0.4f, 1.f, 0.4f, 1.f);
        else
          color = glm::vec4(0.5f, 0.7f, 1.f, 1.f);
      }
      BrushPreview const pv = brushPreview();
      uint8_t const shape = usesShape ? _shape : uint8_t(SHAPE_SPHERE);
      drawBrush(_aim->pos, pv.outer, shape, color);
      if (pv.inner > 0.f)
        drawBrush(_aim->pos, pv.inner, shape, glm::vec4(glm::min(color.r * 1.2f + 0.1f, 1.f), glm::min(color.g * 1.2f + 0.1f, 1.f), glm::min(color.b * 1.2f + 0.1f, 1.f), 1.f), 0.6f);
    }
  }

  // ------------------------------------------------------------------------------------------ water flow
  // Coordinates: the contract's pos / dir are WoW world coordinates (x north, y west, z up). Noggit's internal
  // space is (ZEROPOINT - wow.y, wow.z, ZEROPOINT - wow.x); wowToNoggit / noggitToWow (EverwoodLayer.cpp, the
  // same axis swap the .gps coordinates use) convert points both ways. Directions are built in WoW axes from two
  // converted sample points, so no vector conversion is needed.
  void EverwoodTool::flowTick(TickParameters const& params)
  {
    ::World* world = mapView()->getWorld();

    _flowHit.reset();
    bool onWater = false;
    if (auto const liquid = world->intersectLiquid(mapView()->intersect_ray(), -1))
    {
      _flowHit = *liquid;
      onWater = true;
    }
    else if (!params.underMap)
      _flowHit = mapView()->cursorPosition();

    if (!params.left_mouse)
    {
      if (_flowHeld)
        flowEnd();
      return;
    }
    if (!_flowHit)
      return;

    glm::vec3 const wow = noggitToWow(*_flowHit);
    std::optional<float> const waterZ = onWater ? std::optional<float>(wow.z) : std::nullopt;

    if (!_flowHeld)
    {
      _flowHeld = true;
      _flowTouched.clear();
      _flowSamples = 0;
      _flowLastWow = wow;
      if (_flowErase)
        flowApply(wow, glm::vec2(0.f), waterZ);
      return;
    }

    if (_flowErase)
    {
      flowApply(wow, glm::vec2(0.f), waterZ);
      _flowLastWow = wow;
      return;
    }

    glm::vec2 const d(wow.x - _flowLastWow->x, wow.y - _flowLastWow->y);
    float const len = glm::length(d);
    if (len < FLOW_MIN_STEP)
      return;
    flowApply(wow, d / len, waterZ);
    _flowLastWow = wow;
  }

  // One drag sample: every chunk within the brush that has water gets (Paint) one flow point or (Erase) none.
  // Each touched chunk is registered with the current action first, so the stroke is one undo step.
  void EverwoodTool::flowApply(glm::vec3 const& centreWow, glm::vec2 const& dirWow, std::optional<float> hitWaterZ)
  {
    ::World* world = mapView()->getWorld();
    glm::vec3 const centre = wowToNoggit(centreWow);
    float const radius = std::clamp(_flowRadius, FLOW_RADIUS_MIN, FLOW_RADIUS_MAX);
    bool const erase = _flowErase;

    // Pass 1: which chunks qualify, and the water height (the hit's, else the mean of the chunks').
    float heightSum = 0.f;
    int heightCount = 0;
    world->for_all_chunks_in_range(centre, radius, [&](MapChunk* chunk)
    {
      ChunkWater* water = chunk->liquid_chunk();
      if (water && water->hasData(0) && (!erase || water->flowCount()))
      {
        heightSum += water->getMaxHeight();
        ++heightCount;
      }
      return false;
    });
    if (!heightCount)
      return;
    float const waterZ = hitWaterZ ? *hitWaterZ : heightSum / float(heightCount);

    mclq_flowvs slot{};
    slot.pos[0] = centreWow.x;
    slot.pos[1] = centreWow.y;
    slot.pos[2] = waterZ;
    slot.radius = radius;
    slot.dir[0] = dirWow.x;
    slot.dir[1] = dirWow.y;
    slot.dir[2] = 0.f;
    slot.velocity = std::clamp(_flowVelocity, 0.f, 8.f);
    slot.amplitude = std::clamp(_flowTurbulence, 0.f, 1.f);
    slot.frequency = 0.f;

    NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eCHUNKS_WATER, ActionModalityControllers::eLMB);

    // Pass 2: edit; a changed chunk marks its tile so it saves.
    world->for_all_chunks_in_range(centre, radius, [&](MapChunk* chunk)
    {
      ChunkWater* water = chunk->liquid_chunk();
      if (!water || !water->hasData(0) || (erase && !water->flowCount()))
        return false;
      NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
      if (erase)
        water->clearFlow();
      else
        water->setFlow(slot);
      water->tagUpdate();
      _flowTouched.insert(water);
      return true;
    });
    ++_flowSamples;
  }

  void EverwoodTool::flowEnd()
  {
    if (_flowTouched.empty())
      Log << "[everwood-flow] stroke changed nothing (no water under the brush)" << std::endl;
    else if (_flowErase)
      Log << "[everwood-flow] erased " << _flowTouched.size() << " chunks (" << _flowSamples << " samples, radius "
          << _flowRadius << " yd)" << std::endl;
    else
      Log << "[everwood-flow] painted " << _flowTouched.size() << " chunks (" << _flowSamples << " samples, radius "
          << _flowRadius << " yd, velocity " << _flowVelocity << " yd/s, turbulence " << _flowTurbulence << ")" << std::endl;
    _flowHeld = false;
    _flowLastWow.reset();
    _flowTouched.clear();
    _flowSamples = 0;
  }

  void EverwoodTool::renderFlow(glm::mat4x4 const& mvp)
  {
    // An arrow per flow point: shaft + two head strokes, at water height, in WoW axes then converted.
    std::vector<glm::vec3> seg;
    auto arrow = [&](mclq_flowvs const& f)
    {
      glm::vec2 const d(f.dir[0], f.dir[1]);
      float const dl = glm::length(d);
      if (dl < 1e-4f)
        return;
      glm::vec2 const u = d / dl;
      glm::vec2 const n(-u.y, u.x);
      float const length = 4.f + f.velocity * 2.f;
      float const lift = 0.2f;
      glm::vec2 const tail(f.pos[0], f.pos[1]);
      glm::vec2 const tip = tail + u * length;
      glm::vec2 const left = tip - u * 1.2f + n * 0.6f;
      glm::vec2 const right = tip - u * 1.2f - n * 0.6f;
      auto point = [&](glm::vec2 const& p) { return wowToNoggit({ p.x, p.y, f.pos[2] + lift }); };
      seg.push_back(point(tail)); seg.push_back(point(tip));
      seg.push_back(point(tip));  seg.push_back(point(left));
      seg.push_back(point(tip));  seg.push_back(point(right));
    };

    for (MapTile* tile : mapView()->getWorld()->mapIndex.loaded_tiles())
    {
      if (!tile || !tile->finishedLoading())
        continue;
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
        {
          ChunkWater* water = tile->Water.getChunk(x, z);
          if (!water)
            continue;
          for (std::uint32_t i = 0; i < water->flowCount() && i < 2; ++i)
            arrow(water->flowSlot(int(i)));
        }
    }
    glm::vec4 const arrowColor(0.45f, 0.8f, 1.f, 1.f);
    _line.drawSegments(mvp, seg, arrowColor);

    if (_flowHit)
    {
      std::vector<glm::vec3> ring;
      int const steps = 48;
      float const r = std::clamp(_flowRadius, FLOW_RADIUS_MIN, FLOW_RADIUS_MAX);
      glm::vec4 const color = _flowErase ? glm::vec4(1.f, 0.5f, 0.4f, 1.f) : glm::vec4(0.3f, 1.f, 0.9f, 1.f);
      for (int i = 0; i < steps; ++i)
      {
        float const a0 = 6.2831853f * float(i) / float(steps);
        float const a1 = 6.2831853f * float(i + 1) / float(steps);
        ring.push_back(*_flowHit + glm::vec3(std::cos(a0) * r, 0.2f, std::sin(a0) * r));
        ring.push_back(*_flowHit + glm::vec3(std::cos(a1) * r, 0.2f, std::sin(a1) * r));
      }
      _line.drawSegments(mvp, ring, color);
    }
  }
}
