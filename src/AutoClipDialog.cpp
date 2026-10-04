#include "AutoClipDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

AutoClipDialog::AutoClipDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Auto-Cut from Highlights"));
    setMinimumWidth(420);

    // --- widgets ---
    m_aspectCombo = new QComboBox(this);
    m_aspectCombo->addItem(tr("Original (0)"), 0.0);
    m_aspectCombo->addItem(tr("Vertical 9:16 (Shorts)"), 9.0 / 16.0);
    m_aspectCombo->addItem(tr("Square 1:1"), 1.0);
    m_aspectCombo->addItem(tr("Landscape 16:9"), 16.0 / 9.0);

    m_minDurationSpin = new QDoubleSpinBox(this);
    m_minDurationSpin->setRange(0.5, 30.0);
    m_minDurationSpin->setValue(3.0);
    m_minDurationSpin->setSuffix(tr(" sec"));

    m_maxDurationSpin = new QDoubleSpinBox(this);
    m_maxDurationSpin->setRange(1.0, 600.0);
    m_maxDurationSpin->setValue(60.0);
    m_maxDurationSpin->setSuffix(tr(" sec"));

    m_maxClipsSpin = new QSpinBox(this);
    m_maxClipsSpin->setRange(1, 100);
    m_maxClipsSpin->setValue(10);

    auto* descriptionLabel = new QLabel(
        tr("Automatically compute cut ranges from detected highlights."), this);
    descriptionLabel->setWordWrap(true);

    // --- layout ---
    auto* formLayout = new QFormLayout;
    formLayout->addRow(tr("Aspect:"), m_aspectCombo);
    formLayout->addRow(tr("Min length:"), m_minDurationSpin);
    formLayout->addRow(tr("Max length:"), m_maxDurationSpin);
    formLayout->addRow(tr("Max clips:"), m_maxClipsSpin);

    m_buttonBox = new QDialogButtonBox(this);
    m_computeButton = m_buttonBox->addButton(tr("Compute cut ranges"),
                                             QDialogButtonBox::AcceptRole);
    m_buttonBox->addButton(tr("Close"), QDialogButtonBox::RejectRole);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(descriptionLabel);
    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(m_buttonBox);

    // --- connections ---
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

autoclip::AutoClipConfig AutoClipDialog::config() const
{
    autoclip::AutoClipConfig cfg;
    cfg.targetAspect    = m_aspectCombo->currentData().toDouble();
    cfg.minDurationSec  = m_minDurationSpin->value();
    cfg.maxDurationSec  = m_maxDurationSpin->value();
    cfg.maxClips        = m_maxClipsSpin->value();
    return cfg;
}

void AutoClipDialog::setHighlightCount(int count)
{
    if (m_computeButton)
        m_computeButton->setEnabled(count > 0);
}

void AutoClipDialog::setSourceDuration(double durationSec)
{
    Q_UNUSED(durationSec);
}
