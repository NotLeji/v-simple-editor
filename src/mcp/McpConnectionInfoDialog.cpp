#include "McpConnectionInfoDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString tomlString(const QString& value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

QWidget *snippetRow(const QString& snippet, const QString& buttonText,
                    QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *text = new QPlainTextEdit(row);
    text->setReadOnly(true);
    text->setPlainText(snippet);
    text->setMinimumHeight(70);
    layout->addWidget(text, 1);

    auto *copy = new QPushButton(buttonText, row);
    copy->setToolTip(QStringLiteral("Copy this setting to the clipboard."));
    QObject::connect(copy, &QPushButton::clicked, row, [text]() {
        if (QClipboard *clipboard = QApplication::clipboard())
            clipboard->setText(text->toPlainText());
    });
    layout->addWidget(copy);
    return row;
}

} // namespace

McpConnectionInfoDialog::McpConnectionInfoDialog(const QString& endpoint,
                                                 const QString& token,
                                                 QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("MCP Server Connection Info"));
    resize(720, 520);

    auto *layout = new QVBoxLayout(this);

    auto *endpointLabel = new QLabel(QStringLiteral("Endpoint"), this);
    layout->addWidget(endpointLabel);
    auto *endpointEdit = new QLineEdit(endpoint, this);
    endpointEdit->setReadOnly(true);
    layout->addWidget(endpointEdit);

    auto *tokenLabel = new QLabel(QStringLiteral("Token"), this);
    layout->addWidget(tokenLabel);
    auto *tokenEdit = new QLineEdit(token, this);
    tokenEdit->setReadOnly(true);
    tokenEdit->setEchoMode(QLineEdit::Normal);
    layout->addWidget(tokenEdit);

    const QString claudeSnippet = QString::fromUtf8(
        QJsonDocument(QJsonObject{
            {QStringLiteral("mcpServers"), QJsonObject{
                {QStringLiteral("veditor"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("http")},
                    {QStringLiteral("url"), endpoint},
                    {QStringLiteral("headers"), QJsonObject{
                        {QStringLiteral("Authorization"),
                         QStringLiteral("Bearer ") + token}
                    }}
                }}
            }}
        }).toJson(QJsonDocument::Compact));

    layout->addWidget(new QLabel(QStringLiteral("For Claude Code"), this));
    layout->addWidget(snippetRow(claudeSnippet, QStringLiteral("Copy"), this));
    layout->addWidget(new QLabel(
        QStringLiteral("Usage: claude --mcp-config veditor-mcp.json --strict-mcp-config"),
        this));

    const QString executable = QDir::toNativeSeparators(
        QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath());
    const QString codexSnippet = QStringLiteral(
        "[mcp_servers.veditor]\n"
        "command = %1\n"
        "args = [\"--mcp-stdio\", \"--port\", \"%2\"]\n"
        "env = { VEDITOR_MCP_TOKEN = %3 }\n")
        .arg(tomlString(executable),
             QString::number(QUrl(endpoint).port()),
             tomlString(token));

    layout->addWidget(new QLabel(QStringLiteral("For Codex CLI"), this));
    layout->addWidget(snippetRow(codexSnippet, QStringLiteral("Copy"), this));

    auto *warning = new QLabel(
        QStringLiteral("An LLM connected to this server will edit the timeline without confirmation.\n"
                       "Changes can be undone with Ctrl+Z. Do not share the token with others."),
        this);
    warning->setStyleSheet(QStringLiteral("color: #d32f2f;"));
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
