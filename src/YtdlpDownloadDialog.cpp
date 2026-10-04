#include "YtdlpDownloadDialog.h"
#include "YtdlpDownloader.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

YtdlpDownloadDialog::YtdlpDownloadDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Download YouTube Video"));
    setMinimumWidth(480);

    // default output dir
    m_outputDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                  + QStringLiteral("/v-simple-editor-imports");

    // downloader (owned by this dialog via QObject parent)
    m_downloader = new YtdlpDownloader(this);
    connect(m_downloader, &YtdlpDownloader::progressUpdated,
            this,         &YtdlpDownloadDialog::onProgressUpdated);
    connect(m_downloader, &YtdlpDownloader::finished,
            this,         &YtdlpDownloadDialog::onFinished);

    // --- widgets ---
    auto* urlLabel = new QLabel(tr("YouTube URL:"), this);

    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QStringLiteral("https://youtube.com/watch?v=..."));

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);

    m_statusLabel = new QLabel(tr("Waiting..."), this);

    m_downloadButton = new QPushButton(tr("Download"), this);
    m_cancelButton   = new QPushButton(tr("Cancel"), this);
    m_closeButton    = new QPushButton(tr("Close"), this);

    m_cancelButton->setEnabled(false);

    // --- layout ---
    auto* buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(m_downloadButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_closeButton);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(urlLabel);
    mainLayout->addWidget(m_urlEdit);
    mainLayout->addWidget(m_progressBar);
    mainLayout->addWidget(m_statusLabel);
    mainLayout->addLayout(buttonLayout);

    // --- connections ---
    connect(m_downloadButton, &QPushButton::clicked, this, &YtdlpDownloadDialog::onDownloadClicked);
    connect(m_cancelButton,   &QPushButton::clicked, this, &YtdlpDownloadDialog::onCancelClicked);
    connect(m_closeButton,    &QPushButton::clicked, this, &QDialog::reject);
}

YtdlpDownloadDialog::~YtdlpDownloadDialog() = default;

QString YtdlpDownloadDialog::downloadedFilePath() const
{
    return m_downloadedFilePath;
}

void YtdlpDownloadDialog::setOutputDir(const QString& dir)
{
    m_outputDir = dir;
}

void YtdlpDownloadDialog::onDownloadClicked()
{
    const QString url = m_urlEdit->text().trimmed();

    if (!YtdlpDownloader::isYoutubeUrl(url)) {
        QMessageBox::warning(this, tr("Input Error"),
                             tr("Not a YouTube URL.\nPlease enter it in the https://youtube.com/watch?v=... format."));
        return;
    }

    m_downloadedFilePath.clear();
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);
    m_statusLabel->setText(tr("Downloading..."));
    m_downloadButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_closeButton->setEnabled(false);

    m_downloader->start(url, m_outputDir);
}

void YtdlpDownloadDialog::onCancelClicked()
{
    m_downloader->cancel();
    m_statusLabel->setText(tr("Canceling..."));
    m_cancelButton->setEnabled(false);
}

void YtdlpDownloadDialog::onProgressUpdated(int percent, const QString& message)
{
    m_progressBar->setValue(percent);
    m_statusLabel->setText(message);
}

void YtdlpDownloadDialog::onFinished(bool ok, const QString& outputPath, const QString& errorMessage)
{
    m_downloadButton->setEnabled(true);
    m_cancelButton->setEnabled(false);
    m_closeButton->setEnabled(true);

    if (ok) {
        m_downloadedFilePath = outputPath;
        m_progressBar->setValue(100);
        m_statusLabel->setText(tr("Done: %1").arg(QFileInfo(outputPath).fileName()));
        QDialog::accept();
    } else {
        m_progressBar->setVisible(false);
        m_statusLabel->setText(tr("Error: ") + errorMessage);
    }
}
