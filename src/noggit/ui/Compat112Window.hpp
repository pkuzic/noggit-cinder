// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#ifndef NOGGIT_UI_COMPAT112WINDOW_HPP
#define NOGGIT_UI_COMPAT112WINDOW_HPP

#include <noggit/Compat112Check.hpp>

#include <QtWidgets/QDialog>

#include <vector>

class QTreeWidget;

namespace Noggit::Ui
{
  // Modal report window for the "Check loaded tiles for 1.12 issues" tool.
  // Groups findings by category; double-clicking a tile-scoped finding asks the
  // MapView (via jumpToTile) to fly the camera to that tile.
  class Compat112Window : public QDialog
  {
    Q_OBJECT

  public:
    Compat112Window(std::vector<Noggit::Compat112Finding> const& findings, QWidget* parent = nullptr);

  signals:
    void jumpToTile(int tile_x, int tile_z);

  private:
    QTreeWidget* _tree;
  };
}

#endif // NOGGIT_UI_COMPAT112WINDOW_HPP
