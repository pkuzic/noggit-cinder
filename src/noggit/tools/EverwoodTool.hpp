// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// The "Everwood" tool panel: sculpt (dig / build / smooth, push / pull), flatten, bridge / tunnel on the
// voxel layer, and (CONTRACT v3.10) "Paint (caves)", the 3D voxel paint (journal mode 7) of dug and built faces;
// the same brushes as the game's EME map editor (crates/eme/src/tools), and "Water flow" (EVERWOOD-WATER-FLOW.md:
// drag across water to author its current in the MCLQ flow slots of the ADT). Only created when
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
#include <unordered_set>
#include <vector>

class ChunkWater;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QWidget;
class QFormLayout;
class QLineEdit;
class QString;

namespace Noggit
{
  namespace Ui
  {
    class current_texture;
  }

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
    // A stroke whose press produced no sample (a refused paint dab): the first accepted sample is sent at once.
    void beginEmpty(EverwoodStrokeConfig const& config);
    void update(uint64_t nowMs, Sample sample, std::vector<Sample>& out);
    void tick(uint64_t nowMs, std::vector<Sample>& out);
    // Release. A stationary cursor (cursorMoved = false) adds no new input: a sample still held back is only a
    // re-aim onto the surface this stroke already changed, so it is dropped (softenOnly keeps a held-back
    // Push/Pull softening pass, which is part of the one dab).
    void end(uint64_t nowMs, std::optional<Sample> sample, std::vector<Sample>& out, bool cursorMoved = true, bool softenOnly = false);
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
    enum class Kind { Sculpt = 0, Flatten = 1, Bridge = 2, Paint = 3, Flow = 4, Ramp = 5 };
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
    // Remove floaters (VoxelFloaters.hpp): the whole edited area, one stroke = one undo step. Returns the status text
    // ("Removed 7 floaters (31 edits)").
    std::string removeFloaters();
    // Ramp / Path: points of the path (WoW axes); confirmRamp turns them into ordinary LEVEL / smooth edits, one
    // stroke = one undo step. confirmRamp returns an empty string when it built the ramp, else why it did not.
    void addRampPoint(glm::vec3 const& wow);
    std::string confirmRamp();
    void cancelRamp();
    size_t rampPointCount() const { return _rampPoints.size(); }
    size_t lastRampEdits() const { return _lastRampEdits; }
    // One scripted stroke of exact edits, as the mouse would have produced them.
    void applyStroke(std::vector<Everwood::Voxel::Edit> const& edits);
    // Paint (caves): one dab where a ray meets the voxel surface (WoW axes), through the tool's own checks.
    // Returns an empty string when it painted, else why it did not.
    std::string paintRay(glm::vec3 const& origin, glm::vec3 const& dir);
    int paintTexture() const { return _paintTexture; }

  private:
    using Edit = Everwood::Voxel::Edit;

    void updateAim();
    void updatePanel();
    void updateStatus();

    Edit sculptSample(Everwood::AimHit const& hit) const;
    Edit pushPullSample(Everwood::AimHit const& hit) const;
    Edit softenSample(glm::vec3 const& centre) const;
    uint16_t strengthWire() const;
    // CONTRACT v3.10 mode 7 at hit + 0.5 yd * normal (the air side); nullopt + why when the hit is refused.
    std::optional<Edit> paintSample(Everwood::AimHit const& hit, QString* why);
    bool paintRefused(Edit const& e, QString* why);
    // Tablet pressure 0..1 for this sample (1 when "Use tablet pressure" is off or no tablet is active).
    float pressureScale() const;
    // Paint (caves) spray: one due dab -> `_sprayDensity` small dabs scattered in a disc on the aimed surface plane.
    void sprayDabs(Edit const& dab, std::vector<Edit>& out);
    void setPaintTexture(int id);
    bool selectPaintText(QString const& text);
    void setHint(QString const& text);

    // Water flow (EVERWOOD-WATER-FLOW.md).
    void flowTick(TickParameters const& params);
    void flowEnd();
    void flowApply(glm::vec3 const& centreWow, glm::vec2 const& dirWow, std::optional<float> hitWaterZ);
    void renderFlow(glm::mat4x4 const& mvp);

    // Ramp / Path (EVERWOOD-MODE.md): the edit scheme lives in rampEdits.
    struct RampPath;
    std::vector<Edit> rampEdits(std::vector<glm::vec3> const& points, QString* why) const;
    bool rampGround(float x, float y, float zHint, float& z) const;
    void drawRamp(glm::mat4x4 const& mvp);
    void extendFloaterBox(Edit const& e);
    void removeFloatersUnderBrush();

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
    int _smoothPasses = 2;         // CONTRACT v3.11 pass code: 0..3 = 1 / 2 / 4 / 8 passes (default 4)
    bool _usePressure = true;      // multiply the sculpt strength / paint opacity by the tablet pressure
    float _fakePressure = 0.f;     // smoke only: 0 = off, else the pressure to pretend a tablet reports
    uint8_t _shape = Everwood::Voxel::SHAPE_SPHERE;
    float _offset = 0.f;
    int _pushEdge = 0;             // 0 hard edge, 1 smooth, 2 linear
    bool _pushAlongView = false;
    bool _tunnel = false;
    float _bridgeRadius = 2.5f;
    uint8_t _bridgeShape = Everwood::Voxel::SHAPE_CUBE;
    float _arch = 0.f;
    float _thickness = 1.5f;
    // Paint (caves), eme tools/paint.rs defaults
    int _paintTexture = -1;        // palette id, -1 none
    float _paintRadius = 3.f;
    float _paintOpacity = 30.f;
    uint8_t _paintFalloff = Everwood::Voxel::FALLOFF_SMOOTH;
    int _paintHard = -1;           // CONTRACT v3.11 HARD core: -1 off (falloff), 0..3 = core 25 / 50 / 75 / 90 %
    bool _spray = false;           // Paint (caves) spray (Noggit only: ordinary mode-7 dabs)
    float _spraySize = 4.f;        // yd: radius of the scatter disc
    int _sprayDensity = 6;         // dabs per due dab
    uint32_t _sprayState = 0x9E3779B9u;   // xorshift32, reseeded per stroke
    bool _paintErase = false;
    bool _paintReveal = false;
    bool _actionOpen = false;
    // Water flow
    float _flowRadius = 16.f;
    float _flowVelocity = 1.5f;
    float _flowTurbulence = 0.5f;
    bool _flowErase = false;
    bool _flowHeld = false;
    std::optional<glm::vec3> _flowLastWow;        // previous drag sample (WoW axes)
    std::optional<glm::vec3> _flowHit;            // cursor on water / terrain (Noggit axes)
    std::unordered_set<ChunkWater*> _flowTouched; // chunks changed by the current stroke
    size_t _flowSamples = 0;

    // The stroke.
    EverwoodStrokeBuilder _builder;
    uint16_t _strokeId = 0;
    uint64_t _nowMs = 0;
    bool _strokeHeld = false;
    float _flattenPlane = 0.f;
    std::optional<glm::vec3> _lastCentre;
    bool _softenNext = false;
    std::pair<glm::vec3, glm::vec3> _cursorRay{};
    std::optional<std::pair<glm::vec3, glm::vec3>> _sentRay;
    uint64_t _strokeStartMs = 0;
    int _strokeTicks = 0;

    std::optional<glm::vec3> _bridgeA, _bridgeB;

    // Remove floaters
    int _floaterMax = 64;                  // voxels (0.5 yd each)
    bool _smoothFloaters = true;           // Sculpt > Smooth: also remove the floaters under the brush at stroke end
    bool _floaterBoxValid = false;         // the current smooth stroke's lattice box (Everwood::Voxel lattice, inclusive)
    Everwood::Voxel::Key _floaterLo{ 0, 0, 0 }, _floaterHi{ 0, 0, 0 };
    std::vector<std::array<float, 4>> _floaterBalls;   // the stroke's smooth dabs (lattice centre, radius)
    QString _floaterText;

    // Ramp / Path
    std::vector<glm::vec3> _rampPoints;    // WoW axes, the clicked ground points
    float _rampWidth = 6.f;                // yd, the flat width of the path
    int _rampSlope = 0;                    // 0 follow the clicked points' heights, 1 fixed angle
    float _rampAngle = 12.f;               // deg (fixed angle mode): + rises along the path, - falls
    int _rampEdge = 1;                     // edge softness: 0 sharp, 1 normal, 2 soft
    float _rampStrength = 100.f;           // %
    bool _rampSmooth = true;               // a light smooth along the strip after the levelling
    size_t _lastRampEdits = 0;
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
    QDoubleSpinBox* _rampWidthBox = nullptr;
    QComboBox* _rampSlopeBox = nullptr;
    QDoubleSpinBox* _rampAngleBox = nullptr;
    QComboBox* _rampEdgeBox = nullptr;
    QCheckBox* _rampSmoothBox = nullptr;
    QCheckBox* _smoothFloatersBox = nullptr;
    QDoubleSpinBox* _floaterMaxBox = nullptr;
    QWidget* _floaterBox = nullptr;        // Max floater size + Remove floaters + its result line
    QLabel* _floaterLabel = nullptr;
    QPushButton* _confirmButton = nullptr;
    QPushButton* _cancelButton = nullptr;
    QLabel* _status = nullptr;
    QLabel* _hint = nullptr;
    QWidget* _paintBox = nullptr;
    Ui::current_texture* _paintPreview = nullptr;
    QComboBox* _paintTextureBox = nullptr;
    QPushButton* _paintPickButton = nullptr;
    QDoubleSpinBox* _opacityBox = nullptr;
    QComboBox* _paintFalloffBox = nullptr;
    QComboBox* _passesBox = nullptr;
    QComboBox* _paintHardBox = nullptr;
    QCheckBox* _pressureBox = nullptr;
    QCheckBox* _sprayBox = nullptr;
    QDoubleSpinBox* _spraySizeBox = nullptr;
    QDoubleSpinBox* _sprayDensityBox = nullptr;
    QCheckBox* _eraseBox = nullptr;
    QCheckBox* _revealBox = nullptr;
    QWidget* _flowBox = nullptr;
    QWidget* _voxelButtons = nullptr;    // Undo / Redo stroke, Export, Re-seed (hidden for Water flow)
    QDoubleSpinBox* _flowRadiusBox = nullptr;
    QDoubleSpinBox* _flowVelocityBox = nullptr;
    QDoubleSpinBox* _flowTurbulenceBox = nullptr;
    QComboBox* _flowModeBox = nullptr;
    bool _updating = false;

    Noggit::Rendering::Primitives::Line _line;
    Noggit::Rendering::Primitives::Sphere _sphere;
    Noggit::Rendering::Primitives::WireBox _box;
  };
}
