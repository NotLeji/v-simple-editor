#include "WhisperTranscribeDialog.h"

#include "SpeechRecognizer.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

WhisperTranscribeDialog::WhisperTranscribeDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Transcribe Video"));
    setMinimumWidth(480);

    // --- 入力ファイル ---
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(tr("Please select a video / audio file"));

    m_browseButton = new QPushButton(tr("Browse..."), this);

    auto* pathLayout = new QHBoxLayout;
    pathLayout->addWidget(m_pathEdit);
    pathLayout->addWidget(m_browseButton);

    // --- モデル (recognizer) ---
    m_modelCombo = new QComboBox(this);
    m_cliBrowseButton = new QPushButton(tr("Specify Executable…"), this);

    const auto recognizers = speech::availableRecognizers();
    for (const auto& r : recognizers) {
        if (r) {
            m_modelCombo->addItem(r->name());
        }
    }

    m_engineWarningLabel = new QLabel(
        tr("External engine whisper-cli not found. Using sample transcription."),
        this);
    m_engineWarningLabel->setWordWrap(true);
    m_engineWarningLabel->setStyleSheet(QStringLiteral("color: #b00020; font-weight: 600;"));

    m_engineInstallLabel = new QLabel(
        tr("Get whisper-cli from the whisper.cpp GitHub releases and add it to your PATH."),
        this);
    m_engineInstallLabel->setWordWrap(true);

    // --- 言語 ---
    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItem(tr("Auto (auto)"), QStringLiteral("auto"));
    m_languageCombo->addItem(tr("Japanese (ja)"), QStringLiteral("ja"));
    m_languageCombo->addItem(tr("English (en)"), QStringLiteral("en"));

    // --- フォーム ---
    auto* formLayout = new QFormLayout;
    formLayout->addRow(tr("Input file:"), pathLayout);
    formLayout->addRow(tr("Model:"), m_modelCombo);
    formLayout->addRow(tr("Language:"), m_languageCombo);

    // --- 結果表示 ---
    m_resultLabel = new QLabel(QString(), this);
    m_resultLabel->setWordWrap(true);

    // --- ボタン ---
    m_buttonBox = new QDialogButtonBox(this);
    auto* acceptButton = m_buttonBox->addButton(tr("Transcribe"), QDialogButtonBox::AcceptRole);
    m_buttonBox->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    Q_UNUSED(acceptButton);

    // --- レイアウト ---
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(m_engineWarningLabel);
    mainLayout->addWidget(m_cliBrowseButton);
    mainLayout->addWidget(m_engineInstallLabel);
    mainLayout->addWidget(m_resultLabel);
    mainLayout->addWidget(m_buttonBox);

    // --- 接続 ---
    connect(m_browseButton, &QPushButton::clicked, this, &WhisperTranscribeDialog::onBrowseClicked);
    connect(m_cliBrowseButton, &QPushButton::clicked,
            this, &WhisperTranscribeDialog::onCliBrowseClicked);
    connect(m_pathEdit, &QLineEdit::textChanged, this, &WhisperTranscribeDialog::updateAcceptState);
    connect(m_modelCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateRecognizerWarning();
    });
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateRecognizerWarning();
    updateAcceptState();
}

void WhisperTranscribeDialog::setMediaPath(const QString& path)
{
    m_pathEdit->setText(path);
}

void WhisperTranscribeDialog::setResultText(const QString& text)
{
    m_resultLabel->setText(text);
}

whisper::TranscribeRequest WhisperTranscribeDialog::request() const
{
    whisper::TranscribeRequest req;
    req.mediaPath      = m_pathEdit->text().trimmed();
    req.language       = m_languageCombo->currentData().toString();
    req.recognizerName = m_modelCombo->currentText();
    return req;
}

void WhisperTranscribeDialog::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select video / audio file"),
        QString(),
        tr("Media files (*.mp4 *.mov *.mkv *.avi *.webm *.wav *.mp3 *.m4a *.aac *.flac);;All files (*.*)"));

    if (!path.isEmpty()) {
        m_pathEdit->setText(path);
    }
}

void WhisperTranscribeDialog::onCliBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select whisper-cli executable"),
        QString(),
#ifdef Q_OS_WIN
        tr("Executables (*.exe);;All files (*.*)"));
#else
        tr("All files (*)"));
#endif
    if (path.isEmpty())
        return;

    QSettings settings;
    settings.setValue(QStringLiteral("whisper/cli_path"), path);
    settings.sync();

    const QString previousSelection = m_modelCombo->currentText();
    m_modelCombo->clear();
    const auto recognizers = speech::availableRecognizers();
    for (const auto& recognizer : recognizers) {
        if (recognizer)
            m_modelCombo->addItem(recognizer->name());
    }
    const int previousIndex = m_modelCombo->findText(previousSelection);
    if (previousIndex >= 0)
        m_modelCombo->setCurrentIndex(previousIndex);
    updateRecognizerWarning();
}

void WhisperTranscribeDialog::updateAcceptState()
{
    const bool hasPath = !m_pathEdit->text().trimmed().isEmpty();
    // Accept 系ボタンを mediaPath 有無で enable/disable
    const auto buttons = m_buttonBox->buttons();
    for (auto* b : buttons) {
        if (m_buttonBox->buttonRole(b) == QDialogButtonBox::AcceptRole) {
            b->setEnabled(hasPath);
        }
    }
}

void WhisperTranscribeDialog::updateRecognizerWarning()
{
    const whisperpath::Resolution resolution = whisperpath::resolveWhisperCli();
    const bool found = !resolution.executablePath.isEmpty();
    if (found) {
        m_engineWarningLabel->setText(
            tr("Detected whisper-cli: %1").arg(resolution.executablePath));
        m_engineWarningLabel->setStyleSheet(
            QStringLiteral("color: #17823b; font-weight: 600;"));
    } else {
        m_engineWarningLabel->setText(
            tr("External engine whisper-cli not found. Using sample transcription."));
        m_engineWarningLabel->setStyleSheet(
            QStringLiteral("color: #b00020; font-weight: 600;"));
    }
    m_cliBrowseButton->setVisible(!found);
    m_engineInstallLabel->setVisible(!found);
}
