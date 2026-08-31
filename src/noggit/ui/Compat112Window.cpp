// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/Compat112Window.hpp>

#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <map>

namespace Noggit::Ui
{
  namespace
  {
    char const* severity_label(Noggit::Compat112Severity s)
    {
      switch (s)
      {
        case Noggit::Compat112Severity::Error:   return "ERROR";
        case Noggit::Compat112Severity::Warning: return "WARN";
        default:                                 return "INFO";
      }
    }

    QColor severity_color(Noggit::Compat112Severity s)
    {
      switch (s)
      {
        case Noggit::Compat112Severity::Error:   return QColor(224, 96, 96);
        case Noggit::Compat112Severity::Warning: return QColor(224, 176, 80);
        default:                                 return QColor(150, 168, 208);
      }
    }
  }

  Compat112Window::Compat112Window(std::vector<Noggit::Compat112Finding> const& findings, QWidget* parent)
    : QDialog(parent)
    , _tree(nullptr)
  {
    setWindowTitle("1.12 Compatibility Check");
    setWindowIcon(QIcon(":/icon"));
    resize(780, 500);

    auto* layout(new QVBoxLayout(this));

    int errors(0), warnings(0), infos(0);
    for (auto const& f : findings)
    {
      switch (f.severity)
      {
        case Noggit::Compat112Severity::Error:   ++errors; break;
        case Noggit::Compat112Severity::Warning: ++warnings; break;
        default:                                 ++infos; break;
      }
    }

    QString summary;
    if (findings.empty())
    {
      summary = "PASS — no 1.12 compatibility issues found in the loaded tiles.";
    }
    else
    {
      summary = QString("%1 issue(s): %2 error, %3 warning, %4 info"
                        "  —  double-click a row to fly to its tile.")
                  .arg(static_cast<int>(findings.size()))
                  .arg(errors).arg(warnings).arg(infos);
    }
    auto* summary_label(new QLabel(summary, this));
    summary_label->setWordWrap(true);
    layout->addWidget(summary_label);

    _tree = new QTreeWidget(this);
    _tree->setColumnCount(3);
    _tree->setHeaderLabels({ "Severity", "Tile", "Detail" });
    _tree->header()->setStretchLastSection(true);
    _tree->setAlternatingRowColors(true);
    _tree->setUniformRowHeights(true);
    layout->addWidget(_tree, 1);

    // Group findings under a per-category parent node.
    std::map<std::string, QTreeWidgetItem*> groups;
    for (auto const& f : findings)
    {
      QTreeWidgetItem* group;
      auto it(groups.find(f.category));
      if (it == groups.end())
      {
        group = new QTreeWidgetItem(_tree);
        group->setText(0, QString::fromStdString(f.category));
        group->setFirstColumnSpanned(true);
        group->setExpanded(true);
        // group nodes carry no tile data -> not clickable-to-jump
        groups.emplace(f.category, group);
      }
      else
      {
        group = it->second;
      }

      auto* item(new QTreeWidgetItem(group));
      item->setText(0, severity_label(f.severity));
      item->setForeground(0, severity_color(f.severity));
      item->setText(1, f.tile_x >= 0 ? QString("%1, %2").arg(f.tile_x).arg(f.tile_z) : QString("—"));
      item->setText(2, QString::fromStdString(f.detail));
      item->setData(0, Qt::UserRole, f.tile_x);
      item->setData(1, Qt::UserRole, f.tile_z);
    }

    _tree->resizeColumnToContents(0);
    _tree->resizeColumnToContents(1);

    connect(_tree, &QTreeWidget::itemDoubleClicked, this,
      [this](QTreeWidgetItem* item, int)
      {
        QVariant const vx(item->data(0, Qt::UserRole));
        if (!vx.isValid())
          return; // a group header, not a finding

        int const tx(vx.toInt());
        int const tz(item->data(1, Qt::UserRole).toInt());
        if (tx >= 0 && tz >= 0)
          emit jumpToTile(tx, tz);
      });

    auto* button_row(new QHBoxLayout());
    button_row->addStretch(1);
    auto* close_button(new QPushButton("Close", this));
    connect(close_button, &QPushButton::clicked, this, &QDialog::accept);
    button_row->addWidget(close_button);
    layout->addLayout(button_row);
  }
}
