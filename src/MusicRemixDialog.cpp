#include "MusicRemixDialog.h"
#include "MusicRemix.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>

#include <cmath>

namespace {

QString formatMinutesSeconds(double seconds)
{
    const int rounded = qMax(0, qRound(seconds));
    const int minutes = rounded / 60;
    const int secs = rounded % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

} // namespace

MusicRemixDialog::MusicRemixDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Music Remix"));
    setMinimumWidth(360);

    m_bpmLabel = new QLabel(QStringLiteral("Detected BPM: --"), this);
    m_durationLabel = new QLabel(QStringLiteral("Target duration (mm:ss): 00:00"), this);

    m_targetSpin = new QDoubleSpinBox(this);
    m_targetSpin->setRange(0.05, remix::kMaxTargetSec);
    m_targetSpin->setDecimals(2);
    m_targetSpin->setSingleStep(1.0);
    m_targetSpin->setSuffix(QStringLiteral(" sec"));
    connect(m_targetSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { updateDurationLabel(value); });

    m_rippleCheck = new QCheckBox(QStringLiteral("Ripple subsequent clips"), this);
    m_rippleCheck->setChecked(false);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Detection result:"), m_bpmLabel);
    form->addRow(QStringLiteral("Target duration (s):"), m_targetSpin);
    form->addRow(QStringLiteral("Display:"), m_durationLabel);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Apply"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_rippleCheck);
    layout->addWidget(buttons);
}

void MusicRemixDialog::setDetectedBpm(double bpm)
{
    if (m_bpmLabel) {
        m_bpmLabel->setText(std::isfinite(bpm) && bpm > 0.0
                                ? QStringLiteral("Detected BPM: %1").arg(bpm, 0, 'f', 1)
                                : QStringLiteral("Detected BPM: --"));
    }
}

void MusicRemixDialog::setTargetDuration(double seconds)
{
    if (!m_targetSpin)
        return;
    m_targetSpin->setValue(qBound(m_targetSpin->minimum(),
                                  seconds, m_targetSpin->maximum()));
    updateDurationLabel(m_targetSpin->value());
}

double MusicRemixDialog::targetDuration() const
{
    return m_targetSpin ? m_targetSpin->value() : 0.0;
}

bool MusicRemixDialog::rippleFollowingClips() const
{
    return m_rippleCheck && m_rippleCheck->isChecked();
}

void MusicRemixDialog::updateDurationLabel(double seconds)
{
    if (m_durationLabel)
        m_durationLabel->setText(QStringLiteral("Target duration (mm:ss): %1")
                                     .arg(formatMinutesSeconds(seconds)));
}
