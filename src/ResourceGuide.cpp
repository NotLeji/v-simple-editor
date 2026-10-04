#include "ResourceGuide.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QGroupBox>
#include <QDesktopServices>
#include <QUrl>
#include <QFont>
#include <QFrame>

// --- Static Resource Data ---

QVector<ResourceCategory> ResourceGuideDialog::allCategories()
{
    return {
        // Video / Footage
        { "Video Footage", "🎬", {
            { "Pexels Videos",
              "https://www.pexels.com/videos/",
              "High-quality free stock video. Free for commercial use, no credit required" },
            { "Pixabay Videos",
              "https://pixabay.com/videos/",
              "Over 1 million free videos. Free for commercial use" },
            { "Coverr",
              "https://coverr.co/",
              "Short web-ready videos. New material added every 7 days" },
            { "Videvo",
              "https://www.videvo.net/",
              "Free video clips & motion graphics" },
            { "Mixkit",
              "https://mixkit.co/free-stock-video/",
              "High-quality free stock video. Free for commercial use" },
            { "Life of Vids",
              "https://lifeofvids.com/",
              "CC0 nature & landscape videos" }
        }},

        // Images / Photos
        { "Images & Photos", "📷", {
            { "Unsplash",
              "https://unsplash.com/",
              "High-resolution photos. Free for commercial use, no credit required" },
            { "Pexels Photos",
              "https://www.pexels.com/",
              "Free stock photos. Free for commercial use" },
            { "Pixabay Images",
              "https://pixabay.com/",
              "Photos, illustrations & vector images" },
            { "StockSnap.io",
              "https://stocksnap.io/",
              "CC0 high-quality photos" },
            { "Burst (Shopify)",
              "https://burst.shopify.com/",
              "Free business photos" },
            { "Pakutaso",
              "https://www.pakutaso.com/",
              "Free Japanese stock photos. Rich in people & landscapes" }
        }},

        // BGM / Music
        { "Music", "🎵", {
            { "DOVA-SYNDROME",
              "https://dova-s.jp/",
              "One of Japan's largest free BGM sites. Popular with YouTubers" },
            { "Amacha Music Workshop",
              "https://amachamusic.chagasi.com/",
              "Free BGM in many genres. Japanese" },
            { "Maou Damashii",
              "https://maou.audio/",
              "Free BGM & sound effects for games & videos" },
            { "FreePD",
              "https://freepd.com/",
              "Public domain music" },
            { "Incompetech (Kevin MacLeod)",
              "https://incompetech.com/music/",
              "CC music in many genres. Free with credit" },
            { "Mixkit Music",
              "https://mixkit.co/free-stock-music/",
              "High-quality free BGM. Free for commercial use" },
            { "YouTube Audio Library",
              "https://studio.youtube.com/channel/UC/music",
              "YouTube's official free music library" }
        }},

        // Sound Effects
        { "Sound Effects", "🔊", {
            { "Sound Effect Lab",
              "https://soundeffect-lab.info/",
              "Japanese sound effects site. Rich categories" },
            { "Freesound",
              "https://freesound.org/",
              "User-submitted. CC/CC0 licensed" },
            { "Zapsplat",
              "https://www.zapsplat.com/",
              "Over 150,000 free sound effects" },
            { "SoundBible",
              "https://soundbible.com/",
              "CC/public domain sound effects" },
            { "OtoLogic",
              "https://otologic.jp/",
              "Japanese. BGM, sound effects & jingles" },
            { "On-Jin",
              "https://on-jin.com/",
              "Japanese. Rich system & everyday sounds" }
        }},

        // Fonts
        { "Fonts", "🔤", {
            { "Google Fonts",
              "https://fonts.google.com/",
              "Over 1500 open-source fonts" },
            { "FontFree",
              "https://fontfree.me/",
              "Free Japanese font collection" },
            { "FONTBEAR",
              "https://fontbear.net/",
              "Free Japanese fonts for commercial use" },
            { "Font Meme",
              "https://fontmeme.com/",
              "Movie & brand-style fonts" },
            { "DaFont",
              "https://www.dafont.com/",
              "Rich decorative fonts (check license)" }
        }},

        // Icons / Illustrations
        { "Icons & Illustrations", "🎨", {
            { "unDraw",
              "https://undraw.co/illustrations",
              "Recolorable SVG illustrations" },
            { "Flaticon",
              "https://www.flaticon.com/",
              "Icon material (free version requires credit)" },
            { "Icons8",
              "https://icons8.com/",
              "Icons, photos, illustrations & music" },
            { "Irasutoya",
              "https://www.irasutoya.com/",
              "Japan's most famous free illustrations" },
            { "Loose Drawing",
              "https://loosedrawing.com/",
              "Simple free illustration material" },
            { "ICOOON MONO",
              "https://icooon-mono.com/",
              "Monochrome icon material. Free for commercial use" }
        }},

        // Textures / Backgrounds
        { "Textures & Backgrounds", "🖼", {
            { "Subtle Patterns",
              "https://www.toptal.com/designers/subtlepatterns/",
              "Delicate tile-pattern backgrounds" },
            { "Transparent Textures",
              "https://www.transparenttextures.com/",
              "Transparent textures" },
            { "Hero Patterns",
              "https://heropatterns.com/",
              "SVG-based repeating patterns" },
            { "Poly Haven",
              "https://polyhaven.com/",
              "HDR environment maps & textures. CC0" }
        }}
    };
}

// --- Dialog ---

ResourceGuideDialog::ResourceGuideDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Free Resource Guide");
    resize(700, 600);
    setupUI();
}

void ResourceGuideDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Header
    auto *headerLabel = new QLabel(
        "<h2>Free Resource Guide</h2>"
        "<p style='color:#888;'>A collection of free stock sites for video editing."
        "Please check each site's terms of use.</p>");
    headerLabel->setWordWrap(true);
    mainLayout->addWidget(headerLabel);

    // Scroll area
    auto *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *scrollWidget = new QWidget;
    auto *scrollLayout = new QVBoxLayout(scrollWidget);
    scrollLayout->setSpacing(12);

    auto categories = allCategories();
    for (const auto &cat : categories) {
        auto *group = new QGroupBox(QString("%1 %2").arg(cat.icon, cat.name));
        QFont groupFont = group->font();
        groupFont.setBold(true);
        groupFont.setPointSize(groupFont.pointSize() + 1);
        group->setFont(groupFont);

        auto *groupLayout = new QVBoxLayout(group);
        groupLayout->setSpacing(6);

        for (const auto &site : cat.sites) {
            auto *row = new QHBoxLayout;

            auto *linkBtn = new QPushButton(site.name);
            linkBtn->setCursor(Qt::PointingHandCursor);
            linkBtn->setFlat(true);
            linkBtn->setStyleSheet(
                "QPushButton { color: #4da6ff; text-align: left; font-weight: bold; "
                "text-decoration: underline; border: none; padding: 2px; }"
                "QPushButton:hover { color: #80c0ff; }");
            linkBtn->setFixedWidth(200);

            QString url = site.url;
            connect(linkBtn, &QPushButton::clicked, this, [this, url]() {
                openUrl(url);
            });

            auto *descLabel = new QLabel(site.description);
            descLabel->setWordWrap(true);
            descLabel->setStyleSheet("color: #aaa; padding: 2px;");

            row->addWidget(linkBtn);
            row->addWidget(descLabel, 1);
            groupLayout->addLayout(row);
        }

        scrollLayout->addWidget(group);
    }

    scrollLayout->addStretch();
    scrollArea->setWidget(scrollWidget);
    mainLayout->addWidget(scrollArea, 1);

    // Close button
    auto *closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    auto *bottomLayout = new QHBoxLayout;
    bottomLayout->addStretch();
    bottomLayout->addWidget(closeBtn);
    mainLayout->addLayout(bottomLayout);
}

void ResourceGuideDialog::openUrl(const QString &url)
{
    QDesktopServices::openUrl(QUrl(url));
}
