#pragma once

#include <QDialog>
#include <QPoint>
#include <memory>

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class World;

namespace Noggit::Project { class NoggitProject; }

namespace Noggit::Ui::Windows
{
  class AdtGridWidget;

  class AdtPorterDialog final : public QDialog
  {
    Q_OBJECT
  public:
    // Port      = ADT Porting Tool (copy ADTs map -> map, or shift on one map).
    // TileMover = ADT Tile Mover (move OR copy whole tiles within ONE map; refuses to
    //             overwrite an existing destination tile unless "overwrite" is ticked).
    enum class Mode { Port, TileMover };

    explicit AdtPorterDialog(std::shared_ptr<Project::NoggitProject> project,
                             QWidget* parent = nullptr, Mode mode = Mode::Port);
    ~AdtPorterDialog() override;

  private:
    void portAdt();
    void moveTiles();
    bool moving() const;
    void updateSummary();
    void updateNudgeButtons();
    void nudge(QPoint const& direction);
    QPoint selectionPivot() const;
    QPoint selectionOffset() const;
    bool offsetFits(QPoint const& offset) const;
    void reloadSourceGrid();
    void reloadDestinationGrid();

    std::shared_ptr<Project::NoggitProject> _project;
    Mode _mode = Mode::Port;
    QComboBox* _source_map = nullptr;
    QComboBox* _destination_map = nullptr;
    QCheckBox* _same_map = nullptr;
    QCheckBox* _hide_adt_squares = nullptr;
    QSlider* _preview_opacity = nullptr;
    AdtGridWidget* _source_grid = nullptr;
    AdtGridWidget* _destination_grid = nullptr;
    QLabel* _summary = nullptr;
    QLabel* _status = nullptr;
    QRadioButton* _move_mode = nullptr;
    QRadioButton* _copy_mode = nullptr;
    QCheckBox* _overwrite = nullptr;
    QPushButton* _action_button = nullptr;
    QSpinBox* _nudge_step = nullptr;
    QPushButton* _nudge_north = nullptr;
    QPushButton* _nudge_west = nullptr;
    QPushButton* _nudge_east = nullptr;
    QPushButton* _nudge_south = nullptr;
    std::unique_ptr<World> _source_preview_world;
    std::unique_ptr<World> _destination_preview_world;
  };
}
