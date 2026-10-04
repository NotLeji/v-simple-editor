#include "ProjectDiffDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QRegularExpression>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
QString label(projdiff::Change::Type type)
{
    switch (type) {
    case projdiff::Change::Added: return QStringLiteral("Add");
    case projdiff::Change::Removed: return QStringLiteral("Delete");
    case projdiff::Change::Moved: return QStringLiteral("Moved");
    case projdiff::Change::Trimmed: return QStringLiteral("Trim");
    case projdiff::Change::PropertyChanged: return QStringLiteral("Property Changed");
    case projdiff::Change::EffectsChanged: return QStringLiteral("Effects Changed");
    case projdiff::Change::TransitionChanged: return QStringLiteral("Transition Changed");
    case projdiff::Change::TrackFlagChanged: return QStringLiteral("Track Settings Changed");
    }
    return {};
}
}

ProjectDiffDialog::ProjectDiffDialog(const QVector<projdiff::Change> &changes, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Compare with Saved"));
    resize(980, 520);
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(changes.isEmpty()
        ? QStringLiteral("No changes.")
        : QStringLiteral("%1 changes. Double-click a row to jump to the current clip. Deleted clips cannot be jumped to.")
              .arg(changes.size()), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *table = new QTableWidget(int(changes.size()), 4, this);
    table->setObjectName(QStringLiteral("projectDiffTable"));
    table->setHorizontalHeaderLabels({QStringLiteral("Type"), QStringLiteral("Location"),
                                     QStringLiteral("Before"), QStringLiteral("After")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int row = 0; row < changes.size(); ++row) {
        const auto &change = changes[row];
        const QStringList cells{label(change.type), change.path, change.before, change.after};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells[column]);
            item->setToolTip(cells[column]);
            table->setItem(row, column, item);
        }
    }
    connect(table, &QTableWidget::cellDoubleClicked, this, [this, changes](int row, int) {
        if (row < 0 || row >= changes.size() || changes[row].type == projdiff::Change::Removed) return;
        static const QRegularExpression pattern(QStringLiteral("^(video|audio)\\[(\\d+)\\]\\.clips\\[(\\d+)\\]"));
        const auto match = pattern.match(changes[row].path);
        if (match.hasMatch())
            emit clipActivated(match.captured(1) == QStringLiteral("audio"),
                               match.captured(2).toInt(), match.captured(3).toInt());
    });
    layout->addWidget(table);
    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(QStringLiteral("Close"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
