// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// The "Everwood" tool panel: sculpt (dig / build / smooth, push / pull), flatten, bridge / tunnel on the
// voxel layer, the same brushes as the game's EME map editor (crates/eme/src/tools). Only created when
// the project has EverwoodMode. See EVERWOOD-MODE.md.
#pragma once

#include <noggit/everwood/EverwoodLayer.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <noggit/Tool.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QWidget;
class QFormLayout;
class QString;

namespace Noggit
{
  // eme-core stroke.rs EmeStrokeBuilder: a held brush becomes one stroke of sparse edits. Send when the
  // centre moved >= radius / 2 or 125 ms passed. Idempotent modes (hard dig / build) held still do not
  // resend; hard dig / build measure movement in the ground plane only and stay within a depth cap.
  struct EverwoodStrokeConfig
  {
    float moveFraction = 0.5f;
    float stationaryFraction = 0.05f;
    float hardDepthRadii = 1.0f;
    bool planeMoves = true;
    uint64_t minIntervalMs = 125;
  };

  class EverwoodStrokeBuilder
  {
  public:
    using Sample = Everwood::Voxel::Edit;

    void begin(EverwoodStrokeConfig const& config, uint64_t nowMs, Sample const& first, std::vector<Sample>& out);
    void update(uint64_t nowMs, Sample sample, std::vector<Sample>& out);
    void tick(uint64_t nowMs, std::vector<Sample>& out);
    void end(uint64_t nowMs, std::optional<Sample> sample, std::vector<Sample>& out);
    bool active() const { return _active; }

  private:
    static bool idempotent(Sample const& s);
    Sample capped(Sample s) const;
    bool due(Sample const& s) const;
    bool rateAllows(uint64_t nowMs) const;

    EverwoodStrokeConfig _config;
    bool _active = false;
    float _startZ = 0.f;
    std::optional<Sample> _pending;
    std::optional<Sample> _lastSent;
    std::optional<uint64_t> _lastSentMs;
  };

  class EverwoodTool final : public Tool
  {
  public:
    enum class Kind { Sculpt = 0, Flatten = 1, Bridge = 2 };
    enum class SculptMode { Dig = 0, Build = 1, Smooth = 2, Push = 3, Pull = 4 };

    explicit EverwoodTool(MapView* mapView);
    ~EverwoodTool() override;

    [[nodiscard]] char const* name() const override;
    [[nodiscard]] editing_mode editingMode() const override;
    [[nodiscard]] Ui::FontNoggit::Icons icon() const override;
    [[nodiscard]] ToolDrawParameters drawParameters() const override;
    [[nodiscard]] float brushRadius() const override;

    void setupUi(Ui::Tools::ToolPanel* toolPanel) override;
    void postUiSetup() override;
    void onSelected() override;
    void onDeselected() override;
    void onTick(float deltaTime, TickParameters const& params) override;
    void postRender() override;
    void onMousePress(MousePressParameters const& params) override;
    void onMouseWheel(MouseWheelParameters const& params) override;
    void onFocusLost() override;
    void unload() override;

    Everwood::EverwoodLayer* layer() { return _layer.get(); }

    // What the panel does, also used by the smoke-test hook.
    bool setOption(QString const& key, QString const& value);
    void undo();
    void redo();
    std::string exportEdits();
    void confirmBridge();
    void cancelBridge();
    void setBridgePoint(glm::vec3 const& wow);
    // One scripted stroke of exact edits, as the mouse would have produced them.
    void applyStroke(std::vector<Everwood::Voxel::Edit> const& edits);

  private:
    using Edit = Everwood::Voxel::Edit;

    void updateAim();
    void updatePanel();
    void updateStatus();

    Edit sculptSample(Everwood::AimHit const& hit) const;
    Edit pushPullSample(Everwood::AimHit const& hit) const;
    Edit softenSample(glm::vec3 const& centre) const;
    uint16_t strengthWire() const;

    void beginStroke(Everwood::AimHit const& hit);
    void endStroke();
    void commit(std::vector<Edit> const& edits);
    void openAction(int modality);

    struct BridgeSpec
    {
      glm::vec3 a, b;
      float radius;
      uint8_t shape;
      bool tunnel;
      float arch;
      float thickness;

      int points() const;
      glm::vec3 point(int i, int n) const;
      bool carvesUnderside() const;
    };
    BridgeSpec bridgeSpec() const;

    std::shared_ptr<Everwood::EverwoodLayer> _layer;

    // Options, the eme tools' defaults.
    Kind _kind = Kind::Sculpt;
    SculptMode _mode = SculptMode::Dig;
    float _radius = 4.f;
    float _strength = 20.f;
    float _flattenStrength = 25.f;
    float _pushStrength = 30.f;
    uint8_t _falloff = Everwood::Voxel::FALLOFF_SMOOTH;
    float _hardness = 0.f;
    bool _hard = false;
    uint8_t _shape = Everwood::Voxel::SHAPE_SPHERE;
    float _offset = 0.f;
    int _pushEdge = 0;             // 0 hard edge, 1 smooth, 2 linear
    bool _pushAlongView = false;
    bool _tunnel = false;
    float _bridgeRadius = 2.5f;
    uint8_t _bridgeShape = Everwood::Voxel::SHAPE_CUBE;
    float _arch = 0.f;
    float _thickness = 1.5f;

    // The stroke.
    EverwoodStrokeBuilder _builder;
    uint16_t _strokeId = 0;
    uint64_t _nowMs = 0;
    bool _strokeHeld = false;
    float _flattenPlane = 0.f;
    std::optional<glm::vec3> _lastCentre;
    bool _softenNext = false;

    std::optional<glm::vec3> _bridgeA, _bridgeB;
    std::optional<Everwood::AimHit> _aim;
    glm::vec3 _viewDirWow{ 0.f, 0.f, -1.f };

    // UI
    QWidget* _panel = nullptr;
    QFormLayout* _form = nullptr;
    QComboBox* _kindBox = nullptr;
    QComboBox* _modeBox = nullptr;
    QDoubleSpinBox* _radiusBox = nullptr;
    QDoubleSpinBox* _strengthBox = nullptr;
    QComboBox* _shapeBox = nullptr;
    QCheckBox* _hardBox = nullptr;
    QComboBox* _falloffBox = nullptr;
    QDoubleSpinBox* _hardnessBox = nullptr;
    QDoubleSpinBox* _offsetBox = nullptr;
    QComboBox* _pushEdgeBox = nullptr;
    QComboBox* _pushDirBox = nullptr;
    QComboBox* _bridgeKindBox = nullptr;
    QDoubleSpinBox* _archBox = nullptr;
    QDoubleSpinBox* _thicknessBox = nullptr;
    QLabel* _bridgeLabel = nullptr;
    QPushButton* _confirmButton = nullptr;
    QPushButton* _cancelButton = nullptr;
    QLabel* _status = nullptr;
    bool _updating = false;

    Noggit::Rendering::Primitives::Sphere _sphere;
    Noggit::Rendering::Primitives::WireBox _box;
  };
}
