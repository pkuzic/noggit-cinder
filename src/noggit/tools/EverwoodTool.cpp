// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "EverwoodTool.hpp"

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/Log.h>
#include <noggit/everwood/PaintPalette.hpp>
#include <noggit/everwood/VoxelPaint3.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <opengl/context.hpp>
#include <opengl/context.inl>

#include <QtCore/QString>
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
#include <cmath>

namespace Noggit
{
  using namespace Everwood;
  using namespace Everwood::Voxel;

  namespace
  {
    constexpr float BRIDGE_MAX_LENGTH = 160.f;
    constexpr int BRIDGE_MAX_EDITS = 400;
    constexpr float AIM_RANGE = 800.f;

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

  void EverwoodStrokeBuilder::end(uint64_t nowMs, std::optional<Sample> sample, std::vector<Sample>& out)
  {
    if (!_active)
      return;
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

    _kindBox = combo({ "Sculpt", "Flatten", "Bridge / Tunnel", "Paint (caves)" });
    _modeBox = combo({ "Lower / Dig", "Raise / Build", "Smooth", "Push out", "Pull in" });
    _radiusBox = spin(0.5, 16.0, 0.5, _radius);
    _strengthBox = spin(1.0, 100.0, 1.0, _strength, 0);
    _shapeBox = combo({ "Sphere", "Cube", "Upright cylinder" });
    _hardBox = new QCheckBox("Hard brush (CSG)", _panel);
    _falloffBox = combo({ "Smooth", "Linear", "Flat" });
    _hardnessBox = spin(0.0, 100.0, 5.0, _hardness, 0);
    _offsetBox = spin(-16.0, 16.0, 0.25, _offset, 2);
    _pushEdgeBox = combo({ "Hard edge", "Smooth", "Linear" });
    _pushDirBox = combo({ "Surface normal", "View direction" });
    _bridgeKindBox = combo({ "Bridge (build)", "Tunnel (dig)" });
    _archBox = spin(-32.0, 32.0, 0.5, _arch);
    _thicknessBox = spin(0.5, 32.0, 0.25, _thickness, 2);

    _form->addRow("Tool", _kindBox);
    _form->addRow("Mode", _modeBox);
    _form->addRow("Bridge", _bridgeKindBox);
    _form->addRow("Radius (yd)", _radiusBox);
    _form->addRow("Shape", _shapeBox);
    _form->addRow("Strength (%)", _strengthBox);
    _form->addRow("", _hardBox);
    _form->addRow("Falloff", _falloffBox);
    _form->addRow("Hardness (%)", _hardnessBox);
    _form->addRow("Plane offset (yd)", _offsetBox);
    _form->addRow("Edge", _pushEdgeBox);
    _form->addRow("Direction", _pushDirBox);
    _form->addRow("Arch (yd)", _archBox);
    _form->addRow("Deck thickness (yd)", _thicknessBox);

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
    _eraseBox = new QCheckBox("Erase this texture", _paintBox);
    _revealBox = new QCheckBox("Reveal (fade every cave texture)", _paintBox);
    paintForm->addRow(_paintPreview);
    paintForm->addRow("Texture", _paintTextureBox);
    paintForm->addRow(_paintPickButton);
    paintForm->addRow("Opacity (%)", _opacityBox);
    paintForm->addRow("Falloff", _paintFalloffBox);
    paintForm->addRow("", _eraseBox);
    paintForm->addRow("", _revealBox);
    auto* paintHelp = new QLabel("Paints dug or built voxel faces (cave floors, walls, ceilings) through the air the brush "
                                 "reaches. Up to 4 cave textures per 16 yd block. Untouched ground: use Texture Paint.", _paintBox);
    paintHelp->setWordWrap(true);
    paintForm->addRow(paintHelp);
    outer->addWidget(_paintBox);

    _bridgeLabel = new QLabel(_panel);
    _bridgeLabel->setWordWrap(true);
    outer->addWidget(_bridgeLabel);
    auto* bridgeRow = new QHBoxLayout();
    _confirmButton = new QPushButton("Build", _panel);
    _cancelButton = new QPushButton("Cancel", _panel);
    bridgeRow->addWidget(_confirmButton);
    bridgeRow->addWidget(_cancelButton);
    outer->addLayout(bridgeRow);

    auto* undoRow = new QHBoxLayout();
    auto* undoButton = new QPushButton("Undo stroke", _panel);
    auto* redoButton = new QPushButton("Redo stroke", _panel);
    undoRow->addWidget(undoButton);
    undoRow->addWidget(redoButton);
    outer->addLayout(undoRow);

    auto* exportButton = new QPushButton("Export voxel edits...", _panel);
    exportButton->setToolTip("Writes every live edit to <project>/everwood/export for the server import (EVERWOOD-MODE.md).");
    outer->addWidget(exportButton);
    auto* reseedButton = new QPushButton("Re-seed from saved ADTs", _panel);
    reseedButton->setToolTip("Replays every edit on the ADT heights as saved on disk now. Use it after saving terrain changes under voxel edits.");
    outer->addWidget(reseedButton);

    _hint = new QLabel(_panel);
    _hint->setWordWrap(true);
    _hint->setStyleSheet("color: #e0a040;");
    _hint->setVisible(false);
    outer->addWidget(_hint);

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
      if (_kind == Kind::Flatten)
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
    QObject::connect(_eraseBox, &QCheckBox::toggled, _panel, [this](bool on) { _paintErase = on; });
    QObject::connect(_revealBox, &QCheckBox::toggled, _panel, [this, changed](bool on)
    {
      _paintReveal = on;
      changed();
    });

    QObject::connect(_confirmButton, &QPushButton::clicked, _panel, [this] { confirmBridge(); });
    QObject::connect(_cancelButton, &QPushButton::clicked, _panel, [this] { cancelBridge(); });
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

    auto show = [this](QWidget* field, bool visible)
    {
      field->setVisible(visible);
      if (auto* label = _form->labelForField(field))
        label->setVisible(visible);
    };
    show(_modeBox, sculpt);
    show(_bridgeKindBox, bridge);
    show(_strengthBox, !bridge && !paint);
    show(_shapeBox, shapeMatters);
    show(_hardBox, sculpt && !pushPull);
    show(_falloffBox, softOptions);
    show(_hardnessBox, softOptions);
    show(_offsetBox, flatten);
    show(_pushEdgeBox, pushPull);
    show(_pushDirBox, pushPull);
    show(_archBox, bridge);
    show(_thicknessBox, bridge && !_tunnel);
    _bridgeLabel->setVisible(bridge);
    _confirmButton->setVisible(bridge);
    _cancelButton->setVisible(bridge);
    _paintBox->setVisible(paint);
    _eraseBox->setEnabled(!_paintReveal);
    if (!paint)
      setHint({});

    // Mode 7 is valid up to 8 yd (CONTRACT v3.10).
    _radiusBox->setRange(paint ? PAINT_RADIUS_MIN : 0.5, paint ? PAINT3_MAX_RADIUS : 16.0);
    _radiusBox->setValue(bridge ? _bridgeRadius : (paint ? _paintRadius : _radius));
    _shapeBox->setCurrentIndex(bridge ? _bridgeShape : _shape);
    _strengthBox->setValue(flatten ? _flattenStrength : (pushPull ? _pushStrength : _strength));

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
    _updating = false;
  }

  void EverwoodTool::updateStatus()
  {
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
    if (key == "tool") return pick(_kindBox, { "sculpt", "flatten", "bridge", "paint" });
    if (key == "texture") return selectPaintText(value);
    if (key == "opacity") { _opacityBox->setValue(value.toDouble()); return true; }
    if (key == "paint_falloff") return pick(_paintFalloffBox, { "smooth", "linear", "flat" });
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
    if (_strokeHeld)
      endStroke();
  }

  void EverwoodTool::onFocusLost()
  {
    if (_strokeHeld)
      endStroke();
  }

  void EverwoodTool::unload()
  {
    mapView()->getWorld()->renderer()->setEverwoodLayer(nullptr);
    _layer->releaseGl();
    _sphere.unload();
    _box.unload();
  }

  // ---------------------------------------------------------------------------------------------- aim
  void EverwoodTool::updateAim()
  {
    math::ray const ray = mapView()->cursorRay();
    glm::vec3 const origin = noggitToWow(ray.origin());
    glm::vec3 const end = noggitToWow(ray.position(1.f));
    _viewDirWow = glm::normalize(end - origin);
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
    uint8_t const s = strengthU8(_strength);
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
    e.strength = strengthU8(_paintOpacity);
    Paint3Param p;
    // REVEAL ignores the id: it works with no texture selected.
    p.id = uint16_t(std::max(_paintTexture, 0));
    p.falloff = _paintFalloff;
    p.erase = _paintErase && !_paintReveal;
    p.reveal = _paintReveal;
    e.param = p.ToParam();
    return e;
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
    Action* action = NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eEVERWOOD_VOXEL, modality);
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

  void EverwoodTool::commit(std::vector<Edit> const& edits)
  {
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
    }
  }

  void EverwoodTool::beginStroke(AimHit const& hit)
  {
    _strokeHeld = true;
    _strokeId = _layer->beginStroke();
    _flattenPlane = hit.pos.z + _offset;
    _lastCentre.reset();
    _softenNext = false;

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
    _builder.end(_nowMs, last, out);
    commit(out);
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

    updateAim();

    if (_kind == Kind::Bridge)
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

    std::vector<Edit> out;
    if (_aim)
    {
      if (_kind == Kind::Sculpt && (_mode == SculptMode::Push || _mode == SculptMode::Pull))
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
    commit(out);
    if (!out.empty())
      updateStatus();
  }

  void EverwoodTool::onMousePress(MousePressParameters const& params)
  {
    if (params.button != Qt::LeftButton || _kind != Kind::Bridge)
      return;
    updateAim();
    if (!_aim)
      return;
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

  void EverwoodTool::applyStroke(std::vector<Edit> const& edits)
  {
    if (NOGGIT_CUR_ACTION)
      NOGGIT_ACTION_MGR->endAction();
    _strokeId = _layer->beginStroke();
    openAction(ActionModalityControllers::eNONE);
    commit(edits);
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
    float const step = params.event.angleDelta().y() > 0 ? 0.5f : -0.5f;
    _radiusBox->setValue(std::clamp(double(brushRadius() + step), 0.5, _kind == Kind::Paint ? double(PAINT3_MAX_RADIUS) : 16.0));
  }

  void EverwoodTool::postRender()
  {
    if (mapView()->get_editing_mode() != editing_mode::everwood)
      return;

    glm::mat4x4 const modelView = mapView()->model_view();
    glm::mat4x4 const projection = mapView()->projection();
    glm::mat4x4 const mvp = projection * modelView;

    auto drawBrush = [&](glm::vec3 const& wow, float radius, uint8_t shape, glm::vec4 const& color)
    {
      glm::vec3 const p = wowToNoggit(wow);
      if (shape == SHAPE_SPHERE)
      {
        _sphere.draw(mvp, p, color, radius, 24, 14, 0.25f);
        return;
      }
      // Both other shapes are boxes in WoW axes; for the cylinder this is its bounding box.
      _box.draw(modelView, projection, glm::mat4x4(1.f), color, p - glm::vec3(radius), p + glm::vec3(radius));
    };

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
      drawBrush(_aim->pos + _aim->normal * 0.5f, std::clamp(_paintRadius, PAINT_RADIUS_MIN, PAINT3_MAX_RADIUS), SHAPE_SPHERE, color);
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
      drawBrush(_aim->pos, _radius, usesShape ? _shape : uint8_t(SHAPE_SPHERE), color);
    }
  }
}
