#include "CaptionStyle.h"

namespace caption {

Style defaultStyle()
{
    return Style{};
}

QVector<StylePreset> capCutStylePresets()
{
    Style popWhite = defaultStyle();
    popWhite.fontSizePt = 36;
    popWhite.bold = true;
    popWhite.textColor = Qt::white;
    popWhite.outlineColor = Qt::black;
    popWhite.outlineThickness = 5.0;
    popWhite.shadowColor = QColor(0, 0, 0, 140);
    popWhite.shadowOffset = QPointF(2.0, 2.0);
    popWhite.background = false;
    popWhite.backgroundColor = QColor(0, 0, 0, 0);
    popWhite.anchor = Anchor::BottomCenter;

    Style boxBlack = defaultStyle();
    boxBlack.fontSizePt = 30;
    boxBlack.bold = true;
    boxBlack.textColor = Qt::white;
    boxBlack.outlineColor = Qt::black;
    boxBlack.outlineThickness = 2.5;
    boxBlack.shadowColor = QColor(0, 0, 0, 0);
    boxBlack.shadowOffset = QPointF(0.0, 0.0);
    boxBlack.background = true;
    boxBlack.backgroundColor = QColor(0, 0, 0, 190);
    boxBlack.anchor = Anchor::BottomCenter;

    Style yellowHighlight = defaultStyle();
    yellowHighlight.fontSizePt = 34;
    yellowHighlight.bold = true;
    yellowHighlight.textColor = QColor(255, 230, 0);
    yellowHighlight.outlineColor = Qt::black;
    yellowHighlight.outlineThickness = 4.0;
    yellowHighlight.shadowColor = QColor(0, 0, 0, 220);
    yellowHighlight.shadowOffset = QPointF(4.0, 4.0);
    yellowHighlight.background = false;
    yellowHighlight.backgroundColor = QColor(0, 0, 0, 0);
    yellowHighlight.anchor = Anchor::BottomCenter;

    Style neon = defaultStyle();
    neon.fontSizePt = 32;
    neon.bold = true;
    neon.textColor = QColor(0, 255, 255);
    neon.outlineColor = QColor(255, 0, 180);
    neon.outlineThickness = 3.5;
    neon.shadowColor = QColor(0, 24, 40, 230);
    neon.shadowOffset = QPointF(3.0, 3.0);
    neon.background = false;
    neon.backgroundColor = QColor(0, 0, 0, 0);
    neon.anchor = Anchor::BottomCenter;

    Style minimal = defaultStyle();
    minimal.fontSizePt = 26;
    minimal.bold = false;
    minimal.textColor = Qt::white;
    minimal.outlineColor = Qt::black;
    minimal.outlineThickness = 1.0;
    minimal.shadowColor = QColor(0, 0, 0, 0);
    minimal.shadowOffset = QPointF(0.0, 0.0);
    minimal.background = false;
    minimal.backgroundColor = QColor(0, 0, 0, 0);
    minimal.anchor = Anchor::BottomCenter;

    return QVector<StylePreset>{
        { QStringLiteral("Pop White"), popWhite },
        { QStringLiteral("Box Black"), boxBlack },
        { QStringLiteral("Yellow highlight"), yellowHighlight },
        { QStringLiteral("Neon"), neon },
        { QStringLiteral("Minimal"), minimal }
    };
}

QStringList anchorNames()
{
    return QStringList{
        "Top left",   "Top Center",   "Top right",
        "Middle left",   "Center",     "Middle right",
        "Bottom left",   "Bottom Center",   "Bottom right"
    };
}

QString anchorToString(Anchor a)
{
    switch (a) {
    case Anchor::TopLeft:      return "TopLeft";
    case Anchor::TopCenter:    return "TopCenter";
    case Anchor::TopRight:     return "TopRight";
    case Anchor::MiddleLeft:   return "MiddleLeft";
    case Anchor::MiddleCenter: return "MiddleCenter";
    case Anchor::MiddleRight:  return "MiddleRight";
    case Anchor::BottomLeft:   return "BottomLeft";
    case Anchor::BottomCenter: return "BottomCenter";
    case Anchor::BottomRight:  return "BottomRight";
    }
    return "BottomCenter";
}

Anchor anchorFromString(const QString& s)
{
    if (s == "TopLeft")      return Anchor::TopLeft;
    if (s == "TopCenter")    return Anchor::TopCenter;
    if (s == "TopRight")     return Anchor::TopRight;
    if (s == "MiddleLeft")   return Anchor::MiddleLeft;
    if (s == "MiddleCenter") return Anchor::MiddleCenter;
    if (s == "MiddleRight")  return Anchor::MiddleRight;
    if (s == "BottomLeft")   return Anchor::BottomLeft;
    if (s == "BottomCenter") return Anchor::BottomCenter;
    if (s == "BottomRight")  return Anchor::BottomRight;
    return Anchor::BottomCenter;
}

} // namespace caption
