#include "StillGalleryDock.h"

#include "StillStore.h"

#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr int kStillIdRole = Qt::UserRole + 1;
constexpr int kStillPathRole = Qt::UserRole + 2;
constexpr int kStillLabelRole = Qt::UserRole + 3;
}

StillGalleryDock::StillGalleryDock(QWidget *parent)
    : QDockWidget(tr("Still Gallery"), parent)
{
    setObjectName(QStringLiteral("stillGalleryDock"));

    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    m_list = new QListWidget(root);
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(160, 90));
    m_list->setGridSize(QSize(180, 132));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setWordWrap(true);
    layout->addWidget(m_list, 1);

    auto *modeRow = new QHBoxLayout();
    auto *modeLabel = new QLabel(tr("Compare Method:"), root);
    modeRow->addWidget(modeLabel);
    m_modeCombo = new QComboBox(root);
    m_modeCombo->addItem(tr("Horizontal Wipe"));
    m_modeCombo->addItem(tr("Vertical Wipe"));
    m_modeCombo->addItem(tr("Side by Side"));
    modeLabel->setBuddy(m_modeCombo);
    modeRow->addWidget(m_modeCombo, 1);
    layout->addLayout(modeRow);

    auto *positionRow = new QHBoxLayout();
    m_positionLabel = new QLabel(root);
    m_positionSlider = new QSlider(Qt::Horizontal, root);
    m_positionSlider->setRange(0, 100);
    m_positionSlider->setValue(50);
    m_positionLabel->setBuddy(m_positionSlider);
    positionRow->addWidget(m_positionLabel);
    positionRow->addWidget(m_positionSlider, 1);
    layout->addLayout(positionRow);
    updatePositionLabel(m_positionSlider->value());

    setWidget(root);

    connect(m_list, &QListWidget::itemDoubleClicked,
            this, &StillGalleryDock::onItemDoubleClicked);
    connect(m_list, &QListWidget::customContextMenuRequested,
            this, &StillGalleryDock::showItemMenu);
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool hasStills = m_list->count() > 0
            && !idForItem(m_list->item(0)).isEmpty();
        m_positionSlider->setEnabled(hasStills && index != 2);
        emit comparisonModeChanged(index);
    });
    connect(m_positionSlider, &QSlider::valueChanged, this, [this](int value) {
        updatePositionLabel(value);
        emit comparisonPositionChanged(static_cast<double>(value) / 100.0);
    });
}

void StillGalleryDock::setStore(stillstore::StillStore *store)
{
    m_store = store;
    refresh();
}

void StillGalleryDock::refresh()
{
    m_list->clear();
    m_modeCombo->setEnabled(false);
    m_positionSlider->setEnabled(false);
    if (!m_store)
        return;

    QString error;
    const QVector<stillstore::Still> stills = m_store->list(&error);
    if (!error.isEmpty()) {
        auto *errorItem = new QListWidgetItem(tr("Cannot load stills list"), m_list);
        errorItem->setTextAlignment(Qt::AlignCenter);
        errorItem->setToolTip(error);
        errorItem->setFlags(Qt::NoItemFlags);
        return;
    }

    const bool hasStills = !stills.isEmpty();
    m_modeCombo->setEnabled(hasStills);
    m_positionSlider->setEnabled(hasStills && m_modeCombo->currentIndex() != 2);
    if (!hasStills) {
        auto *emptyItem = new QListWidgetItem(
            tr("No saved stills\nView > Save Still"), m_list);
        emptyItem->setTextAlignment(Qt::AlignCenter);
        emptyItem->setFlags(Qt::NoItemFlags);
        return;
    }

    for (const stillstore::Still &still : stills) {
        const QString title = still.label.isEmpty()
            ? still.timestamp.toLocalTime().toString(QStringLiteral("yyyy/MM/dd HH:mm:ss"))
            : still.label;
        const QString project = still.projectName.isEmpty()
            ? tr("No project name")
            : still.projectName;
        auto *item = new QListWidgetItem(title + QLatin1Char('\n') + project, m_list);
        item->setData(kStillIdRole, still.id);
        item->setData(kStillPathRole, still.filePath);
        item->setData(kStillLabelRole, still.label);
        const QImage thumbnail(still.filePath);
        if (!thumbnail.isNull())
            item->setIcon(QPixmap::fromImage(thumbnail));
        item->setToolTip(tr("Double-click to set as comparison target"));
    }
}

void StillGalleryDock::selectStill(const QString &id)
{
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (idForItem(item) == id) {
            m_list->setCurrentItem(item);
            m_list->scrollToItem(item);
            return;
        }
    }
}

QString StillGalleryDock::idForItem(const QListWidgetItem *item) const
{
    return item ? item->data(kStillIdRole).toString() : QString();
}

void StillGalleryDock::updatePositionLabel(int value)
{
    m_positionLabel->setText(tr("Position: %1%").arg(value));
}

void StillGalleryDock::onItemDoubleClicked(QListWidgetItem *item)
{
    if (!item || idForItem(item).isEmpty())
        return;
    const QImage image(item->data(kStillPathRole).toString());
    if (image.isNull()) {
        QMessageBox::warning(this, tr("Still Gallery"),
                             tr("Cannot open still image."));
        return;
    }
    emit stillSelected(idForItem(item), image);
}

void StillGalleryDock::showItemMenu(const QPoint &position)
{
    QListWidgetItem *item = m_list->itemAt(position);
    if (!item || !m_store || idForItem(item).isEmpty())
        return;

    QMenu menu(this);
    QAction *labelAction = menu.addAction(tr("Change Label..."));
    QAction *deleteAction = menu.addAction(tr("Delete"));
    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(position));
    if (!chosen)
        return;

    const QString id = idForItem(item);
    QString error;
    if (chosen == labelAction) {
        bool accepted = false;
        const QString label = QInputDialog::getText(
            this, tr("Change Label"), tr("Label:"), QLineEdit::Normal,
            item->data(kStillLabelRole).toString(), &accepted);
        if (!accepted)
            return;
        if (!m_store->setLabel(id, label, &error)) {
            QMessageBox::warning(this, tr("Change Label"), error);
            return;
        }
        refresh();
        selectStill(id);
        return;
    }

    if (QMessageBox::question(this, tr("Delete Still"),
                              tr("Delete the selected still?"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const QString imagePath = item->data(kStillPathRole).toString();
    const bool removed = m_store->remove(id, &error);
    // remove() can delete the PNG and then fail to persist index.json. Always
    // reload so StillStore::list() can repair that partial state. If the PNG
    // is already gone, notify the owner even though remove() returned false;
    // otherwise MainWindow would keep comparing against an orphaned QImage.
    refresh();
    if (!removed) {
        if (!QFileInfo::exists(imagePath))
            emit stillRemoved(id);
        QMessageBox::warning(this, tr("Delete Still"), error);
        return;
    }
    emit stillRemoved(id);
}
