#include "ShapeModifierDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

ShapeModifierDialog::ShapeModifierDialog(const ShapeModifiers &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Shape Modifier"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Edit the shape at the start of the clip."), this));
    auto *repeaterBox = new QGroupBox(QStringLiteral("Repeater"), this);
    auto *repeatForm = new QFormLayout(repeaterBox);
    m_repeater = new QCheckBox(QStringLiteral("Enabled"), repeaterBox);
    m_repeater->setChecked(initial.repeater.enabled);
    repeatForm->addRow(m_repeater);
    m_copies = new QSpinBox(repeaterBox);
    m_copies->setRange(1, 1000);
    m_copies->setValue(initial.repeater.copies);
    repeatForm->addRow(QStringLiteral("Copies"), m_copies);
    auto spin = [this](QFormLayout *form, const QString &label, const QString &name,
                       double low, double high, double value, const QString &suffix) {
        auto *box = new QDoubleSpinBox(this);
        box->setObjectName(name);
        box->setDecimals(4);
        box->setRange(low, high);
        box->setSuffix(suffix);
        box->setValue(value);
        form->addRow(label, box);
        connect(box, &QDoubleSpinBox::valueChanged, this,
                [this](double) { emit modifiersChanged(); });
        return box;
    };
    m_offsetX = spin(repeatForm, QStringLiteral("Offset X"), QStringLiteral("offsetX"),
                     -1000000, 1000000, initial.repeater.offset.x(), QStringLiteral(" px"));
    m_offsetY = spin(repeatForm, QStringLiteral("Offset Y"), QStringLiteral("offsetY"),
                     -1000000, 1000000, initial.repeater.offset.y(), QStringLiteral(" px"));
    m_rotation = spin(repeatForm, QStringLiteral("Rotation"), QStringLiteral("rotation"),
                      -36000, 36000, initial.repeater.rotationDeg, QStringLiteral("°"));
    m_scale = spin(repeatForm, QStringLiteral("Scale"), QStringLiteral("scale"),
                   0, 10000, initial.repeater.scale * 100.0, QStringLiteral(" %"));
    m_opacity = spin(repeatForm, QStringLiteral("Last Copy Opacity"), QStringLiteral("opacityEnd"),
                     0, 100, initial.repeater.opacityEnd * 100.0, QStringLiteral(" %"));
    layout->addWidget(repeaterBox);

    auto *trimBox = new QGroupBox(QStringLiteral("Trim Paths"), this);
    auto *trimForm = new QFormLayout(trimBox);
    m_trim = new QCheckBox(QStringLiteral("Enabled"), trimBox);
    m_trim->setChecked(initial.trim.enabled);
    trimForm->addRow(m_trim);
    m_start = spin(trimForm, QStringLiteral("Start"), QStringLiteral("startPct"),
                   0, 100, initial.trim.startPct, QStringLiteral(" %"));
    m_end = spin(trimForm, QStringLiteral("End"), QStringLiteral("endPct"),
                 0, 100, initial.trim.endPct, QStringLiteral(" %"));
    m_offset = spin(trimForm, QStringLiteral("Offset"), QStringLiteral("offsetPct"),
                    -1000000, 1000000, initial.trim.offsetPct, QStringLiteral(" %"));
    layout->addWidget(trimBox);
    connect(m_repeater, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_trim, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_copies, &QSpinBox::valueChanged, this, [this](int) { emit modifiersChanged(); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Apply"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

ShapeModifiers ShapeModifierDialog::modifiers() const
{
    ShapeModifiers m;
    m.repeater.enabled = m_repeater->isChecked();
    m.repeater.copies = m_copies->value();
    m.repeater.offset = QPointF(m_offsetX->value(), m_offsetY->value());
    m.repeater.rotationDeg = m_rotation->value();
    m.repeater.scale = m_scale->value() / 100.0;
    m.repeater.opacityEnd = m_opacity->value() / 100.0;
    m.trim.enabled = m_trim->isChecked();
    m.trim.startPct = m_start->value();
    m.trim.endPct = m_end->value();
    m.trim.offsetPct = m_offset->value();
    return m;
}
