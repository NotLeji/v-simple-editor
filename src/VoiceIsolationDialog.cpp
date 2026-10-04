#include "VoiceIsolationDialog.h"

#include "libavcore/AudioExtract.h"

#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QDir>
#include <QEventLoop>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QtGlobal>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

QDoubleSpinBox *makeDoubleSpin(QWidget *parent,
                               double minimum,
                               double maximum,
                               double step,
                               int decimals,
                               double value)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setValue(value);
    spin->setKeyboardTracking(false);
    return spin;
}

void bindSliderAndSpin(QSlider *slider,
                       QDoubleSpinBox *spin,
                       QObject *owner,
                       std::function<void()> changed)
{
    QObject::connect(slider, &QSlider::valueChanged, owner,
                     [spin, changed](int value) {
        const QSignalBlocker blocker(spin);
        spin->setValue(value / 100.0);
        changed();
    });
    QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), owner,
                     [slider, changed](double value) {
        const QSignalBlocker blocker(slider);
        slider->setValue(qRound(value * 100.0));
        changed();
    });
}

QByteArray toPcm16(const QVector<float> &samples)
{
    QByteArray pcm;
    if (samples.size() > std::numeric_limits<int>::max() / 2)
        return pcm;
    pcm.resize(samples.size() * 2);
    for (int i = 0; i < samples.size(); ++i) {
        double value = std::isfinite(samples[i])
            ? static_cast<double>(samples[i]) : 0.0;
        value = std::clamp(value, -1.0, 1.0);
        const qint16 sample = static_cast<qint16>(std::lround(value * 32767.0));
        pcm[i * 2] = static_cast<char>(sample & 0xff);
        pcm[i * 2 + 1] = static_cast<char>((sample >> 8) & 0xff);
    }
    return pcm;
}

} // namespace

VoiceIsolationDialog::VoiceIsolationDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Voice Isolation (Speech Enhancement)"));
    setModal(true);
    resize(720, 680);
    setMinimumWidth(640);

    m_sourceLabel = new QLabel(tr("Audio Source Not Set"), this);
    m_sourceLabel->setWordWrap(true);
    m_sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sourceLabel->setMinimumHeight(34);

    m_rangeStartSpin = makeDoubleSpin(this, 0.0, 0.0, 0.1, 3, 0.0);
    m_rangeEndSpin = makeDoubleSpin(this, 0.0, 0.0, 0.1, 3, 0.0);
    m_rangeStartSpin->setSuffix(tr(" sec"));
    m_rangeEndSpin->setSuffix(tr(" sec"));
    connect(m_rangeStartSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::onRangeChanged);
    connect(m_rangeEndSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::onRangeChanged);

    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(tr("Voice only"), static_cast<int>(voiceiso::OutputMode::VoiceOnly));
    m_modeCombo->addItem(tr("Background only"), static_cast<int>(voiceiso::OutputMode::BackgroundOnly));
    m_modeCombo->addItem(tr("Mix"), static_cast<int>(voiceiso::OutputMode::Mix));
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);

    m_strengthSlider = new QSlider(Qt::Horizontal, this);
    m_strengthSlider->setRange(0, 100);
    m_strengthSlider->setSingleStep(5);
    m_strengthSlider->setValue(80);
    m_strengthSpin = makeDoubleSpin(this, 0.0, 1.0, 0.05, 2, 0.8);
    bindSliderAndSpin(m_strengthSlider, m_strengthSpin, this,
                      [this]() { invalidateAnalysis(); });

    m_voiceGainSpin = makeDoubleSpin(this, -60.0, 24.0, 0.5, 1, 0.0);
    m_voiceGainSpin->setSuffix(tr(" dB"));
    m_backgroundGainSpin = makeDoubleSpin(this, -60.0, 24.0, 0.5, 1, -18.0);
    m_backgroundGainSpin->setSuffix(tr(" dB"));
    m_lowHzSpin = makeDoubleSpin(this, 0.0, 24000.0, 10.0, 0, 80.0);
    m_lowHzSpin->setSuffix(tr(" Hz"));
    m_highHzSpin = makeDoubleSpin(this, 0.0, 24000.0, 100.0, 0, 8000.0);
    m_highHzSpin->setSuffix(tr(" Hz"));
    m_noiseLearnSpin = makeDoubleSpin(this, 0.0, 5.0, 0.1, 1, 0.5);
    m_noiseLearnSpin->setSuffix(tr(" sec"));
    m_adaptiveNoiseCheck = new QCheckBox(tr("Use adaptive noise floor"), this);
    m_adaptiveNoiseCheck->setChecked(true);
    m_harmonicSlider = new QSlider(Qt::Horizontal, this);
    m_harmonicSlider->setRange(0, 100);
    m_harmonicSlider->setSingleStep(5);
    m_harmonicSlider->setValue(50);
    m_harmonicSpin = makeDoubleSpin(this, 0.0, 1.0, 0.05, 2, 0.5);
    bindSliderAndSpin(m_harmonicSlider, m_harmonicSpin, this,
                      [this]() { invalidateAnalysis(); });
    m_smoothingSlider = new QSlider(Qt::Horizontal, this);
    m_smoothingSlider->setRange(0, 100);
    m_smoothingSlider->setSingleStep(5);
    m_smoothingSlider->setValue(70);
    m_smoothingSpin = makeDoubleSpin(this, 0.0, 1.0, 0.05, 2, 0.7);
    bindSliderAndSpin(m_smoothingSlider, m_smoothingSpin, this,
                      [this]() { invalidateAnalysis(); });

    auto *sourceGroup = new QGroupBox(tr("Target Clip and Range"), this);
    auto *sourceForm = new QFormLayout(sourceGroup);
    sourceForm->addRow(tr("Clip:"), m_sourceLabel);
    auto *rangeRow = new QHBoxLayout;
    rangeRow->addWidget(m_rangeStartSpin);
    rangeRow->addWidget(new QLabel(tr("From"), sourceGroup));
    rangeRow->addWidget(m_rangeEndSpin);
    rangeRow->addWidget(new QLabel(tr("to"), sourceGroup));
    rangeRow->addStretch(1);
    sourceForm->addRow(tr("Processing range:"), rangeRow);

    auto *separationGroup = new QGroupBox(tr("Separation"), this);
    auto *separationForm = new QFormLayout(separationGroup);
    auto *strengthRow = new QHBoxLayout;
    strengthRow->addWidget(m_strengthSlider, 1);
    strengthRow->addWidget(m_strengthSpin);
    separationForm->addRow(tr("Separation strength:"), strengthRow);
    separationForm->addRow(tr("Output mode:"), m_modeCombo);
    separationForm->addRow(tr("Voice gain:"), m_voiceGainSpin);
    separationForm->addRow(tr("Background gain:"), m_backgroundGainSpin);
    auto *bandRow = new QHBoxLayout;
    bandRow->addWidget(m_lowHzSpin);
    bandRow->addWidget(new QLabel(tr("From"), separationGroup));
    bandRow->addWidget(m_highHzSpin);
    bandRow->addStretch(1);
    separationForm->addRow(tr("Voice band:"), bandRow);

    auto *analysisGroup = new QGroupBox(tr("Analysis and Voiceness"), this);
    auto *analysisForm = new QFormLayout(analysisGroup);
    analysisForm->addRow(tr("Noise learning:"), m_noiseLearnSpin);
    analysisForm->addRow(QString(), m_adaptiveNoiseCheck);
    auto *harmonicRow = new QHBoxLayout;
    harmonicRow->addWidget(m_harmonicSlider, 1);
    harmonicRow->addWidget(m_harmonicSpin);
    analysisForm->addRow(tr("Harmonic structure:"), harmonicRow);
    auto *smoothingRow = new QHBoxLayout;
    smoothingRow->addWidget(m_smoothingSlider, 1);
    smoothingRow->addWidget(m_smoothingSpin);
    analysisForm->addRow(tr("Temporal smoothing:"), smoothingRow);

    m_ratioLabel = new QLabel(tr("Estimated Voice Ratio: Not Analyzed"), this);
    m_ratioLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel = new QLabel(tr("Set a target to analyze."), this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setMinimumHeight(32);

    m_analyzeButton = new QPushButton(tr("Analyze"), this);
    m_previewButton = new QPushButton(tr("Preview Playback"), this);
    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setDefault(true);
    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(m_analyzeButton, &QPushButton::clicked,
            this, &VoiceIsolationDialog::onAnalyzeClicked);
    connect(m_previewButton, &QPushButton::clicked,
            this, &VoiceIsolationDialog::onPreviewClicked);
    connect(m_applyButton, &QPushButton::clicked,
            this, &VoiceIsolationDialog::onApplyClicked);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    auto *actionRow = new QHBoxLayout;
    actionRow->addWidget(m_analyzeButton);
    actionRow->addWidget(m_previewButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_applyButton);
    actionRow->addWidget(closeButton);

    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(1.0);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOutput);
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &VoiceIsolationDialog::onPlayerStateChanged);
    m_previewFile.setAutoRemove(true);
    m_previewFile.setFileTemplate(
        QDir::tempPath() + QStringLiteral("/v-simple-editor-voice-XXXXXX.wav"));

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->addWidget(sourceGroup);
    layout->addWidget(separationGroup);
    layout->addWidget(analysisGroup);
    layout->addWidget(m_ratioLabel);
    layout->addWidget(m_statusLabel);
    layout->addLayout(actionRow);

    connect(m_voiceGainSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_backgroundGainSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_lowHzSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_highHzSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_noiseLearnSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_adaptiveNoiseCheck, &QCheckBox::toggled,
            this, &VoiceIsolationDialog::invalidateAnalysis);

    updateActionState();
}

void VoiceIsolationDialog::setAudioSource(const QString &label,
                                          const QVector<float> &samples,
                                          int sampleRate)
{
    if (m_player)
        m_player->stop();
    m_samples = samples;
    m_processedSamples = samples;
    m_sampleRate = sampleRate;
    m_hasAnalysis = false;
    const double duration = (sampleRate > 0 && !samples.isEmpty())
        ? samples.size() / static_cast<double>(sampleRate) : 0.0;
    m_sourceLabel->setText(label.isEmpty() ? tr("Audio Source Not Set") : label);
    m_rangeStartSpin->setRange(0.0, duration);
    m_rangeEndSpin->setRange(0.0, duration);
    {
        const QSignalBlocker startBlocker(m_rangeStartSpin);
        const QSignalBlocker endBlocker(m_rangeEndSpin);
        m_rangeStartSpin->setValue(0.0);
        m_rangeEndSpin->setValue(duration);
    }
    m_ratioLabel->setText(tr("Estimated Voice Ratio: Not Analyzed"));
    m_statusLabel->setText(samples.isEmpty() || sampleRate <= 0
                               ? tr("Could not load audio samples.")
                               : tr("You can run analysis or preview playback."));
    updateActionState();
}

void VoiceIsolationDialog::setInitialRange(double startSeconds,
                                           double endSeconds)
{
    if (m_samples.isEmpty() || m_sampleRate <= 0)
        return;
    const double duration = m_samples.size() / static_cast<double>(m_sampleRate);
    const double start = std::clamp(startSeconds, 0.0, duration);
    const double end = std::clamp(endSeconds, start, duration);
    const QSignalBlocker startBlocker(m_rangeStartSpin);
    const QSignalBlocker endBlocker(m_rangeEndSpin);
    m_rangeStartSpin->setValue(start);
    m_rangeEndSpin->setValue(end);
}

double VoiceIsolationDialog::rangeStartSeconds() const
{
    return m_rangeStartSpin ? m_rangeStartSpin->value() : 0.0;
}

double VoiceIsolationDialog::rangeEndSeconds() const
{
    return m_rangeEndSpin ? m_rangeEndSpin->value() : 0.0;
}

voiceiso::VoiceIsolationParams VoiceIsolationDialog::paramsFromUi() const
{
    voiceiso::VoiceIsolationParams params;
    params.mode = static_cast<voiceiso::OutputMode>(
        m_modeCombo->currentData().toInt());
    params.strength = m_strengthSpin->value();
    params.voiceGainDb = m_voiceGainSpin->value();
    params.backgroundGainDb = m_backgroundGainSpin->value();
    params.voiceLowHz = m_lowHzSpin->value();
    params.voiceHighHz = m_highHzSpin->value();
    params.noiseLearnSeconds = m_noiseLearnSpin->value();
    params.adaptiveNoise = m_adaptiveNoiseCheck->isChecked();
    params.harmonicWeight = m_harmonicSpin->value();
    params.smoothingFactor = m_smoothingSpin->value();
    return params;
}

bool VoiceIsolationDialog::processCurrentRange()
{
    if (m_samples.isEmpty() || m_sampleRate <= 0) {
        m_statusLabel->setText(tr("No valid audio samples."));
        return false;
    }
    const qint64 first64 = qBound<qint64>(
        0, qRound64(rangeStartSeconds() * m_sampleRate), m_samples.size() - 1);
    const qint64 end64 = qBound<qint64>(
        first64 + 1, qRound64(rangeEndSeconds() * m_sampleRate), m_samples.size());
    if (end64 <= first64) {
        m_statusLabel->setText(tr("Please set the processing range end after the start."));
        return false;
    }

    setBusy(true);
    // Give the user-visible busy state one event-loop turn before the
    // synchronous DSP work starts. User input remains blocked by setBusy().
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QVector<float> range;
    range.reserve(static_cast<qsizetype>(end64 - first64));
    for (qint64 i = first64; i < end64; ++i)
        range.append(m_samples[static_cast<int>(i)]);

    const voiceiso::VoiceIsolationParams params = paramsFromUi();
    const voiceiso::VoiceIsolationResult result = voiceiso::isolate(
        range, m_sampleRate, params);
    const int rangeLength = static_cast<int>(end64 - first64);
    if (static_cast<int>(result.output.size()) != rangeLength) {
        m_statusLabel->setText(tr("Could not allocate memory needed for voice isolation. Shorten the range and try again."));
        setBusy(false);
        return false;
    }
    const QVector<double> &floor = result.noiseFloor;
    m_processedSamples = m_samples;
    for (int i = 0; i < static_cast<int>(result.output.size()); ++i)
        m_processedSamples[static_cast<int>(first64) + i] = result.output[i];
    m_hasAnalysis = true;

    double floorMean = 0.0;
    for (double value : floor)
        floorMean += value;
    if (!floor.isEmpty())
        floorMean /= floor.size();
    const double floorDb = 10.0 * std::log10(std::max(1.0e-12, floorMean));
    m_ratioLabel->setText(tr("Estimated voice ratio: %1% | Noise floor: %2 dBFS")
                              .arg(result.estimatedVoiceRatio * 100.0, 0, 'f', 1)
                              .arg(floorDb, 0, 'f', 1));
    m_statusLabel->setText(tr("Analysis complete. You can preview the processed result for the range."));
    setBusy(false);
    return true;
}

bool VoiceIsolationDialog::writePreviewFile(const QVector<float> &samples,
                                            QString *error)
{
    if (m_sampleRate <= 0 || samples.isEmpty()) {
        if (error)
            *error = tr("Preview target is empty.");
        return false;
    }
    if (m_previewFile.fileName().isEmpty()) {
        if (!m_previewFile.open()) {
            if (error)
                *error = m_previewFile.errorString();
            return false;
        }
        m_previewFile.close();
    }
    const QByteArray pcm = toPcm16(samples);
    if (pcm.isEmpty()) {
        if (error)
            *error = tr("Failed to create preview PCM.");
        return false;
    }
    return libavcore::writePcm16AsWav(
        m_previewFile.fileName(), pcm, m_sampleRate, 1, error);
}

void VoiceIsolationDialog::setBusy(bool busy)
{
    m_busy = busy;
    updateActionState();
    if (m_busy)
        m_statusLabel->setText(tr("Analyzing..."));
}

void VoiceIsolationDialog::invalidateAnalysis()
{
    if (m_busy)
        return;
    m_hasAnalysis = false;
    if (!m_samples.isEmpty())
        m_statusLabel->setText(tr("Settings changed. Please re-analyze."));
}

void VoiceIsolationDialog::updateActionState()
{
    const bool ready = !m_samples.isEmpty() && m_sampleRate > 0;
    const bool canAct = ready && !m_busy;
    m_analyzeButton->setEnabled(canAct);
    m_previewButton->setEnabled(canAct);
    m_applyButton->setEnabled(canAct);
    m_rangeStartSpin->setEnabled(canAct);
    m_rangeEndSpin->setEnabled(canAct);
    m_modeCombo->setEnabled(canAct);
    m_strengthSlider->setEnabled(canAct);
    m_strengthSpin->setEnabled(canAct);
    m_voiceGainSpin->setEnabled(canAct);
    m_backgroundGainSpin->setEnabled(canAct);
    m_lowHzSpin->setEnabled(canAct);
    m_highHzSpin->setEnabled(canAct);
    m_noiseLearnSpin->setEnabled(canAct);
    m_adaptiveNoiseCheck->setEnabled(canAct);
    m_harmonicSlider->setEnabled(canAct);
    m_harmonicSpin->setEnabled(canAct);
    m_smoothingSlider->setEnabled(canAct);
    m_smoothingSpin->setEnabled(canAct);
}

void VoiceIsolationDialog::onAnalyzeClicked()
{
    processCurrentRange();
}

void VoiceIsolationDialog::onPreviewClicked()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->stop();
        return;
    }
    if (!processCurrentRange())
        return;
    QString error;
    if (!writePreviewFile(m_processedSamples, &error)) {
        m_statusLabel->setText(tr("Failed to prepare preview: %1").arg(error));
        return;
    }
    m_player->setSource(QUrl::fromLocalFile(m_previewFile.fileName()));
    m_player->play();
    m_statusLabel->setText(tr("Playing preview."));
}

void VoiceIsolationDialog::onApplyClicked()
{
    if (!processCurrentRange())
        return;
    emit applied();
    accept();
}

void VoiceIsolationDialog::onRangeChanged()
{
    if (m_rangeStartSpin->value() > m_rangeEndSpin->value()) {
        const QSignalBlocker blocker(m_rangeEndSpin);
        m_rangeEndSpin->setValue(m_rangeStartSpin->value());
    }
    invalidateAnalysis();
}

void VoiceIsolationDialog::onPlayerStateChanged(QMediaPlayer::PlaybackState state)
{
    if (state == QMediaPlayer::PlayingState) {
        m_previewButton->setText(tr("Stop Preview"));
    } else {
        m_previewButton->setText(tr("Preview Playback"));
        if (!m_busy && m_hasAnalysis)
            m_statusLabel->setText(tr("Analysis complete. Apply it or adjust the settings."));
    }
}
