#include "DolbyVisionMetadata.h"

#include <algorithm>

#include <QJsonArray>
#include <QtGlobal>

namespace dolbyvision {

// ===========================================================================
// PQ (SMPTE ST.2084) 変換
// ===========================================================================
double nitsToPq(double nits)
{
    if (!std::isfinite(nits) || nits <= 0.0)
        return 0.0;
    if (nits >= kPqPeakNits)
        return 1.0;

    // 正規化輝度 Y = nits / 10000。
    double y = nits / kPqPeakNits;

    const double ym1 = std::pow(y, kPqM1);
    const double num = kPqC1 + kPqC2 * ym1;
    const double den = 1.0 + kPqC3 * ym1;
    double pq = std::pow(num / den, kPqM2);
    if (pq < 0.0) pq = 0.0;
    if (pq > 1.0) pq = 1.0;
    return pq;
}

double pqToNits(double pq)
{
    if (!std::isfinite(pq) || pq <= 0.0)
        return 0.0;
    if (pq >= 1.0)
        return kPqPeakNits;

    const double ep = std::pow(pq, 1.0 / kPqM2);
    double num = ep - kPqC1;
    if (num < 0.0) num = 0.0;  // c1 未満は黒 (負の輝度を避ける)
    const double den = kPqC2 - kPqC3 * ep;
    if (den <= 0.0) return kPqPeakNits;  // 数値破綻ガード (ピークへ飽和)
    const double y = std::pow(num / den, 1.0 / kPqM1);
    return y * kPqPeakNits;
}

int nitsToPq12bit(double nits)
{
    const double pq = nitsToPq(nits);
    int code = static_cast<int>(std::lround(pq * 4095.0));
    if (code < 0) code = 0;
    if (code > 4095) code = 4095;
    return code;
}

// ===========================================================================
// XML 生成
// ===========================================================================
namespace {

// double を XML 属性向けに固定小数表現へ。整数なら .0 を残す。
QString fmtNum(double v)
{
    return QString::number(v, 'f', 6);
}

QString escapeXml(QString text)
{
    text.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    text.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    text.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    text.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return text;
}

// startSec を frameRate でフレーム番号へ換算 (四捨五入)。
qint64 secToFrame(double sec, double frameRate)
{
    if (frameRate <= 0.0) frameRate = 24.0;
    double f = sec * frameRate;
    if (f < 0.0) f = 0.0;
    return static_cast<qint64>(std::llround(f));
}

} // namespace

QString toDolbyVisionXml(const DolbyVisionMetadata& meta, double frameRate)
{
    if (frameRate <= 0.0) frameRate = 24.0;

    QString xml;
    xml.reserve(1024 + meta.shots.size() * 512);

    xml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml += QStringLiteral(
        "<DolbyLabsMDF version=\"4.0.2\" "
        "xmlns=\"http://www.dolby.com/schemas/dvmd/4_0_2\">\n");
    xml += QStringLiteral("  <Outputs>\n");
    xml += QStringLiteral("    <Output>\n");
    xml += QStringLiteral("      <Title>%1</Title>\n")
               .arg(escapeXml(meta.title));
    xml += QStringLiteral("      <Profile>%1</Profile>\n").arg(meta.profile);
    xml += QStringLiteral("      <Level>%1</Level>\n").arg(meta.level);
    xml += QStringLiteral("      <FrameRate>%1</FrameRate>\n").arg(fmtNum(frameRate));
    xml += QStringLiteral("      <Track>\n");
    xml += QStringLiteral("        <Shots>\n");

    for (const DvShot& shot : meta.shots) {
        const qint64 inFrame = secToFrame(shot.startSec, frameRate);
        qint64 outFrame = secToFrame(shot.endSec, frameRate);
        if (outFrame < inFrame) outFrame = inFrame;

        xml += QStringLiteral("          <Shot>\n");
        xml += QStringLiteral("            <Record inFrame=\"%1\" outFrame=\"%2\"/>\n")
                   .arg(inFrame).arg(outFrame);
        xml += QStringLiteral("            <PluginNode>\n");
        xml += QStringLiteral("              <DVDynamicData>\n");

        // Level1 (PQ 12bit 整数)
        xml += QStringLiteral(
                   "                <Level1 level=\"1\" minPQ=\"%1\" maxPQ=\"%2\" midPQ=\"%3\"/>\n")
                   .arg(nitsToPq12bit(shot.l1.minNits))
                   .arg(nitsToPq12bit(shot.l1.maxNits))
                   .arg(nitsToPq12bit(shot.l1.avgNits));

        // Level2 (ターゲット別 trim, 0 個以上)
        for (const L2Trim& t : shot.trims) {
            xml += QStringLiteral(
                "                <Level2 level=\"2\" targetNits=\"%1\" slope=\"%2\" "
                "offset=\"%3\" power=\"%4\" chromaWeight=\"%5\" "
                "saturationGain=\"%6\" toneDetail=\"%7\"/>\n")
                .arg(t.targetNits)
                .arg(fmtNum(t.slope))
                .arg(fmtNum(t.offset))
                .arg(fmtNum(t.power))
                .arg(fmtNum(t.chromaWeight))
                .arg(fmtNum(t.saturationGain))
                .arg(fmtNum(t.toneDetail));
        }

        // Level5 (アクティブエリア)
        xml += QStringLiteral(
                   "                <Level5 level=\"5\" activeAreaLeftOffset=\"%1\" "
                   "activeAreaRightOffset=\"%2\" activeAreaTopOffset=\"%3\" "
                   "activeAreaBottomOffset=\"%4\"/>\n")
                   .arg(shot.l5.left).arg(shot.l5.right)
                   .arg(shot.l5.top).arg(shot.l5.bottom);

        xml += QStringLiteral("              </DVDynamicData>\n");
        xml += QStringLiteral("            </PluginNode>\n");
        xml += QStringLiteral("          </Shot>\n");
    }

    xml += QStringLiteral("        </Shots>\n");
    xml += QStringLiteral("      </Track>\n");

    // Level6 (静的: CLL/FALL + マスタリングディスプレイ)
    xml += QStringLiteral("      <Level6 level=\"6\">\n");
    xml += QStringLiteral("        <MaxCLL>%1</MaxCLL>\n").arg(meta.l6.maxCll);
    xml += QStringLiteral("        <MaxFALL>%1</MaxFALL>\n").arg(meta.l6.maxFall);
    xml += QStringLiteral(
               "        <MasteringDisplay MaxNits=\"%1\" MinNits=\"%2\"/>\n")
               .arg(meta.l6.masteringMaxNits).arg(meta.l6.masteringMinNits);
    xml += QStringLiteral("      </Level6>\n");

    xml += QStringLiteral("    </Output>\n");
    xml += QStringLiteral("  </Outputs>\n");
    xml += QStringLiteral("</DolbyLabsMDF>\n");

    return xml;
}

// ===========================================================================
// 検証
// ===========================================================================
bool validate(const DolbyVisionMetadata& meta, QString* errorOut)
{
    auto fail = [&](const QString& msg) {
        if (errorOut) *errorOut = msg;
        return false;
    };

    if (meta.profile != 5 && meta.profile != 81) {
        return fail(QStringLiteral("Only profiles 5 and 81 are supported (current: %1)")
                        .arg(meta.profile));
    }
    if (meta.level < 1) {
        return fail(QStringLiteral("Level must be 1 or higher (current: %1)")
                        .arg(meta.level));
    }
    if (meta.l6.masteringMaxNits < meta.l6.masteringMinNits) {
        return fail(QStringLiteral(
                        "L6 mastering display max luminance is below the min luminance"));
    }
    if (meta.l6.maxCll < 0 || meta.l6.maxCll > 10000) {
        return fail(QStringLiteral("L6 MaxCLL must be within 0..10000 nits"));
    }
    if (meta.l6.maxFall < 0) {
        return fail(QStringLiteral("L6 MaxFALL must be 0 or higher"));
    }
    if (meta.l6.maxFall > meta.l6.maxCll) {
        return fail(QStringLiteral("L6 MaxFALL must be at or below MaxCLL"));
    }
    if (meta.l6.masteringMinNits < 0 || meta.l6.masteringMaxNits < 0
        || meta.l6.masteringMaxNits > 10000) {
        return fail(QStringLiteral(
                        "L6 mastering display luminance must be within 0..10000 nits"));
    }

    for (int i = 0; i < meta.shots.size(); ++i) {
        const DvShot& s = meta.shots.at(i);
        if (!std::isfinite(s.startSec) || !std::isfinite(s.endSec)) {
            return fail(QStringLiteral("Shot %1: time is not a finite value").arg(i));
        }
        if (s.endSec < s.startSec) {
            return fail(QStringLiteral("Shot %1: endSec is before startSec")
                            .arg(i));
        }
        if (s.startSec < 0.0 || s.endSec < 0.0) {
            return fail(QStringLiteral("Shot %1: time is negative").arg(i));
        }
        if (!std::isfinite(s.l1.minNits) || !std::isfinite(s.l1.avgNits)
            || !std::isfinite(s.l1.maxNits)) {
            return fail(QStringLiteral("Shot %1: L1 luminance is not a finite value").arg(i));
        }
        if (s.l1.minNits < 0.0 || s.l1.avgNits < 0.0 || s.l1.maxNits < 0.0) {
            return fail(QStringLiteral("Shot %1: L1 luminance is negative").arg(i));
        }
        if (s.l1.maxNits > kPqPeakNits) {
            return fail(QStringLiteral("Shot %1: L1 max luminance exceeds 10000 nits").arg(i));
        }
        if (!(s.l1.maxNits > s.l1.minNits)) {
            return fail(QStringLiteral("Shot %1: L1 max luminance must be greater than min luminance").arg(i));
        }
        if (!(s.l1.minNits <= s.l1.avgNits && s.l1.avgNits <= s.l1.maxNits)) {
            return fail(QStringLiteral(
                            "Shot %1: L1 luminance does not satisfy min<=avg<=max")
                            .arg(i));
        }
        for (int j = 0; j < s.trims.size(); ++j) {
            const L2Trim& t = s.trims.at(j);
            if (t.targetNits <= 0 || t.targetNits > 10000) {
                return fail(QStringLiteral(
                                "Shot %1 trim %2: targetNits must be within 1..10000")
                                .arg(i).arg(j));
            }
            const double values[] = {
                t.slope, t.offset, t.power, t.chromaWeight,
                t.saturationGain, t.toneDetail
            };
            for (double v : values) {
                if (!std::isfinite(v) || v < -1.0 || v > 1.0) {
                    return fail(QStringLiteral(
                                    "Shot %1 trim %2: L2 trim value must be within -1..1")
                                    .arg(i).arg(j));
                }
            }
        }
        if (s.l5.left < 0 || s.l5.right < 0 || s.l5.top < 0 || s.l5.bottom < 0) {
            return fail(QStringLiteral("Shot %1: L5 active area offset is negative").arg(i));
        }
    }

    if (errorOut) errorOut->clear();
    return true;
}

// ===========================================================================
// JSON 永続化
// ===========================================================================
namespace {

QJsonObject l1ToJson(const L1Metadata& l1)
{
    QJsonObject o;
    o["minNits"] = l1.minNits;
    o["avgNits"] = l1.avgNits;
    o["maxNits"] = l1.maxNits;
    return o;
}

L1Metadata l1FromJson(const QJsonObject& o)
{
    L1Metadata l1;
    l1.minNits = o.value("minNits").toDouble(0.0);
    l1.avgNits = o.value("avgNits").toDouble(0.0);
    l1.maxNits = o.value("maxNits").toDouble(0.0);
    return l1;
}

QJsonObject l2ToJson(const L2Trim& t)
{
    QJsonObject o;
    o["targetNits"] = t.targetNits;
    o["slope"] = t.slope;
    o["offset"] = t.offset;
    o["power"] = t.power;
    o["chromaWeight"] = t.chromaWeight;
    o["saturationGain"] = t.saturationGain;
    o["toneDetail"] = t.toneDetail;
    return o;
}

L2Trim l2FromJson(const QJsonObject& o)
{
    L2Trim t;
    t.targetNits = o.value("targetNits").toInt(100);
    t.slope = o.value("slope").toDouble(0.0);
    t.offset = o.value("offset").toDouble(0.0);
    t.power = o.value("power").toDouble(0.0);
    t.chromaWeight = o.value("chromaWeight").toDouble(0.0);
    t.saturationGain = o.value("saturationGain").toDouble(0.0);
    t.toneDetail = o.value("toneDetail").toDouble(0.0);
    return t;
}

QJsonObject l5ToJson(const L5ActiveArea& a)
{
    QJsonObject o;
    o["left"] = a.left;
    o["right"] = a.right;
    o["top"] = a.top;
    o["bottom"] = a.bottom;
    return o;
}

L5ActiveArea l5FromJson(const QJsonObject& o)
{
    L5ActiveArea a;
    a.left = o.value("left").toInt(0);
    a.right = o.value("right").toInt(0);
    a.top = o.value("top").toInt(0);
    a.bottom = o.value("bottom").toInt(0);
    return a;
}

QJsonObject l6ToJson(const L6Metadata& l6)
{
    QJsonObject o;
    o["maxCll"] = l6.maxCll;
    o["maxFall"] = l6.maxFall;
    o["masteringMaxNits"] = l6.masteringMaxNits;
    o["masteringMinNits"] = l6.masteringMinNits;
    return o;
}

L6Metadata l6FromJson(const QJsonObject& o)
{
    L6Metadata l6;
    l6.maxCll = o.value("maxCll").toInt(0);
    l6.maxFall = o.value("maxFall").toInt(0);
    l6.masteringMaxNits = o.value("masteringMaxNits").toInt(1000);
    l6.masteringMinNits = o.value("masteringMinNits").toInt(0);
    return l6;
}

QJsonObject shotToJson(const DvShot& s)
{
    QJsonObject o;
    o["startSec"] = s.startSec;
    o["endSec"] = s.endSec;
    o["l1"] = l1ToJson(s.l1);
    o["l5"] = l5ToJson(s.l5);
    QJsonArray trims;
    for (const L2Trim& t : s.trims) trims.append(l2ToJson(t));
    o["trims"] = trims;
    return o;
}

DvShot shotFromJson(const QJsonObject& o)
{
    DvShot s;
    s.startSec = o.value("startSec").toDouble(0.0);
    s.endSec = o.value("endSec").toDouble(0.0);
    s.l1 = l1FromJson(o.value("l1").toObject());
    s.l5 = l5FromJson(o.value("l5").toObject());
    const QJsonArray trims = o.value("trims").toArray();
    for (const QJsonValue& v : trims) s.trims.append(l2FromJson(v.toObject()));
    return s;
}

} // namespace

QJsonObject toJson(const DolbyVisionMetadata& meta)
{
    QJsonObject o;
    o["profile"] = meta.profile;
    o["level"] = meta.level;
    o["title"] = meta.title;
    o["l6"] = l6ToJson(meta.l6);
    QJsonArray shots;
    for (const DvShot& s : meta.shots) shots.append(shotToJson(s));
    o["shots"] = shots;
    return o;
}

DolbyVisionMetadata fromJson(const QJsonObject& obj)
{
    DolbyVisionMetadata meta;
    meta.profile = obj.value("profile").toInt(81);
    meta.level = obj.value("level").toInt(6);
    meta.title = obj.value("title").toString();
    meta.l6 = l6FromJson(obj.value("l6").toObject());
    const QJsonArray shots = obj.value("shots").toArray();
    for (const QJsonValue& v : shots) meta.shots.append(shotFromJson(v.toObject()));
    return meta;
}

} // namespace dolbyvision
