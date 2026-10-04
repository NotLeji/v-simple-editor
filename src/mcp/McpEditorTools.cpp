#include "McpEditorTools.h"

#include "McpToolRegistry.h"
#include "../CaptionEditorDialog.h"
#include "../BeatDetect.h"
#include "../DynamicZoom.h"
#include "../DialogueLeveler.h"
#include "../MainWindow.h"
#include "../MusicRemix.h"
#include "../ProjectDiff.h"
#include "../ProjectFile.h"
#include "../RenderQueue.h"
#include "../RenderInPlace.h"
#include "../TimelineFrameRenderer.h"
#include "../Timeline.h"
#include "../TrimOps.h"
#include "../TrackMatteKey.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include "../WaveformGenerator.h"
#include "../TimecodeBurnIn.h"

#include <QAction>
#include <QBuffer>
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QPointer>
#include <QPair>
#include <QTimer>
#include <QUuid>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

// Exporter.cpp: GUI の exportVideo と同じラウドネス正規化ゲインを export_video にも適用する。
double exporter_loudnessGainDb();

namespace mcp {

namespace {

QJsonObject objectSchema(const QJsonObject& properties = {})
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("additionalProperties"), false}
    };
}

// QJsonObject には QHash のような unite() が無いので、共通のクリップ指定
// プロパティにツール固有のプロパティを重ねる小さなヘルパを用意する。
// 同じキーがあれば extra 側が勝つ。
QJsonObject mergedProperties(const QJsonObject& base, const QJsonObject& extra)
{
    QJsonObject merged = base;
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    return merged;
}

QJsonObject schemaWithRequired(const QJsonObject& properties,
                               const QStringList& required)
{
    QJsonObject schema = objectSchema(properties);
    QJsonArray requiredArray;
    for (const QString& name : required)
        requiredArray.append(name);
    if (!requiredArray.isEmpty())
        schema.insert(QStringLiteral("required"), requiredArray);
    return schema;
}

// additionalProperties は付けない (応答キーが条件で増減するツールがある)。
QJsonObject outputSchemaOf(const QJsonObject& properties,
                           const QStringList& required)
{
    QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties}
    };
    QJsonArray requiredArray;
    for (const QString& name : required)
        requiredArray.append(name);
    if (!requiredArray.isEmpty())
        schema.insert(QStringLiteral("required"), requiredArray);
    return schema;
}

QJsonObject timecodeBurnInSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("enabled"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("position"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("topLeft"), QStringLiteral("topCenter"),
                QStringLiteral("topRight"), QStringLiteral("bottomLeft"),
                QStringLiteral("bottomCenter"), QStringLiteral("bottomRight")
            }}
        }},
        {QStringLiteral("fontSizePct"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 1},
            {QStringLiteral("maximum"), 20}
        }},
        {QStringLiteral("showFrames"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("dropFrame"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("prefix"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("showClipName"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("opacity"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("minimum"), 0.0},
            {QStringLiteral("maximum"), 1.0}
        }}
    }, {QStringLiteral("enabled"), QStringLiteral("position"),
        QStringLiteral("fontSizePct"), QStringLiteral("showFrames"),
        QStringLiteral("dropFrame"), QStringLiteral("prefix"),
        QStringLiteral("showClipName"), QStringLiteral("opacity")});
}

QJsonObject timecodeBurnInInputSchema()
{
    QJsonObject schema = timecodeBurnInSchema();
    schema.remove(QStringLiteral("required"));
    schema.insert(QStringLiteral("additionalProperties"), false);
    return schema;
}

QJsonObject timecodeBurnInForMcp(const TimecodeBurnInSettings &settings)
{
    QJsonObject object = settings.toJson();
    // The MCP contract intentionally exposes the eight fields declared by
    // set_project_option; showDate remains available in project/UI JSON.
    object.remove(QStringLiteral("showDate"));
    return object;
}

const QStringList& transitionTypeNames()
{
    static const QStringList names{
        QStringLiteral("None"),
        QStringLiteral("FadeIn"),
        QStringLiteral("FadeOut"),
        QStringLiteral("CrossDissolve"),
        QStringLiteral("WipeLeft"),
        QStringLiteral("WipeRight"),
        QStringLiteral("WipeUp"),
        QStringLiteral("WipeDown"),
        QStringLiteral("SlideLeft"),
        QStringLiteral("SlideRight"),
        QStringLiteral("SlideUp"),
        QStringLiteral("SlideDown"),
        QStringLiteral("DipToBlack"),
        QStringLiteral("DipToWhite"),
        QStringLiteral("IrisRound"),
        QStringLiteral("IrisBox"),
        QStringLiteral("ClockWipe"),
        QStringLiteral("BarnDoorHorizontal"),
        QStringLiteral("BarnDoorVertical"),
        QStringLiteral("PushLeft"),
        QStringLiteral("PushRight"),
        QStringLiteral("PushUp"),
        QStringLiteral("PushDown"),
        QStringLiteral("CrossZoom"),
        QStringLiteral("FilmDissolve"),
        QStringLiteral("SpinCW"),
        QStringLiteral("SpinCCW"),
        QStringLiteral("DitherDissolve"),
        QStringLiteral("IrisRoundClose"),
        QStringLiteral("IrisBoxClose"),
        QStringLiteral("BarnDoorHClose"),
        QStringLiteral("BarnDoorVClose"),
        QStringLiteral("ClockWipeCCW"),
        QStringLiteral("WhipPanLeft"),
        QStringLiteral("WhipPanRight"),
        QStringLiteral("Glitch"),
        QStringLiteral("LightLeak"),
        QStringLiteral("FlipHorizontal"),
        QStringLiteral("FlipVertical"),
        QStringLiteral("LensFlare"),
        QStringLiteral("FilmBurn"),
        QStringLiteral("Pixelate"),
        QStringLiteral("BlurDissolve"),
        QStringLiteral("CameraShake"),
        QStringLiteral("ColorChannelShift"),
        QStringLiteral("MorphCut")
    };
    return names;
}

QJsonArray transitionTypeEnum()
{
    QJsonArray result;
    for (const QString& name : transitionTypeNames())
        result.append(name);
    return result;
}

bool transitionTypeFromName(const QString& name, TransitionType* out)
{
    const int index = transitionTypeNames().indexOf(name);
    if (index < 0)
        return false;
    if (out)
        *out = static_cast<TransitionType>(index);
    return true;
}

const QStringList& clipLabelIds()
{
    static const QStringList ids{
        QStringLiteral("none"), QStringLiteral("red"),
        QStringLiteral("orange"), QStringLiteral("yellow"),
        QStringLiteral("green"), QStringLiteral("cyan"),
        QStringLiteral("blue"), QStringLiteral("purple"),
        QStringLiteral("pink")
    };
    return ids;
}

QJsonArray clipLabelEnum()
{
    QJsonArray result;
    for (const QString& id : clipLabelIds())
        result.append(id);
    return result;
}

bool parseClipLabel(const QString& id, ClipLabel* out)
{
    if (!clipLabelIds().contains(id))
        return false;
    if (out)
        *out = clipLabelFromString(id);
    return true;
}

QJsonObject transitionToJson(const Transition& transition)
{
    return QJsonObject{
        {QStringLiteral("type"), transitionTypeNames().at(static_cast<int>(transition.type))},
        {QStringLiteral("durationSec"),
         transition.type == TransitionType::None ? 0.0 : transition.duration},
        {QStringLiteral("alignment"), transitionAlignmentNames().at(static_cast<int>(transition.alignment))},
        {QStringLiteral("easing"), transitionEasingNames().at(static_cast<int>(transition.easing))},
        {QStringLiteral("softness"), transition.softness},
        {QStringLiteral("borderWidth"), transition.borderWidth},
        {QStringLiteral("borderColor"), transition.borderColor.name()}
    };
}

QJsonObject transitionIdentifierSchema(const QStringList& names)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray::fromStringList(names)}
    };
}

QJsonObject transitionOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("type"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("alignment"), transitionIdentifierSchema(transitionAlignmentNames())},
        {QStringLiteral("easing"), transitionIdentifierSchema(transitionEasingNames())},
        {QStringLiteral("softness"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("borderWidth"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("borderColor"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("type"), QStringLiteral("durationSec")});
}

ToolDescriptor withOutputSchema(ToolDescriptor tool,
                                const QJsonObject& outputSchema)
{
    tool.outputSchema = outputSchema;
    return tool;
}

QJsonObject clipOutputItemSchema()
{
    const QJsonObject properties{
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("displayName"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("filePath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("inPointSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("outPointSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("speed"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("reversed"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("autoOrient"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("volume"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("opacity"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("label"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), clipLabelEnum()}
        }},
        {QStringLiteral("linkGroup"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("selected"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("leadIn"), transitionOutputItemSchema()},
        {QStringLiteral("trailOut"), transitionOutputItemSchema()},
        {QStringLiteral("overlap"), objectSchema(QJsonObject{
            {QStringLiteral("leadInSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("trailOutSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
        })},
        {QStringLiteral("textOverlayCount"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0}
        }}
    };
    return outputSchemaOf(properties, {
        QStringLiteral("index"), QStringLiteral("displayName"),
        QStringLiteral("filePath"), QStringLiteral("startSec"),
        QStringLiteral("durationSec"), QStringLiteral("inPointSec"),
        QStringLiteral("outPointSec"), QStringLiteral("speed"),
        QStringLiteral("reversed"), QStringLiteral("autoOrient"),
        QStringLiteral("volume"),
        QStringLiteral("opacity"), QStringLiteral("label"),
        QStringLiteral("linkGroup"), QStringLiteral("selected"),
        QStringLiteral("leadIn"), QStringLiteral("trailOut"),
        QStringLiteral("textOverlayCount")
    });
}

QJsonObject trackOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("locked"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("clips"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), clipOutputItemSchema()}
        }}
    }, {QStringLiteral("index"), QStringLiteral("locked"), QStringLiteral("clips")});
}

QJsonObject captionOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("endSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("index"), QStringLiteral("startSec"),
        QStringLiteral("endSec"), QStringLiteral("text")});
}

QJsonObject commandOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("label"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("menuPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("enabled"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("id"), QStringLiteral("label"),
        QStringLiteral("menuPath"), QStringLiteral("risk"),
        QStringLiteral("enabled")});
}

QJsonObject importedClipOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("kind"), QStringLiteral("trackIndex"),
        QStringLiteral("clipIndex"), QStringLiteral("startSec"),
        QStringLiteral("durationSec")});
}

QJsonObject clipSelectorProperties()
{
    return QJsonObject{
        {QStringLiteral("kind"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("video"), QStringLiteral("audio")
            }},
            {QStringLiteral("default"), QStringLiteral("video")},
            {QStringLiteral("description"),
             QStringLiteral("video or audio. Defaults to video")}
        }},
        {QStringLiteral("trackIndex"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0},
            {QStringLiteral("default"), 0},
            {QStringLiteral("description"),
             QStringLiteral("0-based track number. Defaults to 0")}
        }},
        {QStringLiteral("clipIndex"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0},
            {QStringLiteral("description"),
             QStringLiteral("0-based clip number within the track. Corresponds to get_timeline index. Required")}
        }}
    };
}

QJsonObject trackSelectorProperties()
{
    return QJsonObject{
        {QStringLiteral("kind"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("video"), QStringLiteral("audio")
            }},
            {QStringLiteral("description"),
             QStringLiteral("video or audio")}
        }},
        {QStringLiteral("trackIndex"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0},
            {QStringLiteral("description"),
             QStringLiteral("0-based track number")}
        }}
    };
}

bool setError(QString* err, const QString& message)
{
    if (err)
        *err = message;
    return false;
}

bool rejectUnknownArguments(const QJsonObject& args, const QStringList& allowed,
                            QString* err)
{
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        if (!allowed.contains(it.key()))
            return setError(err, QStringLiteral("unknown argument: %1").arg(it.key()));
    }
    return true;
}

bool requiredFiniteNumber(const QJsonObject& args, const QString& name,
                          double* out, QString* err)
{
    const QJsonValue value = args.value(name);
    if (!value.isDouble())
        return setError(err, QStringLiteral("%1 must be a finite number").arg(name));
    const double number = value.toDouble();
    if (!std::isfinite(number))
        return setError(err, QStringLiteral("%1 must be a finite number").arg(name));
    if (out)
        *out = number;
    return true;
}

bool finiteNumberForMcp(const QJsonObject& args, const QString& name,
                        double* out, QString* err)
{
    const QJsonValue value = args.value(name);
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return setError(err, QStringLiteral("Please specify %1 as a finite number").arg(name));
    if (out)
        *out = value.toDouble();
    return true;
}

bool nonNegativeInteger(const QJsonObject& args, const QString& name,
                        int defaultValue, int* out, QString* err)
{
    if (!args.contains(name)) {
        if (out)
            *out = defaultValue;
        return true;
    }

    const QJsonValue value = args.value(name);
    if (!value.isDouble())
        return setError(err, QStringLiteral("%1 must be a non-negative integer").arg(name));
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0
        || std::floor(number) != number
        || number > static_cast<double>(std::numeric_limits<int>::max())) {
        return setError(err, QStringLiteral("%1 must be a non-negative integer").arg(name));
    }
    if (out)
        *out = static_cast<int>(number);
    return true;
}

bool positiveInteger(const QJsonObject& args, const QString& name,
                     int defaultValue, int* out, QString* err)
{
    if (!args.contains(name)) {
        if (out)
            *out = defaultValue;
        return true;
    }

    const QJsonValue value = args.value(name);
    if (!value.isDouble())
        return setError(err, QStringLiteral("Specify %1 as a positive integer").arg(name));
    const double number = value.toDouble();
    if (!std::isfinite(number) || number <= 0.0
        || std::floor(number) != number
        || number > static_cast<double>(std::numeric_limits<int>::max())) {
        return setError(err, QStringLiteral("Specify %1 as a positive integer").arg(name));
    }
    if (out)
        *out = static_cast<int>(number);
    return true;
}

bool positiveFiniteNumber(const QJsonObject& args, const QString& name,
                          double defaultValue, double* out, QString* err)
{
    if (!args.contains(name)) {
        if (out)
            *out = defaultValue;
        return true;
    }
    double number = 0.0;
    if (!finiteNumberForMcp(args, name, &number, err))
        return false;
    if (number <= 0.0)
        return setError(err, QStringLiteral("Please specify %1 as a number greater than 0").arg(name));
    if (out)
        *out = number;
    return true;
}

bool encodePng(const QImage& image, QByteArray* encoded)
{
    if (!encoded || image.isNull())
        return false;
    encoded->clear();
    QBuffer buffer(encoded);
    if (!buffer.open(QIODevice::WriteOnly))
        return false;
    return image.save(&buffer, "PNG");
}

constexpr int kDefaultFrameMaxWidth = 640;
constexpr qsizetype kMaxFrameResponseBytes = 1024 * 1024;
// JSON のキー・MCP envelope 分を予約し、base64 本体をこの上限内に収める。
constexpr qsizetype kFrameResponseOverhead = 4096;

struct ClipTarget {
    TimelineTrack* track = nullptr;
    // 解決済みの宛先。Timeline の *ByIndex API にそのまま渡すためのもので、
    // 呼び出し側が args を読み直さないようにする (既定値の解釈が二箇所に散らない)。
    bool audio = false;
    int trackIndex = 0;
    int clipIndex = -1;
    double startSec = 0.0;
    double endSec = 0.0;
};

bool readClipTarget(const QJsonObject& args, MainWindow* window,
                    Timeline* currentTimeline, ClipTarget* out, QString* err)
{
    QString kind = QStringLiteral("video");
    if (args.contains(QStringLiteral("kind"))) {
        const QJsonValue value = args.value(QStringLiteral("kind"));
        if (!value.isString())
            return setError(err, QStringLiteral("kind must be video or audio"));
        kind = value.toString();
    }
    if (kind != QStringLiteral("video") && kind != QStringLiteral("audio"))
        return setError(err, QStringLiteral("kind must be video or audio"));

    int trackIndex = 0;
    if (!nonNegativeInteger(args, QStringLiteral("trackIndex"), 0,
                            &trackIndex, err)) {
        return false;
    }
    if (out) {
        out->audio = kind == QStringLiteral("audio");
        out->trackIndex = trackIndex;
    }

    int clipIndex = -1;
    if (!args.contains(QStringLiteral("clipIndex")))
        return setError(err, QStringLiteral("clipIndex is required"));
    if (!nonNegativeInteger(args, QStringLiteral("clipIndex"), -1,
                            &clipIndex, err)) {
        return false;
    }

    if (!window || !currentTimeline)
        return setError(err, QStringLiteral("editor not available"));

    TimelineTrack* track = currentTimeline->trackAt(
        kind == QStringLiteral("audio"), trackIndex);
    if (!track) {
        const int trackCount = kind == QStringLiteral("audio")
            ? currentTimeline->audioTracks().size()
            : currentTimeline->videoTracks().size();
        return setError(err, QStringLiteral("track index is out of range (%1: %2 tracks, 0..%3)")
                                 .arg(kind).arg(trackCount).arg(trackCount - 1));
    }
    if (clipIndex >= track->clipCount()) {
        return setError(err, QStringLiteral("clip index is out of range (%1 track %2 has %3 clips: 0..%4)")
                                 .arg(kind).arg(trackIndex).arg(track->clipCount())
                                 .arg(track->clipCount() - 1));
    }

    double cursor = 0.0;
    const QVector<ClipInfo>& clips = track->clips();
    for (int index = 0; index < clips.size(); ++index) {
        const ClipInfo& clip = clips.at(index);
        const double start = cursor + clip.leadInSec;
        const double end = start + clip.effectiveDuration();
        if (index == clipIndex) {
            if (!std::isfinite(start) || !std::isfinite(end) || end <= start)
                return setError(err, QStringLiteral("clip has no valid duration"));
            if (out) {
                // audio / trackIndex は上で解決済みなので、位置指定の集成初期化で
                // 丸ごと上書きしない (フィールドを足したときに黙って壊れる)。
                out->track = track;
                out->clipIndex = clipIndex;
                out->startSec = start;
                out->endSec = end;
            }
            return true;
        }
        cursor = end;
    }

    return setError(err, QStringLiteral("clip index is out of range"));
}

bool detectMusicRemixBeats(const ClipInfo &clip, QVector<double> *beatTimes,
                           double *bpm, QString *err)
{
    if (beatTimes)
        beatTimes->clear();
    if (bpm)
        *bpm = 0.0;

    QVector<float> samples;
    int sampleRate = 0;
    if (!WaveformGenerator::decodeAudio(clip.filePath, samples, sampleRate)
        || samples.isEmpty() || sampleRate <= 0) {
        return setError(err, QStringLiteral("Failed to decode audio."));
    }
    const double sourceOut = clip.outPoint > 0.0 ? clip.outPoint : clip.duration;
    const double totalSourceSec = static_cast<double>(samples.size()) / sampleRate;
    const double activeStart = qMax(0.0, clip.inPoint);
    const double activeEnd = qMin(sourceOut, totalSourceSec);
    if (activeEnd <= activeStart)
        return setError(err, QStringLiteral("Clip has no valid audio range."));

    const int sampleCount = static_cast<int>(samples.size());
    const int startSample = qBound(
        0, static_cast<int>(std::floor(activeStart * sampleRate)), sampleCount);
    const int endSample = qBound(
        startSample, static_cast<int>(std::ceil(activeEnd * sampleRate)), sampleCount);
    const beatdetect::Result detected = beatdetect::detectBeats(
        samples.mid(startSample, endSample - startSample),
        sampleRate, beatdetect::Config{});
    if (detected.beatTimesSec.size() < 2)
        return setError(err, QStringLiteral("Cannot apply: fewer than 2 beats."));

    const double speed = clip.speed > 0.0 ? clip.speed : 1.0;
    const double clipDuration = clip.effectiveDuration();
    if (!std::isfinite(clipDuration) || clipDuration <= 0.0)
        return setError(err, QStringLiteral("Target clip has an invalid duration"));
    QVector<double> localBeats;
    localBeats.reserve(detected.beatTimesSec.size());
    for (double beat : detected.beatTimesSec) {
        const double sourceLocal = activeStart + beat - clip.inPoint;
        if (sourceLocal >= -1.0e-6
            && sourceLocal <= clipDuration * speed + 1.0e-6) {
            localBeats.append(qBound(0.0, sourceLocal / speed, clipDuration));
        }
    }
    if (localBeats.size() < 2)
        return setError(err, QStringLiteral("Could not create beat boundaries."));
    if (beatTimes)
        *beatTimes = localBeats;
    if (bpm)
        *bpm = detected.bpm;
    return true;
}

bool analyzeDialogueClip(const ClipInfo &clip, const leveler::Config &config,
                         leveler::Analysis *analysis, QString *err)
{
    if (analysis)
        *analysis = {};
    QVector<float> samples;
    int sampleRate = 0;
    if (!WaveformGenerator::decodeAudio(clip.filePath, samples, sampleRate)
        || samples.isEmpty() || sampleRate <= 0) {
        return setError(err, QStringLiteral("Failed to decode audio."));
    }

    const double sourceOut = clip.outPoint > 0.0 ? clip.outPoint : clip.duration;
    const double sourceDuration = static_cast<double>(samples.size()) / sampleRate;
    const double activeStart = qBound(0.0, clip.inPoint, sourceDuration);
    const double activeEnd = qBound(activeStart, sourceOut, sourceDuration);
    const int sampleCount = static_cast<int>(samples.size());
    const int firstSample = qBound(
        0, static_cast<int>(std::floor(activeStart * sampleRate)), sampleCount);
    const int lastSample = qBound(
        firstSample, static_cast<int>(std::ceil(activeEnd * sampleRate)),
        sampleCount);
    if (lastSample <= firstSample)
        return setError(err, QStringLiteral("Clip has no valid audio range."));

    QVector<float> activeSamples =
        samples.mid(firstSample, lastSample - firstSample);
    if (clip.reversed)
        std::reverse(activeSamples.begin(), activeSamples.end());
    leveler::Analysis computed = leveler::analyze(
        activeSamples, sampleRate, config);
    if (computed.envelope.isEmpty())
        return setError(err, QStringLiteral("Could not generate volume envelope."));

    const double speed = clip.speed > 0.0 ? clip.speed : 1.0;
    const double clipDuration = clip.effectiveDuration();
    for (AudioGainPoint &point : computed.envelope)
        point.time = qBound(0.0, point.time / speed, clipDuration);
    if (analysis)
        *analysis = computed;
    return true;
}

QJsonObject dynamicZoomRectSchema()
{
    QJsonObject schema = schemaWithRequired(QJsonObject{
        {QStringLiteral("cx"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("minimum"), 0.0},
            {QStringLiteral("maximum"), 1.0}
        }},
        {QStringLiteral("cy"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("minimum"), 0.0},
            {QStringLiteral("maximum"), 1.0}
        }},
        {QStringLiteral("w"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("exclusiveMinimum"), 0.0},
            {QStringLiteral("maximum"), 1.0}
        }},
        {QStringLiteral("h"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("description"),
             QStringLiteral("Compatibility input. The value is ignored; the visible area is fixed to the canvas aspect ratio")}
        }}
    }, {QStringLiteral("cx"), QStringLiteral("cy"),
        QStringLiteral("w")});
    schema.insert(
        QStringLiteral("description"),
        QStringLiteral("Normalized frame. h is optional and ignored if specified; fixed to the canvas aspect ratio"));
    return schema;
}

bool readDynamicZoomRect(const QJsonObject& args, const QString& name,
                         dynzoom::Rect *out, QString* err)
{
    const QJsonValue value = args.value(name);
    if (!value.isObject())
        return setError(err, QStringLiteral("%1 must be an object").arg(name));
    const QJsonObject object = value.toObject();
    if (!rejectUnknownArguments(
            object,
            {QStringLiteral("cx"), QStringLiteral("cy"),
             QStringLiteral("w"), QStringLiteral("h")}, err)) {
        return false;
    }

    dynzoom::Rect rect;
    if (!requiredFiniteNumber(object, QStringLiteral("cx"), &rect.cx, err)
        || !requiredFiniteNumber(object, QStringLiteral("cy"), &rect.cy, err)
        || !requiredFiniteNumber(object, QStringLiteral("w"), &rect.w, err)) {
        return false;
    }
    if (rect.cx < 0.0 || rect.cx > 1.0
        || rect.cy < 0.0 || rect.cy > 1.0) {
        return setError(err, QStringLiteral("%1 cx/cy must be in range [0, 1]")
                                 .arg(name));
    }
    if (rect.w <= 0.0 || rect.w > 1.0) {
        return setError(err, QStringLiteral("%1 w must be in range (0, 1]")
                                 .arg(name));
    }
    if (out)
        *out = rect;
    return true;
}

bool dynamicZoomPresetFrames(const QString& preset, dynzoom::Rect *start,
                             dynzoom::Rect *end)
{
    dynzoom::Rect first = dynzoom::presetRect(dynzoom::Preset::Full);
    dynzoom::Rect last = first;
    if (preset == QStringLiteral("zoomIn")) {
        last = dynzoom::presetRect(dynzoom::Preset::ZoomIn);
    } else if (preset == QStringLiteral("zoomOut")) {
        first = dynzoom::presetRect(dynzoom::Preset::ZoomOut);
    } else if (preset == QStringLiteral("panLeft")) {
        first = dynzoom::presetRect(dynzoom::Preset::PanRight);
        last = dynzoom::presetRect(dynzoom::Preset::PanLeft);
    } else if (preset == QStringLiteral("panRight")) {
        first = dynzoom::presetRect(dynzoom::Preset::PanLeft);
        last = dynzoom::presetRect(dynzoom::Preset::PanRight);
    } else if (preset == QStringLiteral("panUp")) {
        first = dynzoom::presetRect(dynzoom::Preset::PanDown);
        last = dynzoom::presetRect(dynzoom::Preset::PanUp);
    } else if (preset == QStringLiteral("panDown")) {
        first = dynzoom::presetRect(dynzoom::Preset::PanUp);
        last = dynzoom::presetRect(dynzoom::Preset::PanDown);
    } else {
        return false;
    }
    if (start)
        *start = first;
    if (end)
        *end = last;
    return true;
}

bool requiredString(const QJsonObject& args, const QString& name,
                    QString* out, QString* err)
{
    const QJsonValue value = args.value(name);
    if (!value.isString())
        return setError(err, QStringLiteral("%1 is required").arg(name));
    if (out)
        *out = value.toString();
    return true;
}

struct TrackTarget {
    TimelineTrack* track = nullptr;
    TrackKind kind = TrackKind::Video;
    QString kindName;
    int trackIndex = -1;
};

bool readTrackTarget(const QJsonObject& args, MainWindow* window,
                     Timeline* currentTimeline, TrackTarget* out, QString* err)
{
    QString kind;
    if (!requiredString(args, QStringLiteral("kind"), &kind, err))
        return false;
    if (kind != QStringLiteral("video") && kind != QStringLiteral("audio"))
        return setError(err, QStringLiteral("kind must be video or audio"));
    if (!args.contains(QStringLiteral("trackIndex")))
        return setError(err, QStringLiteral("trackIndex is required"));
    int trackIndex = -1;
    if (!nonNegativeInteger(args, QStringLiteral("trackIndex"), -1,
                            &trackIndex, err)) {
        return false;
    }
    if (!window || !currentTimeline)
        return setError(err, QStringLiteral("editor not available"));

    const TrackKind trackKind = kind == QStringLiteral("audio")
        ? TrackKind::Audio : TrackKind::Video;
    TimelineTrack* track = currentTimeline->trackAt(trackKind == TrackKind::Audio,
                                                    trackIndex);
    if (!track) {
        const int trackCount = trackKind == TrackKind::Audio
            ? currentTimeline->audioTrackCount()
            : currentTimeline->videoTrackCount();
        return setError(err, QStringLiteral("track index is out of range (%1: %2 tracks, 0..%3)")
                                 .arg(kind).arg(trackCount).arg(trackCount - 1));
    }
    if (out) {
        out->track = track;
        out->kind = trackKind;
        out->kindName = kind;
        out->trackIndex = trackIndex;
    }
    return true;
}

QString actionRiskToString(FavoritableActionRisk risk)
{
    switch (risk) {
    case FavoritableActionRisk::Safe:
        return QStringLiteral("safe");
    case FavoritableActionRisk::Blocking:
        return QStringLiteral("blocking");
    case FavoritableActionRisk::Quit:
        return QStringLiteral("quit");
    }
    return QStringLiteral("safe");
}

struct ClipOverlap {
    double leadInSec = 0.0;
    double trailOutSec = 0.0;
};

QJsonObject clipToJson(const ClipInfo& clip, int clipIndex, double startSec,
                       bool selected, const ClipOverlap& overlap)
{
    const double outPoint = clip.outPoint > 0.0 ? clip.outPoint : clip.duration;
    const double durationSec = clip.speed > 0.0 ? clip.effectiveDuration() : 0.0;
    QJsonArray effects;
    for (const auto &effect : clip.effects) {
        effects.append(QJsonObject{{QStringLiteral("type"), static_cast<int>(effect.type)},
            {QStringLiteral("enabled"), effect.enabled}, {QStringLiteral("param1"), effect.param1},
            {QStringLiteral("param2"), effect.param2}, {QStringLiteral("param3"), effect.param3},
            {QStringLiteral("keyColor"), effect.keyColor.name()},
            {QStringLiteral("startSec"), effect.startSec}, {QStringLiteral("endSec"), effect.endSec}});
    }
    return QJsonObject{
        {QStringLiteral("effects"), effects},
        {QStringLiteral("index"), clipIndex},
        {QStringLiteral("displayName"), clip.displayName},
        {QStringLiteral("filePath"), clip.filePath},
        {QStringLiteral("startSec"), startSec},
        {QStringLiteral("durationSec"), durationSec},
        {QStringLiteral("inPointSec"), clip.inPoint},
        {QStringLiteral("outPointSec"), outPoint},
        {QStringLiteral("speed"), clip.speed},
        {QStringLiteral("reversed"), clip.reversed},
        {QStringLiteral("autoOrient"), clip.autoOrientEnabled},
        {QStringLiteral("volume"), clip.volume},
        {QStringLiteral("opacity"), clip.opacity},
        {QStringLiteral("label"), clipLabelToString(clip.label)},
        {QStringLiteral("linkGroup"), clip.linkGroup},
        {QStringLiteral("selected"), selected},
        {QStringLiteral("leadIn"), transitionToJson(clip.leadIn)},
        {QStringLiteral("trailOut"), transitionToJson(clip.trailOut)},
        {QStringLiteral("overlap"), QJsonObject{
            {QStringLiteral("leadInSec"), overlap.leadInSec},
            {QStringLiteral("trailOutSec"), overlap.trailOutSec}
        }},
        {QStringLiteral("textOverlayCount"), clip.textManager.count()}
    };
}

QJsonArray tracksToJson(const Timeline* timeline, TrackKind kind)
{
    const auto& tracks = kind == TrackKind::Video
        ? timeline->videoTracks() : timeline->audioTracks();
    // Use resolved playback bounds, including handle shortages and alignment.
    // Audio can emit several entries per clip (time remapping / sequences).
    QVector<QVector<OverlapInterval>> intervals;
    if (kind == TrackKind::Video) {
        intervals = timeline->videoOverlapIntervals();
    } else {
        intervals.resize(tracks.size());
        for (const PlaybackEntry& entry : timeline->computeAudioPlaybackSequence()) {
            if (entry.sourceTrack < 0 || entry.sourceTrack >= intervals.size())
                continue;
            OverlapInterval interval;
            interval.clipIdx = entry.sourceClipIndex;
            interval.timelineStart = entry.timelineStart;
            interval.timelineEnd = entry.timelineEnd;
            intervals[entry.sourceTrack].append(interval);
        }
    }
    QJsonArray result;
    for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        QJsonArray clips;
        double cursorSec = 0.0;
        const TimelineTrack *trackObject = tracks.at(trackIndex);
        const QVector<ClipInfo> emptyTrack;
        const QVector<ClipInfo>& track = trackObject ? trackObject->clips() : emptyTrack;
        QVector<ClipOverlap> overlaps(track.size());
        if (trackIndex < intervals.size()) {
            QVector<QVector<OverlapInterval>> clipBounds(track.size());
            for (const auto& interval : intervals.at(trackIndex)) {
                if (interval.clipIdx >= 0 && interval.clipIdx < clipBounds.size())
                    clipBounds[interval.clipIdx].append(interval);
            }
            for (int clipIndex = 0; clipIndex + 1 < track.size(); ++clipIndex) {
                QVector<QPair<double, double>> intersections;
                for (const auto& a : clipBounds.at(clipIndex)) {
                    for (const auto& b : clipBounds.at(clipIndex + 1)) {
                        const double start = qMax(a.timelineStart, b.timelineStart);
                        const double end = qMin(a.timelineEnd, b.timelineEnd);
                        if (end > start) intersections.append(qMakePair(start, end));
                    }
                }
                std::sort(intersections.begin(), intersections.end());
                double duration = 0.0;
                double coveredEnd = -std::numeric_limits<double>::infinity();
                for (const auto& span : intersections) {
                    duration += qMax(0.0, span.second - qMax(span.first, coveredEnd));
                    coveredEnd = qMax(coveredEnd, span.second);
                }
                overlaps[clipIndex].trailOutSec = duration;
                overlaps[clipIndex + 1].leadInSec = duration;
            }
        }
        for (int clipIndex = 0; clipIndex < track.size(); ++clipIndex) {
            const ClipInfo& clip = track.at(clipIndex);
            // TimelineSequence::duration() and Timeline's placement logic both
            // treat leadInSec as a gap before the clip, then add effectiveDuration().
            // Therefore cursor + leadInSec is the absolute timeline start.
            cursorSec += clip.leadInSec;
            clips.append(clipToJson(clip, clipIndex, cursorSec,
                                    trackObject && trackObject->isClipSelected(clipIndex),
                                    overlaps.at(clipIndex)));
            cursorSec += clip.speed > 0.0 ? clip.effectiveDuration() : 0.0;
        }
        result.append(QJsonObject{
            {QStringLiteral("index"), trackIndex},
            {QStringLiteral("locked"), trackObject && trackObject->isLocked()},
            {QStringLiteral("clips"), clips}
        });
    }
    return result;
}

} // namespace

McpEditorTools::McpEditorTools(MainWindow* window, McpToolRegistry* registry)
    : m_window(window)
    , m_registry(registry)
{
}

McpEditorTools::~McpEditorTools()
{
    // ハンドラのラムダは this を捕捉しているため、MainWindow が子の
    // RenderQueue を破棄する前に接続を外してダングリング参照を残さない。
    QObject::disconnect(m_exportProgressConnection);
    QObject::disconnect(m_exportCompletedConnection);
    m_observedRenderQueue = nullptr;
}

Timeline* McpEditorTools::timeline() const
{
    return m_window ? m_window->m_timeline : nullptr;
}

bool McpEditorTools::beginExclusiveWrite(const QString& toolName, QString* err)
{
    if (!m_activeWriteTool.isEmpty()) {
        return setError(err,
                        QStringLiteral("Another operation is in progress (%1). Please wait for it to finish and try again.")
                            .arg(m_activeWriteTool));
    }
    m_activeWriteTool = toolName;
    return true;
}

void McpEditorTools::endExclusiveWrite()
{
    m_activeWriteTool.clear();
}

namespace {

// export_video はライブの Timeline をレンダースレッドへ渡すので、レンダリング中に
// クリップ構成を変える変更系ツールは拒否する。選択・再生ヘッド・保存・字幕エディタ
// 側の一覧編集はタイムラインのクリップ配列を触らないので通す。
bool toolMutatesTimeline(const QString& toolName)
{
    static const QSet<QString> kNonMutating{
        QStringLiteral("export_video"), QStringLiteral("select_clip"),
        QStringLiteral("clear_selection"), QStringLiteral("set_playhead"),
        QStringLiteral("match_frame"),
        QStringLiteral("save_project"), QStringLiteral("set_track_locked"),
        QStringLiteral("set_project_option"),
        QStringLiteral("add_caption"),
        QStringLiteral("remove_caption"), QStringLiteral("clear_captions")
    };
    return !kNonMutating.contains(toolName);
}

} // namespace

ToolHandler McpEditorTools::guardedWrite(const QString& toolName, ToolHandler inner)
{
    return [this, toolName, inner](const QJsonObject& args, QString* err) -> QJsonObject {
        if (!beginExclusiveWrite(toolName, err))
            return {};
        struct Reset {
            McpEditorTools* tools;
            ~Reset() { tools->endExclusiveWrite(); }
        } reset{this};
        RenderQueue* queue = m_window ? m_window->m_renderQueue : nullptr;
        if (queue && toolMutatesTimeline(toolName)) {
            for (const RenderJob& job : queue->jobs()) {
                if (job.status != RenderJobStatus::Rendering)
                    continue;
                return setError(err,
                                QStringLiteral("Cannot modify the timeline while exporting (jobId %1). Wait until get_export_status reports done / failed, then try again.")
                                    .arg(job.uuid)),
                       QJsonObject();
            }
        }
        return inner(args, err);
    };
}

void McpEditorTools::syncSelectionAfterEdit()
{
    Timeline* currentTimeline = timeline();
    if (!m_window || !currentTimeline)
        return;

    bool audioSelection = false;
    int selectedTrack = -1;
    int selectedClip = -1;
    bool invalidSelection = false;

    for (int trackIndex = 0;
         trackIndex < currentTimeline->videoTracks().size(); ++trackIndex) {
        TimelineTrack* track = currentTimeline->videoTracks().at(trackIndex);
        if (!track)
            continue;
        const int clipIndex = track->selectedClip();
        if (clipIndex < 0)
            continue;
        if (clipIndex >= track->clipCount()) {
            invalidSelection = true;
        } else if (selectedTrack < 0) {
            selectedTrack = trackIndex;
            selectedClip = clipIndex;
        }
    }

    for (int trackIndex = 0;
         trackIndex < currentTimeline->audioTracks().size(); ++trackIndex) {
        TimelineTrack* track = currentTimeline->audioTracks().at(trackIndex);
        if (!track)
            continue;
        const int clipIndex = track->selectedClip();
        if (clipIndex < 0)
            continue;
        if (clipIndex >= track->clipCount()) {
            invalidSelection = true;
        } else if (selectedTrack < 0) {
            audioSelection = true;
            selectedTrack = trackIndex;
            selectedClip = clipIndex;
        }
    }

    if (invalidSelection || selectedTrack < 0) {
        currentTimeline->clearSelection();
        m_window->m_selectedVideoTrackIndex = -1;
        m_window->m_selectedVideoClipIndexTracked = -1;
        if (m_window->m_player)
            m_window->m_player->setEditTargetByClip(-1, -1);
    } else {
        QString ignored;
        currentTimeline->selectClipByIndex(audioSelection, selectedTrack,
                                           selectedClip, &ignored);
        // select_clip と同じく、同値選択でシグナルが省略されても追跡値を揃える。
        m_window->m_selectedVideoTrackIndex = audioSelection ? -1 : selectedTrack;
        m_window->m_selectedVideoClipIndexTracked = selectedClip;
        if (m_window->m_player)
            m_window->m_player->setEditTargetByClip(
                audioSelection ? -1 : selectedTrack, selectedClip);
    }
    m_window->updateEditActions();
}

RenderQueue* McpEditorTools::ensureRenderQueue(QString* err)
{
    if (!m_window)
        return setError(err, QStringLiteral("editor not available")), nullptr;

    if (!m_window->m_renderQueue)
        m_window->m_renderQueue = new RenderQueue(m_window);
    observeRenderQueue(m_window->m_renderQueue);
    return m_window->m_renderQueue;
}

void McpEditorTools::observeRenderQueue(RenderQueue* queue)
{
    if (!queue || !m_window || m_observedRenderQueue == queue)
        return;

    QObject::disconnect(m_exportProgressConnection);
    QObject::disconnect(m_exportCompletedConnection);
    m_observedRenderQueue = queue;

    // RenderQueue の進捗シグナルはワーカースレッドから届くことがあるため、
    // MainWindow を context にして GUI スレッドで MCP 用スナップショットを更新する。
    m_exportProgressConnection = QObject::connect(
        queue, &RenderQueue::jobProgressUuid, m_window,
        [this](const QString& jobId, int percent) {
            ExportJobObservation& observation = m_exportJobObservations[jobId];
            // 完了通知より遅れて届く進捗通知で、最終スナップショットを
            // 99% などへ巻き戻さない。
            if (observation.status == QStringLiteral("done")
                || observation.status == QStringLiteral("failed"))
                return;
            observation.status = QStringLiteral("running");
            observation.progress = qBound(0, percent, 100);
        });
    m_exportCompletedConnection = QObject::connect(
        queue, &RenderQueue::jobCompletedUuid, m_window,
        [this](const QString& jobId, bool success, const QString& error) {
            ExportJobObservation& observation = m_exportJobObservations[jobId];
            observation.status = success ? QStringLiteral("done")
                                         : QStringLiteral("failed");
            observation.progress = success ? 100 : qBound(0, observation.progress, 100);
            observation.error = error;
        });
}

QJsonObject McpEditorTools::exportStatus(const QString& jobId, QString* err)
{
    if (!m_window)
        return setError(err, QStringLiteral("editor not available")), QJsonObject();

    RenderQueue* queue = m_window->m_renderQueue;
    if (queue) {
        // status の照会でもシグナル監視を有効にする。MCP 以外から投入された
        // RenderQueue ジョブも、照会開始後は同じ進捗スナップショットで返す。
        observeRenderQueue(queue);

        for (const RenderJob& job : queue->jobs()) {
            if (job.uuid != jobId)
                continue;

            QString status;
            switch (job.status) {
            case RenderJobStatus::Pending:
                status = QStringLiteral("queued");
                break;
            case RenderJobStatus::Rendering:
                status = QStringLiteral("running");
                break;
            case RenderJobStatus::Completed:
                status = QStringLiteral("done");
                break;
            case RenderJobStatus::Failed:
            case RenderJobStatus::Cancelled:
                status = QStringLiteral("failed");
                break;
            }

            int progress = job.status == RenderJobStatus::Completed
                ? 100 : qBound(0, job.progressPercent, 100);
            const auto observation = m_exportJobObservations.constFind(jobId);
            if (observation != m_exportJobObservations.constEnd()) {
                if (status == QStringLiteral("running")
                    || status == QStringLiteral("queued")) {
                    progress = observation->progress;
                } else if (status == QStringLiteral("done")) {
                    progress = 100;
                }
            }

            QJsonObject result{
                {QStringLiteral("ok"), true},
                {QStringLiteral("jobId"), jobId},
                {QStringLiteral("status"), status},
                {QStringLiteral("progress"), progress}
            };
            if (!job.outputPath.isEmpty())
                result.insert(QStringLiteral("outputPath"), job.outputPath);
            if (status == QStringLiteral("failed")) {
                const QString jobError = !job.error.isEmpty()
                    ? job.error : job.errorMessage;
                if (!jobError.isEmpty())
                    result.insert(QStringLiteral("error"), jobError);
                else if (observation != m_exportJobObservations.constEnd()
                         && !observation->error.isEmpty())
                    result.insert(QStringLiteral("error"), observation->error);
                else
                    result.insert(QStringLiteral("error"),
                                  QStringLiteral("Export failed"));
            }
            return result;
        }
    }

    // RenderQueue の clearCompleted() などで完了ジョブ本体が片付けられても、
    // MCP が観測した最終スナップショットは McpEditorTools の存続中保持する。
    // したがって jobId の寿命は通常 MainWindow / プロセス終了までである。
    const auto observation = m_exportJobObservations.constFind(jobId);
    if (observation != m_exportJobObservations.constEnd()) {
        QJsonObject result{
            {QStringLiteral("ok"), true},
            {QStringLiteral("jobId"), jobId},
            {QStringLiteral("status"), observation->status},
            {QStringLiteral("progress"), qBound(0, observation->progress, 100)}
        };
        if (observation->status == QStringLiteral("failed")
            && !observation->error.isEmpty()) {
            result.insert(QStringLiteral("error"), observation->error);
        } else if (observation->status == QStringLiteral("failed")) {
            result.insert(QStringLiteral("error"),
                          QStringLiteral("Export failed"));
        }
        return result;
    }

    return setError(err, QStringLiteral("Unknown jobId: %1").arg(jobId)), QJsonObject();
}

void McpEditorTools::registerReadTools()
{
    if (!m_registry)
        return;

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("compare_project"),
        QStringLiteral("Compares the saved version with the current timeline's video/audio clips and track settings. Read-only. Before = saved version, after = current. Removed items use the saved version's clip numbers; all others use the current clip numbers."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("filePath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
        }, {QStringLiteral("filePath")}),
        [this](const QJsonObject &args, QString *err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("filePath")}, err)) return {};
            QString filePath;
            if (!requiredString(args, QStringLiteral("filePath"), &filePath, err)) return {};
            const Timeline *currentTimeline = timeline();
            if (!currentTimeline)
                return setError(err, QStringLiteral("Editor is unavailable")), QJsonObject();
            ProjectData saved;
            if (!ProjectFile::load(filePath, saved))
                return setError(err, QStringLiteral("Could not load project: %1").arg(filePath)), QJsonObject();
            ProjectData current;
            current.videoTracks = currentTimeline->allVideoTracks();
            current.audioTracks = currentTimeline->allAudioTracks();
            current.trackFlags = currentTimeline->trackFlagsToJson();
            QJsonArray changes;
            int added = 0, removed = 0, moved = 0, trimmed = 0, changed = 0;
            for (const auto &change : projdiff::diff(saved, current)) {
                changes.append(QJsonObject{{"type", projdiff::typeName(change.type)},
                    {"path", change.path}, {"before", change.before}, {"after", change.after}});
                switch (change.type) {
                case projdiff::Change::Added: ++added; break;
                case projdiff::Change::Removed: ++removed; break;
                case projdiff::Change::Moved: ++moved; break;
                case projdiff::Change::Trimmed: ++trimmed; break;
                default: ++changed; break;
                }
            }
            return {{"ok", true}, {"changes", changes},
                    {"summary", QJsonObject{{"added", added}, {"removed", removed},
                        {"moved", moved}, {"trimmed", trimmed}, {"changed", changed}}}};
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("changes"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), outputSchemaOf(QJsonObject{
                {QStringLiteral("type"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                {QStringLiteral("before"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                {QStringLiteral("after"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
            }, {QStringLiteral("type"), QStringLiteral("path"), QStringLiteral("before"), QStringLiteral("after")})}
        }},
        {QStringLiteral("summary"), outputSchemaOf(QJsonObject{
            {QStringLiteral("added"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("removed"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("moved"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("trimmed"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("changed"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
        }, {QStringLiteral("added"), QStringLiteral("removed"), QStringLiteral("moved"),
            QStringLiteral("trimmed"), QStringLiteral("changed")})}
    }, {QStringLiteral("ok"), QStringLiteral("changes"), QStringLiteral("summary")})));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_project_info"),
        QStringLiteral("Returns project settings, duration, playhead position, and track counts in seconds. Use to check the current state before editing."),
        objectSchema(),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            const Timeline* currentTimeline = timeline();
            const QVector<QVector<ClipInfo>> videoTracks = currentTimeline
                ? QVector<QVector<ClipInfo>>(currentTimeline->allVideoTracks())
                : QVector<QVector<ClipInfo>>();
            const QVector<QVector<ClipInfo>> audioTracks = currentTimeline
                ? QVector<QVector<ClipInfo>>(currentTimeline->allAudioTracks())
                : QVector<QVector<ClipInfo>>();
            // MainWindow owns the project configuration; when its Timeline is
            // absent there is no loaded editor project, so expose empty/zero values.
            const bool projectAvailable = currentTimeline != nullptr;
            return QJsonObject{
                {QStringLiteral("projectName"), projectAvailable
                    ? m_window->m_projectConfig.name : QString()},
                {QStringLiteral("width"), projectAvailable
                    ? m_window->m_projectConfig.width : 0},
                {QStringLiteral("height"), projectAvailable
                    ? m_window->m_projectConfig.height : 0},
                {QStringLiteral("fps"), projectAvailable
                    ? static_cast<double>(m_window->m_projectConfig.fps) : 0.0},
                {QStringLiteral("durationSec"), currentTimeline
                    ? currentTimeline->totalDuration() : 0.0},
                {QStringLiteral("playheadSec"), currentTimeline
                    ? currentTimeline->playheadPosition() : 0.0},
                {QStringLiteral("videoTrackCount"), videoTracks.size()},
                {QStringLiteral("audioTrackCount"), audioTracks.size()},
                {QStringLiteral("hasUnsavedChanges"), m_window->isWindowModified()},
                {QStringLiteral("timecodeBurnIn"),
                 timecodeBurnInForMcp(m_window->m_tcBurnIn)}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("projectName"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("width"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("height"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("fps"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("playheadSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("videoTrackCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("audioTrackCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("hasUnsavedChanges"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("timecodeBurnIn"), timecodeBurnInSchema()}
    }, {QStringLiteral("projectName"), QStringLiteral("width"),
        QStringLiteral("height"), QStringLiteral("fps"),
        QStringLiteral("durationSec"), QStringLiteral("playheadSec"),
        QStringLiteral("videoTrackCount"), QStringLiteral("audioTrackCount"),
        QStringLiteral("hasUnsavedChanges"),
        QStringLiteral("timecodeBurnIn")})));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_frame"),
        QStringLiteral("Returns the composited frame at the specified timeline time as a PNG image. If maxWidth is omitted, scales down to 640px or less and keeps the response under 1MB so it does not pressure the LLM's context."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }},
            {QStringLiteral("maxWidth"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 1}
            }}
        }, {QStringLiteral("timeSec")}),
        {},
        [this](const QJsonObject& args, QString* err,
               QJsonArray* content) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("timeSec"),
                                         QStringLiteral("maxWidth")}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("Editor or timeline is not available")),
                       QJsonObject();
            if (!content)
                return setError(err, QStringLiteral("Image content output destination is unavailable")),
                       QJsonObject();

            double timeSec = 0.0;
            if (!args.contains(QStringLiteral("timeSec"))) {
                if (err)
                    *err = QStringLiteral("timeSec is required");
                return {};
            }
            if (!finiteNumberForMcp(args, QStringLiteral("timeSec"),
                                    &timeSec, err))
                return {};

            const double durationSec = qMax(0.0, timeline()->totalDuration());
            // Timeline の終端は次のフレームが存在しない半開区間なので、
            // 有効範囲を [0, duration) として終端も明示的に拒否する。
            if (durationSec <= 0.0 || timeSec < 0.0 || timeSec >= durationSec) {
                return setError(err,
                                QStringLiteral("timeSec is out of timeline range: %1 (range 0 to under %2)")
                                    .arg(timeSec, 0, 'f', 6)
                                    .arg(durationSec, 0, 'f', 6)),
                       QJsonObject();
            }

            int maxWidth = kDefaultFrameMaxWidth;
            if (!positiveInteger(args, QStringLiteral("maxWidth"),
                                 kDefaultFrameMaxWidth, &maxWidth, err))
                return {};

            const int canvasWidth = qMax(1, m_window->m_projectConfig.width);
            const int canvasHeight = qMax(1, m_window->m_projectConfig.height);
            const int renderWidth = qMax(1, qMin(canvasWidth, maxWidth));
            const int renderHeight = qMax(1, qRound(
                static_cast<double>(canvasHeight) * renderWidth
                / static_cast<double>(canvasWidth)));
            const qint64 usec = qRound64(timeSec * 1'000'000.0);
            QImage image = tlrender::renderFrameAt(
                timeline(), usec, QSize(renderWidth, renderHeight));
            if (image.isNull())
                return setError(err, QStringLiteral("Could not render the frame at the specified time")),
                       QJsonObject();
            if (image.format() != QImage::Format_RGBA8888)
                image = image.convertToFormat(QImage::Format_RGBA8888);

            // 指定幅が大きくても、base64 と JSON envelope を含む応答全体が
            // 1MB を超えないよう PNG の再圧縮ではなく画像自体を段階的に縮小する。
            QByteArray png;
            QByteArray base64;
            bool fitsResponseLimit = false;
            for (int attempt = 0; attempt < 16; ++attempt) {
                if (!encodePng(image, &png))
                    return setError(err, QStringLiteral("Could not encode frame as PNG")),
                           QJsonObject();
                base64 = png.toBase64();
                if (base64.size() + kFrameResponseOverhead
                        <= kMaxFrameResponseBytes
                    || image.width() <= 1) {
                    fitsResponseLimit = base64.size() + kFrameResponseOverhead
                        <= kMaxFrameResponseBytes;
                    break;
                }

                const int nextWidth = qMax(1, qMin(image.width() - 1,
                                                   qRound(image.width() * 0.8)));
                const int nextHeight = qMax(1, qRound(
                    static_cast<double>(image.height()) * nextWidth
                    / static_cast<double>(image.width())));
                image = image.scaled(QSize(nextWidth, nextHeight),
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            // ループ上限で縮小した場合にも、content と payload が同じ PNG を
            // 参照するよう最後の画像を必ず再エンコードして判定する。
            if (!fitsResponseLimit) {
                if (!encodePng(image, &png))
                    return setError(err, QStringLiteral("Could not encode frame as PNG")),
                           QJsonObject();
                base64 = png.toBase64();
                fitsResponseLimit = base64.size() + kFrameResponseOverhead
                    <= kMaxFrameResponseBytes;
            }
            if (!fitsResponseLimit)
                return setError(err, QStringLiteral("Could not shrink the PNG response to under 1MB")),
                       QJsonObject();

            content->append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("image")},
                {QStringLiteral("data"), QString::fromLatin1(base64)},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")}
            });
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("timeSec"), timeSec},
                {QStringLiteral("width"), image.width()},
                {QStringLiteral("height"), image.height()},
                {QStringLiteral("byteSize"), static_cast<qint64>(png.size())},
                {QStringLiteral("base64Bytes"), static_cast<qint64>(base64.size())}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("timeSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("width"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("height"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("byteSize"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("base64Bytes"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("timeSec"),
        QStringLiteral("width"), QStringLiteral("height"),
        QStringLiteral("byteSize"), QStringLiteral("base64Bytes")})));

    const QJsonObject exportStatusProperties{
        {QStringLiteral("jobId"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_export_status"),
        QStringLiteral("Returns the status and progress of an async export_video job. status is queued / running / done / failed. Includes error on failure."),
        schemaWithRequired(exportStatusProperties, {QStringLiteral("jobId")}),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("jobId")}, err))
                return {};
            QString jobId;
            if (!requiredString(args, QStringLiteral("jobId"), &jobId, err)) {
                if (err)
                    *err = QStringLiteral("jobId is required");
                return {};
            }
            if (jobId.trimmed().isEmpty())
                return setError(err, QStringLiteral("jobId is required")), QJsonObject();
            return exportStatus(jobId, err);
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("jobId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("status"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("progress"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("outputPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("jobId"),
        QStringLiteral("status"), QStringLiteral("progress")})));

    const QJsonObject timelineProperties{
        {QStringLiteral("kind"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("all")
            }},
            {QStringLiteral("default"), QStringLiteral("all")},
            {QStringLiteral("description"),
             QStringLiteral("One of video, audio, all. Defaults to all")}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_timeline"),
        QStringLiteral("Returns all clips on the timeline by track in seconds. kind: video / audio / all (defaults to all). Use to check the current state before editing."),
        objectSchema(timelineProperties),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("kind")}, err))
                return {};
            QString kind = QStringLiteral("all");
            if (args.contains(QStringLiteral("kind"))) {
                const QJsonValue value = args.value(QStringLiteral("kind"));
                if (!value.isString()
                    || (value.toString() != QStringLiteral("video")
                        && value.toString() != QStringLiteral("audio")
                        && value.toString() != QStringLiteral("all"))) {
                    return setError(err, QStringLiteral("kind must be video, audio or all")),
                           QJsonObject();
                }
                kind = value.toString();
            }
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            QJsonObject result;
            if (kind == QStringLiteral("video") || kind == QStringLiteral("all"))
                result.insert(QStringLiteral("video"), QJsonArray());
            if (kind == QStringLiteral("audio") || kind == QStringLiteral("all"))
                result.insert(QStringLiteral("audio"), QJsonArray());
            const Timeline* currentTimeline = timeline();
            if (!currentTimeline)
                return result;

            if (result.contains(QStringLiteral("video")))
                result.insert(QStringLiteral("video"),
                              tracksToJson(currentTimeline, TrackKind::Video));
            if (result.contains(QStringLiteral("audio")))
                result.insert(QStringLiteral("audio"),
                              tracksToJson(currentTimeline, TrackKind::Audio));
            return result;
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("video"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), trackOutputItemSchema()}
        }},
        {QStringLiteral("audio"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), trackOutputItemSchema()}
        }}
    }, {})));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_captions"),
        QStringLiteral("captions is the subtitle editor's content; timelineCaptions is the one-word subtitles already applied to the timeline (only the latter is undone by undo), returned in seconds."),
        objectSchema(),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            QJsonArray captions;
            if (m_window->m_captionEditorDialog) {
                const QList<caption::Clip> clips =
                    m_window->m_captionEditorDialog->track().clips();
                for (int index = 0; index < clips.size(); ++index) {
                    const caption::Clip& clip = clips.at(index);
                    captions.append(QJsonObject{
                        {QStringLiteral("index"), index},
                        {QStringLiteral("startSec"),
                         static_cast<double>(clip.startMs) / 1000.0},
                        {QStringLiteral("endSec"),
                         static_cast<double>(clip.endMs) / 1000.0},
                        {QStringLiteral("text"), clip.text}
                    });
                }
            }

            QJsonArray timelineCaptions;
            const Timeline* currentTimeline = timeline();
            if (currentTimeline) {
                const QVector<EnhancedTextOverlay>& overlays =
                    currentTimeline->generatedCaptionOverlays();
                for (int index = 0; index < overlays.size(); ++index) {
                    const EnhancedTextOverlay& overlay = overlays.at(index);
                    timelineCaptions.append(QJsonObject{
                        {QStringLiteral("index"), index},
                        {QStringLiteral("startSec"), overlay.startTime},
                        {QStringLiteral("endSec"), overlay.endTime},
                        {QStringLiteral("text"), overlay.text}
                    });
                }
            }
            return QJsonObject{
                {QStringLiteral("captions"), captions},
                {QStringLiteral("timelineCaptions"), timelineCaptions},
                {QStringLiteral("timelineCaptionCount"), timelineCaptions.size()}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("captions"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), captionOutputItemSchema()}
        }},
        {QStringLiteral("timelineCaptions"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), captionOutputItemSchema()}
        }},
        {QStringLiteral("timelineCaptionCount"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")}
        }}
    }, {QStringLiteral("captions"), QStringLiteral("timelineCaptions"),
        QStringLiteral("timelineCaptionCount")})));

    const QJsonObject commandProperties{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("description"),
             QStringLiteral("Case-insensitive partial-match filter on id / display name / menu hierarchy. Omit for all")}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("list_commands"),
        QStringLiteral("Lists the favorite-registerable commands available in the editor. Specifying query filters by case-insensitive partial match on id / display name / menu hierarchy. Omitting query returns everything (about 230 entries, ~60KB of JSON), so normally filter with query. Returns id, display name, menu hierarchy, risk level (safe / blocking / quit), and enabled state. blocking commands are refused by run_command by default."),
        objectSchema(commandProperties),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("query")}, err))
                return {};
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            const QString query = args.value(QStringLiteral("query")).toString();
            QJsonArray commands;
            for (const auto& command : m_window->m_favoritableActions) {
                const bool matches = query.isEmpty()
                    || command.id.contains(query, Qt::CaseInsensitive)
                    || command.label.contains(query, Qt::CaseInsensitive)
                    || command.menuPath.contains(query, Qt::CaseInsensitive);
                if (!matches)
                    continue;
                commands.append(QJsonObject{
                    {QStringLiteral("id"), command.id},
                    {QStringLiteral("label"), command.label},
                    {QStringLiteral("menuPath"), command.menuPath},
                    {QStringLiteral("risk"), actionRiskToString(command.risk)},
                    {QStringLiteral("enabled"),
                     command.action ? command.action->isEnabled() : false}
                });
            }
            return QJsonObject{
                {QStringLiteral("commands"), commands},
                {QStringLiteral("total"), m_window->m_favoritableActions.size()}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("commands"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), commandOutputItemSchema()}
        }},
        {QStringLiteral("total"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("description"),
             QStringLiteral("Total command count before filtering (not the number of commands)")}
        }}
    }, {QStringLiteral("commands"), QStringLiteral("total")})));
}

void McpEditorTools::registerWriteTools()
{
    if (!m_registry)
        return;

    const QJsonObject clipProperties = clipSelectorProperties();

    const QJsonObject exportVideoOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("jobId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("status"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{QStringLiteral("queued")}}
        }},
        {QStringLiteral("progress"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("outputPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("width"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("height"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("fps"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("videoCodec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("videoBitrate"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("audioCodec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("audioBitrate"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("jobId"),
        QStringLiteral("status"), QStringLiteral("progress"),
        QStringLiteral("outputPath"), QStringLiteral("width"),
        QStringLiteral("height"), QStringLiteral("fps"),
        QStringLiteral("videoCodec"), QStringLiteral("videoBitrate")});

    const QJsonObject importMediaOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("clips"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), importedClipOutputItemSchema()}
        }},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("ok"), QStringLiteral("clips")});

    const QJsonObject saveProjectOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("path")});

    const QJsonObject openProjectOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("path")});

    const QJsonObject setTrackLockedOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("locked"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("locked")});

    const QJsonObject selectClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex")});

    const QJsonObject clearSelectionOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("ok")});

    const QJsonObject runCommandOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("label"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("undoRecorded"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("undoDescription"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("id"),
        QStringLiteral("label"), QStringLiteral("risk"),
        QStringLiteral("undoRecorded")});

    const QJsonObject splitClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("newClipCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("leftIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("rightIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("newClipCount"),
        QStringLiteral("leftIndex"), QStringLiteral("rightIndex")});

    const QJsonObject deleteClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("remainingClipCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("remainingClipCount")});

    const QJsonObject moveClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("actualStartSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("startSec"),
        QStringLiteral("actualStartSec"), QStringLiteral("trackIndex")});

    const QJsonObject matchFrameOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("filePath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("sourceSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("filePath"), QStringLiteral("sourceSec"),
        QStringLiteral("clipIndex")});

    const QJsonObject replaceClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("warning"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok")});

    const QJsonObject relinkMediaOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("relinked"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("relinked")});

    const QJsonObject setClipPropertyOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("property"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("oneOf"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}
        }}}},
        {QStringLiteral("linkedApplied"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("ok"), QStringLiteral("property"),
        QStringLiteral("value")});

    const QJsonObject musicRemixOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("targetSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("resultDuration"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("segmentCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("targetSec"), QStringLiteral("resultDuration"),
        QStringLiteral("segmentCount")});

    const QJsonObject dialogueLevelOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("targetLufs"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("pointCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("measuredLufsMin"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("measuredLufsMax"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("targetLufs"), QStringLiteral("pointCount"),
        QStringLiteral("measuredLufsMin"), QStringLiteral("measuredLufsMax")});

    const QJsonObject dynamicZoomOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("warning"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("keyframeCount"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0}
        }},
        {QStringLiteral("keyframeCounts"), outputSchemaOf(QJsonObject{
            {QStringLiteral("positionX"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("positionY"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("scaleX"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
            {QStringLiteral("scaleY"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
        }, {QStringLiteral("positionX"), QStringLiteral("positionY"),
            QStringLiteral("scaleX"), QStringLiteral("scaleY")})}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("keyframeCount"), QStringLiteral("keyframeCounts")});

    const QJsonObject setClipLabelOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("label"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), clipLabelEnum()}
        }}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("label")});

    const QJsonObject trimClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("edge"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("ripple"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("endSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("edge"), QStringLiteral("ripple"),
        QStringLiteral("startSec"), QStringLiteral("endSec")});

    const QJsonObject setTransitionOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("warning"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("type"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("leadIn"), transitionOutputItemSchema()},
        {QStringLiteral("trailOut"), transitionOutputItemSchema()}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("type"), QStringLiteral("durationSec"),
        QStringLiteral("leadIn"), QStringLiteral("trailOut")});

    const QJsonObject addTextOverlayOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("endSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("x"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("y"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("fontSize"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("color"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("clipIndices"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
        }}
    }, {QStringLiteral("ok"), QStringLiteral("index"),
        QStringLiteral("text"), QStringLiteral("startSec"),
        QStringLiteral("endSec"), QStringLiteral("x"),
        QStringLiteral("y"), QStringLiteral("fontSize"),
        QStringLiteral("color")});

    const QJsonObject addCaptionOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("captionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("index"),
        QStringLiteral("captionCount")});

    const QJsonObject applyCaptionsOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("appliedCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("captionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("timelineCaptionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("appliedCount"),
        QStringLiteral("captionCount"), QStringLiteral("timelineCaptionCount")});

    const QJsonObject setPlayheadOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("playheadSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("playing"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("previewSeekRequested"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("ok"), QStringLiteral("playheadSec"),
        QStringLiteral("playing"), QStringLiteral("previewSeekRequested")});

    const QJsonObject undoOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok")});

    const QJsonObject redoOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok")});

    const QJsonObject setProjectOptionOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("option"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("value"), timecodeBurnInSchema()}
    }, {QStringLiteral("ok"), QStringLiteral("option"),
        QStringLiteral("value")});

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_project_option"),
        QStringLiteral("Changes project-wide settings. Currently accepts option=timecodeBurnIn, applied immediately to the preview and subsequent exports. Not undoable."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("option"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("timecodeBurnIn")
                }}
            }},
            {QStringLiteral("value"), timecodeBurnInInputSchema()}
        }, {QStringLiteral("option"), QStringLiteral("value")}),
        guardedWrite(QStringLiteral("set_project_option"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(
                    args,
                    {QStringLiteral("option"), QStringLiteral("value")},
                    err)) {
                return {};
            }

            QString option;
            if (!requiredString(args, QStringLiteral("option"), &option, err))
                return {};
            if (option != QStringLiteral("timecodeBurnIn")) {
                return setError(err,
                                QStringLiteral("unknown project option: %1")
                                    .arg(option)),
                       QJsonObject();
            }
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")),
                       QJsonObject();

            const QJsonValue rawValue = args.value(QStringLiteral("value"));
            if (!rawValue.isObject()) {
                return setError(err,
                                QStringLiteral("value must be an object for timecodeBurnIn")),
                       QJsonObject();
            }
            const QJsonObject value = rawValue.toObject();
            const QStringList allowedFields{
                QStringLiteral("enabled"), QStringLiteral("position"),
                QStringLiteral("fontSizePct"), QStringLiteral("showFrames"),
                QStringLiteral("dropFrame"), QStringLiteral("prefix"),
                QStringLiteral("showClipName"), QStringLiteral("opacity")
            };
            if (!rejectUnknownArguments(value, allowedFields, err))
                return {};

            const QStringList booleanFields{
                QStringLiteral("enabled"), QStringLiteral("showFrames"),
                QStringLiteral("dropFrame"), QStringLiteral("showClipName")
            };
            for (const QString &name : booleanFields) {
                if (value.contains(name) && !value.value(name).isBool()) {
                    return setError(
                               err,
                               QStringLiteral("%1 must be a boolean").arg(name)),
                           QJsonObject();
                }
            }
            if (value.contains(QStringLiteral("prefix"))
                && !value.value(QStringLiteral("prefix")).isString()) {
                return setError(err, QStringLiteral("prefix must be a string")),
                       QJsonObject();
            }
            if (value.contains(QStringLiteral("position"))) {
                const QJsonValue rawPosition = value.value(
                    QStringLiteral("position"));
                TimecodeBurnInSettings::Position parsedPosition;
                if (!rawPosition.isString()
                    || !TimecodeBurnInSettings::positionFromName(
                        rawPosition.toString(), &parsedPosition)) {
                    return setError(err,
                                    QStringLiteral("position is invalid")),
                           QJsonObject();
                }
            }
            if (value.contains(QStringLiteral("fontSizePct"))) {
                const QJsonValue rawFontSize = value.value(
                    QStringLiteral("fontSizePct"));
                const double fontSize = rawFontSize.toDouble(-1.0);
                if (!rawFontSize.isDouble() || !std::isfinite(fontSize)
                    || std::floor(fontSize) != fontSize
                    || fontSize < 1.0 || fontSize > 20.0) {
                    return setError(
                               err,
                               QStringLiteral("fontSizePct must be an integer from 1 to 20")),
                           QJsonObject();
                }
            }
            if (value.contains(QStringLiteral("opacity"))) {
                const QJsonValue rawOpacity = value.value(
                    QStringLiteral("opacity"));
                const double opacity = rawOpacity.toDouble(-1.0);
                if (!rawOpacity.isDouble() || !std::isfinite(opacity)
                    || opacity < 0.0 || opacity > 1.0) {
                    return setError(err,
                                    QStringLiteral("opacity must be from 0 to 1")),
                           QJsonObject();
                }
            }

            QJsonObject merged = m_window->m_tcBurnIn.toJson();
            for (auto it = value.constBegin(); it != value.constEnd(); ++it)
                merged.insert(it.key(), it.value());
            const TimecodeBurnInSettings updated =
                TimecodeBurnInSettings::fromJson(merged);
            m_window->applyTimecodeBurnInSettings(updated);
            m_window->setWindowModified(true);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("option"), option},
                {QStringLiteral("value"), timecodeBurnInForMcp(updated)}
            };
        })
    }, setProjectOptionOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("export_video"),
        QStringLiteral("Exports the current timeline to a video file asynchronously. tools/call returns a jobId immediately after submission; check completion with get_export_status. width / height / fps default to the current project settings; videoBitrate / audioBitrate are in kbps (defaults 10000 / 192); videoCodec / audioCodec are ffmpeg encoder names (defaults libx264 / aac). Audio is mixed from the timeline reflecting trim, split, reorder, volume, and mute via ffmpeg before muxing (fails for anything beyond a simple single-clip composition if ffmpeg is not on PATH)."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("outputPath"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("width"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 2}
            }},
            {QStringLiteral("height"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 2}
            }},
            {QStringLiteral("fps"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("exclusiveMinimum"), 0}
            }},
            {QStringLiteral("videoCodec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("videoBitrate"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 1},
                {QStringLiteral("description"), QStringLiteral("kbps")}
            }},
            {QStringLiteral("audioCodec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("ffmpeg audio encoder name. Default aac")}
            }},
            {QStringLiteral("audioBitrate"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 1},
                {QStringLiteral("description"), QStringLiteral("kbps. Default 192")}
            }}
        }, {QStringLiteral("outputPath")}),
        guardedWrite(QStringLiteral("export_video"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("outputPath"),
                                         QStringLiteral("width"),
                                         QStringLiteral("height"),
                                         QStringLiteral("fps"),
                                         QStringLiteral("videoCodec"),
                                         QStringLiteral("videoBitrate"),
                                         QStringLiteral("audioCodec"),
                                         QStringLiteral("audioBitrate")}, err))
                return {};

            QString outputPath;
            if (!requiredString(args, QStringLiteral("outputPath"),
                                &outputPath, err)) {
                if (err)
                    *err = QStringLiteral("outputPath is required");
                return {};
            }
            outputPath = outputPath.trimmed();
            if (outputPath.isEmpty())
                return setError(err, QStringLiteral("outputPath is required")),
                       QJsonObject();

            const QFileInfo outputInfo(outputPath);
            if (!outputInfo.absoluteDir().exists()) {
                return setError(err,
                                QStringLiteral("Output parent directory does not exist: %1")
                                    .arg(outputInfo.absoluteDir().absolutePath())),
                       QJsonObject();
            }
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("Editor or timeline is not available")),
                       QJsonObject();
            if (timeline()->totalDuration() <= 0.0)
                return setError(err, QStringLiteral("Timeline is empty. Add media with import_media")),
                       QJsonObject();

            const int defaultWidth = qMax(2, m_window->m_projectConfig.width);
            const int defaultHeight = qMax(2, m_window->m_projectConfig.height);
            const double defaultFps = qMax(1, m_window->m_projectConfig.fps);
            int width = defaultWidth;
            int height = defaultHeight;
            if (!positiveInteger(args, QStringLiteral("width"), defaultWidth,
                                 &width, err)
                || !positiveInteger(args, QStringLiteral("height"), defaultHeight,
                                    &height, err))
                return {};
            if (width < 2 || height < 2)
                return setError(err, QStringLiteral("Please specify width and height as 2 or greater")),
                       QJsonObject();

            double fps = defaultFps;
            if (!positiveFiniteNumber(args, QStringLiteral("fps"), defaultFps,
                                      &fps, err))
                return {};

            // ProjectConfig が保持する書き出し設定は現状サイズと fps までなので、
            // codec / bitrate は ExportConfig と同じ既定値を使う。
            QString videoCodec = QStringLiteral("libx264");
            if (args.contains(QStringLiteral("videoCodec"))) {
                const QJsonValue value = args.value(QStringLiteral("videoCodec"));
                if (!value.isString() || value.toString().trimmed().isEmpty())
                    return setError(err, QStringLiteral("Please specify videoCodec as a non-empty string")),
                           QJsonObject();
                videoCodec = value.toString().trimmed();
            }

            int videoBitrate = 10000; // kbps。ExportConfig の既定値と合わせる。
            if (!positiveInteger(args, QStringLiteral("videoBitrate"),
                                 videoBitrate, &videoBitrate, err))
                return {};

            QString audioCodec = QStringLiteral("aac");
            if (args.contains(QStringLiteral("audioCodec"))) {
                const QJsonValue value = args.value(QStringLiteral("audioCodec"));
                if (!value.isString() || value.toString().trimmed().isEmpty())
                    return setError(err, QStringLiteral("Please specify audioCodec as a non-empty string")),
                           QJsonObject();
                audioCodec = value.toString().trimmed();
            }
            int audioBitrate = 192; // kbps。ExportConfig の既定値と合わせる。
            if (!positiveInteger(args, QStringLiteral("audioBitrate"),
                                 audioBitrate, &audioBitrate, err))
                return {};

            RenderQueue* queue = ensureRenderQueue(err);
            if (!queue)
                return {};

            RenderJob job;
            job.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            job.name = QFileInfo(outputPath).fileName();
            if (job.name.isEmpty())
                job.name = outputPath;
            // projectFilePath は RenderQueue の音声 mux 元を兼ねる。MCP のプロジェクト
            // パスは常にプロジェクト JSON で音声ソースにならないので渡さない (空なら
            // RenderQueue は V1 先頭クリップの元音声へフォールバックする)。タイムラインの
            // 音声ミックスが必要なら、下の遅延ステップでそのパスを入れる。
            job.projectFilePath.clear();
            job.outputPath = outputPath;
            job.width = width;
            job.height = height;
            job.codec = videoCodec;
            job.bitrateBps = static_cast<qint64>(videoBitrate) * 1000;
            job.startUs = 0;
            job.endUs = 0;
            job.timeline = timeline();
            const double loudnessGainDb = exporter_loudnessGainDb();
            job.loudnessGainDb = loudnessGainDb;
            job.exportConfig = QJsonObject{
                {QStringLiteral("width"), width},
                {QStringLiteral("height"), height},
                {QStringLiteral("fps"), fps},
                {QStringLiteral("videoCodec"), videoCodec},
                {QStringLiteral("videoBitrate"), videoBitrate},
                {QStringLiteral("audioCodec"), audioCodec},
                {QStringLiteral("audioBitrate"), audioBitrate},
                {QStringLiteral("loudnessGainDb"), loudnessGainDb}
            };
            job.exportConfig.insert(
                QStringLiteral("timecodeBurnIn"),
                m_window->m_tcBurnIn.toJson());

            // ライブ Timeline を渡す経路は GUI の表示内容をそのまま使うため、
            // RenderQueue のワーカー開始前に MainWindow 側の補助データを同期する。
            m_window->syncProjectLightingToTimeline();
            QHash<QString, TimelineTrackMatteEntry> matteEntries;
            matteEntries.reserve(m_window->m_trackMatteClipEntries.size());
            for (auto it = m_window->m_trackMatteClipEntries.cbegin();
                 it != m_window->m_trackMatteClipEntries.cend(); ++it) {
                TimelineTrackMatteEntry entry;
                entry.matteType = it.value().matteType;
                entry.matteSourceClipId = it.value().matteSourceClipId;
                matteEntries.insert(it.key(), entry);
            }
            timeline()->setTrackMatteEntries(matteEntries);
            queue->setAcesPipeline(m_window->m_acesPipeline);
            queue->setLoudnessGainDb(loudnessGainDb);
            m_exportJobObservations.insert(job.uuid,
                                           ExportJobObservation{
                                               QStringLiteral("queued"), 0, QString()});

            // 音声ミックス (ffmpeg 同期実行) と start() は tools/call の中では走らせない。
            // jobId を先に返し、GUI イベントループでミックスを作ってからジョブを投入する。
            // それまで get_export_status は観測テーブルの queued を返す。
            const QPointer<RenderQueue> queueGuard(queue);
            const QPointer<MainWindow> windowGuard(m_window);
            const QString jobId = job.uuid;
            QTimer::singleShot(0, m_window, [this, queueGuard, windowGuard, job, jobId]() mutable {
                if (!queueGuard || !windowGuard || windowGuard->m_mcpTools != this)
                    return;
                QString audioMixError;
                const QString audioMixPath = windowGuard->prepareExportAudioMix(&audioMixError);
                if (!audioMixError.isEmpty()) {
                    ExportJobObservation& observation = m_exportJobObservations[jobId];
                    observation.status = QStringLiteral("failed");
                    observation.error = audioMixError;
                    return;
                }
                if (!audioMixPath.isEmpty())
                    job.projectFilePath = audioMixPath;
                queueGuard->addJob(job);
                queueGuard->start();
            });

            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("jobId"), job.uuid},
                {QStringLiteral("status"), QStringLiteral("queued")},
                {QStringLiteral("progress"), 0},
                {QStringLiteral("outputPath"), outputPath},
                {QStringLiteral("width"), width},
                {QStringLiteral("height"), height},
                {QStringLiteral("fps"), fps},
                {QStringLiteral("videoCodec"), videoCodec},
                {QStringLiteral("videoBitrate"), videoBitrate},
                {QStringLiteral("audioCodec"), audioCodec},
                {QStringLiteral("audioBitrate"), audioBitrate}
            };
        })
    }, exportVideoOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("import_media"),
        QStringLiteral("Imports media to the specified track without opening a dialog. trackIndex refers to the video/audio track pair with the same number; creates both if missing (0-based, default 0). If startSec is omitted, appends to the end of the specified video track and places audio at the same start time. If startSec is specified, refuses with an error instead of placing at a position overlapping existing clips (no rounding). kind defaults to auto, decided by the file's stream layout; files without video (BGM / narration) go to the audio track only. video / audio imports one side only. Video/audio pairs are linked with the same linkGroup as the existing GUI path and can be undone with a single undo. Files that cannot be opened as video/audio are an error."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("filePath"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("kind"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("auto"), QStringLiteral("video"), QStringLiteral("audio")
                }},
                {QStringLiteral("default"), QStringLiteral("auto")},
                {QStringLiteral("description"),
                 QStringLiteral("auto: V/A pair if video exists, audio only otherwise. video: video only. audio: audio only (for BGM or narration). Defaults to auto")}
            }},
            {QStringLiteral("trackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("default"), 0},
                {QStringLiteral("description"),
                 QStringLiteral("0-based track number shared by video and audio. Creates both if missing. Defaults to 0")}
            }},
            {QStringLiteral("startSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("Timeline absolute time (sec). If omitted, appends to the end of the specified video track and places audio at the same start time. Refuses with an error if it would overlap existing clips (no rounding)")}
            }}
        }, {QStringLiteral("filePath")}),
        guardedWrite(QStringLiteral("import_media"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("filePath"),
                                         QStringLiteral("trackIndex"),
                                         QStringLiteral("startSec"),
                                         QStringLiteral("kind")}, err))
                return {};
            QString filePath;
            if (!requiredString(args, QStringLiteral("filePath"), &filePath, err))
                return {};
            if (filePath.isEmpty())
                return setError(err, QStringLiteral("File not found: %1").arg(filePath)),
                       QJsonObject();
            Timeline::ImportMediaKind importKind = Timeline::ImportMediaKind::Auto;
            if (args.contains(QStringLiteral("kind"))) {
                const QJsonValue kindValue = args.value(QStringLiteral("kind"));
                const QString kind = kindValue.isString() ? kindValue.toString() : QString();
                if (kind == QStringLiteral("auto"))
                    importKind = Timeline::ImportMediaKind::Auto;
                else if (kind == QStringLiteral("video"))
                    importKind = Timeline::ImportMediaKind::VideoOnly;
                else if (kind == QStringLiteral("audio"))
                    importKind = Timeline::ImportMediaKind::AudioOnly;
                else
                    return setError(err, QStringLiteral("kind must be auto, video or audio")),
                           QJsonObject();
            }
            int trackIndex = 0;
            if (!nonNegativeInteger(args, QStringLiteral("trackIndex"), 0,
                                    &trackIndex, err))
                return {};
            double startSec = -1.0;
            if (args.contains(QStringLiteral("startSec"))) {
                if (!requiredFiniteNumber(args, QStringLiteral("startSec"),
                                          &startSec, err))
                    return {};
                if (startSec < 0.0)
                    return setError(err, QStringLiteral("startSec must be non-negative")),
                           QJsonObject();
            }
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            Timeline::MediaImportResult importResult;
            if (!timeline()->importMedia(filePath, trackIndex, startSec,
                                         &importResult, err, importKind))
                return {};

            QJsonArray addedClips;
            const auto appendClip = [&addedClips](const QString &kind,
                                                   int track,
                                                   int index,
                                                   double start,
                                                   double duration) {
                if (track < 0 || index < 0)
                    return;
                addedClips.append(QJsonObject{
                    {QStringLiteral("kind"), kind},
                    {QStringLiteral("trackIndex"), track},
                    {QStringLiteral("clipIndex"), index},
                    {QStringLiteral("startSec"), start},
                    {QStringLiteral("durationSec"), duration}
                });
            };
            appendClip(QStringLiteral("video"), importResult.videoTrackIndex,
                       importResult.videoClipIndex, importResult.videoStartSec,
                       importResult.durationSec);
            appendClip(QStringLiteral("audio"), importResult.audioTrackIndex,
                       importResult.audioClipIndex, importResult.audioStartSec,
                       importResult.durationSec);

            m_window->hideWelcomeScreen();
            m_window->setWindowModified(true);
            m_window->updateStatusInfo();
            m_window->updateEditActions();
            QJsonObject response{
                {QStringLiteral("ok"), true},
                {QStringLiteral("clips"), addedClips}
            };
            if (!addedClips.isEmpty()) {
                const QJsonObject first = addedClips.first().toObject();
                response.insert(QStringLiteral("kind"), first.value(QStringLiteral("kind")));
                response.insert(QStringLiteral("trackIndex"),
                                first.value(QStringLiteral("trackIndex")));
                response.insert(QStringLiteral("clipIndex"),
                                first.value(QStringLiteral("clipIndex")));
                response.insert(QStringLiteral("startSec"),
                                first.value(QStringLiteral("startSec")));
                response.insert(QStringLiteral("durationSec"),
                                first.value(QStringLiteral("durationSec")));
            }
            return response;
        })
    }, importMediaOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("save_project"),
        QStringLiteral("Saves the project to the specified path. If path is omitted, overwrites the existing save location; returns an error for unsaved projects. No dialog is opened."),
        objectSchema(QJsonObject{
            {QStringLiteral("path"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }}
        }),
        guardedWrite(QStringLiteral("save_project"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("path")}, err))
                return {};
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            QString path = m_window->m_projectFilePath;
            if (args.contains(QStringLiteral("path"))) {
                if (!requiredString(args, QStringLiteral("path"), &path, err))
                    return {};
            }
            if (path.trimmed().isEmpty())
                return setError(err, QStringLiteral("Please specify a destination path")),
                       QJsonObject();

            QString saveError;
            if (!m_window->saveProjectToPath(path, &saveError))
                return setError(err, saveError), QJsonObject();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("path"), m_window->m_projectFilePath}
            };
        })
    }, saveProjectOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("open_project"),
        QStringLiteral("Loads the project at the specified path. No dialog and no unsaved-changes confirmation."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("path"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }}
        }, {QStringLiteral("path")}),
        guardedWrite(QStringLiteral("open_project"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("path")}, err))
                return {};
            QString path;
            if (!requiredString(args, QStringLiteral("path"), &path, err))
                return {};
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            QString openError;
            if (!m_window->openProjectFromPath(
                    path, &openError, /*promptForMissingMedia=*/false))
                return setError(err, openError), QJsonObject();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("path"), m_window->m_projectFilePath}
            };
        })
    }, openProjectOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_track_locked"),
        QStringLiteral("Sets the edit lock for the specified track. While locked, clip edits such as split_clip / delete_clip / move_clip are refused. Lock changes are not undoable."),
        schemaWithRequired(mergedProperties(trackSelectorProperties(), QJsonObject{
            {QStringLiteral("locked"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("description"),
                 QStringLiteral("true to lock, false to unlock")}
            }}
        }), {QStringLiteral("kind"), QStringLiteral("trackIndex"),
             QStringLiteral("locked")}),
        guardedWrite(QStringLiteral("set_track_locked"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"),
                                         QStringLiteral("trackIndex"),
                                         QStringLiteral("locked")}, err)) {
                return {};
            }
            const QJsonValue lockedValue = args.value(QStringLiteral("locked"));
            if (!lockedValue.isBool())
                return setError(err, QStringLiteral("locked must be a boolean")),
                       QJsonObject();

            TrackTarget target;
            Timeline* currentTimeline = timeline();
            if (!readTrackTarget(args, m_window, currentTimeline, &target, err))
                return {};
            const bool locked = lockedValue.toBool();
            const bool changed = target.track->isLocked() != locked;
            if (!currentTimeline->setTrackLocked(target.kind, target.trackIndex,
                                                 locked)) {
                return setError(err, QStringLiteral("track index is out of range")),
                       QJsonObject();
            }
            if (changed)
                m_window->setWindowModified(true);
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), target.kindName},
                {QStringLiteral("trackIndex"), target.trackIndex},
                {QStringLiteral("locked"), locked}
            };
        })
    }, setTrackLockedOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("select_clip"),
        QStringLiteral("Selects the specified clip. kind / trackIndex default to video track 0 (same default as other clip tools). clipIndex is the get_timeline index. Updates the selection state of both Timeline and MainWindow using the same rules as a GUI click."),
        schemaWithRequired(clipProperties, {QStringLiteral("clipIndex")}),
        guardedWrite(QStringLiteral("select_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"),
                                         QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex")}, err))
                return {};
            QString kind = QStringLiteral("video");
            if (args.contains(QStringLiteral("kind"))) {
                if (!args.value(QStringLiteral("kind")).isString())
                    return setError(err, QStringLiteral("kind must be video or audio")),
                           QJsonObject();
                kind = args.value(QStringLiteral("kind")).toString();
            }
            if (kind != QStringLiteral("video") && kind != QStringLiteral("audio"))
                return setError(err, QStringLiteral("kind must be video or audio")),
                       QJsonObject();
            if (!args.contains(QStringLiteral("clipIndex")))
                return setError(err, QStringLiteral("clipIndex is required")),
                       QJsonObject();
            int trackIndex = 0;
            int clipIndex = 0;
            if (!nonNegativeInteger(args, QStringLiteral("trackIndex"), 0,
                                    &trackIndex, err)
                || !nonNegativeInteger(args, QStringLiteral("clipIndex"), 0,
                                       &clipIndex, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            const bool audio = kind == QStringLiteral("audio");
            if (!timeline()->selectClipByIndex(audio, trackIndex, clipIndex, err))
                return {};

            // Timeline の通知が同値選択で省略されても MainWindow の追跡値を
            // 必ず同期させ、selectedVideoClipRef() が同じ対象を返すようにする。
            m_window->m_selectedVideoTrackIndex = audio ? -1 : trackIndex;
            m_window->m_selectedVideoClipIndexTracked = clipIndex;
            m_window->updateEditActions();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("trackIndex"), trackIndex},
                {QStringLiteral("clipIndex"), clipIndex}
            };
        })
    }, selectClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("clear_selection"),
        QStringLiteral("Clears all selection on the timeline."),
        objectSchema(),
        guardedWrite(QStringLiteral("clear_selection"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            timeline()->clearSelection();
            m_window->m_selectedVideoTrackIndex = -1;
            m_window->m_selectedVideoClipIndexTracked = -1;
            m_window->updateEditActions();
            return QJsonObject{{QStringLiteral("ok"), true}};
        })
    }, clearSelectionOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("run_command"),
        QStringLiteral("Runs a favorite-registerable command by id. Returns the risk level; blocking commands only run when allowBlocking:true. quit commands can never run from MCP. Timeline-modifying operations can only be reverted with Ctrl+Z / the undo tool if the command itself recorded undo; judge by undoRecorded in the response (commands that open dialogs may report false at response time)."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("allowBlocking"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("default"), false}
            }}
        }, {QStringLiteral("id")}),
        guardedWrite(QStringLiteral("run_command"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("id"),
                                         QStringLiteral("allowBlocking")}, err))
                return {};
            QString id;
            if (!requiredString(args, QStringLiteral("id"), &id, err))
                return {};
            bool allowBlocking = false;
            if (args.contains(QStringLiteral("allowBlocking"))) {
                const QJsonValue allowBlockingValue =
                    args.value(QStringLiteral("allowBlocking"));
                if (!allowBlockingValue.isBool()) {
                    setError(err, QStringLiteral("Please specify allowBlocking as a boolean"));
                    return {};
                }
                allowBlocking = allowBlockingValue.toBool();
            }
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            for (const auto& command : m_window->m_favoritableActions) {
                if (command.id != id)
                    continue;

                const QString risk = actionRiskToString(command.risk);
                // 危険度の拒否は enabled 判定より先に行う。終了は常に拒否し、
                // Blocking はユーザーが明示的に許可した場合だけ QAction を trigger する。
                if (command.risk == FavoritableActionRisk::Quit) {
                    setError(err, QStringLiteral("This command quits the editor and cannot be run via MCP."));
                    return {};
                }
                if (command.risk == FavoritableActionRisk::Blocking && !allowBlocking) {
                    setError(err, QStringLiteral("This command opens a modal dialog, so it is not run by default. The user must operate it on screen. If you must run it anyway, specify allowBlocking:true."));
                    return {};
                }
                if (!command.action || !command.action->isEnabled()) {
                    if (err)
                        *err = QStringLiteral("command is disabled: %1").arg(id);
                    return {};
                }
                UndoManager* undoManager = timeline()
                    ? timeline()->undoManager() : nullptr;
                // saveSerial は MAX_UNDO で先頭が落ちても増えるので、長い編集
                // セッションでも「この操作で undo が積まれたか」を正しく判定できる。
                const quint64 undoSerialBefore = undoManager
                    ? undoManager->saveSerial() : 0;
                // The QAction owns its own undo policy. Saving here would
                // double-stack actions that already save an undo state.
                command.action->trigger();
                const quint64 undoSerialAfter = undoManager
                    ? undoManager->saveSerial() : 0;
                syncSelectionAfterEdit();
                QJsonObject response{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("label"), command.label},
                    {QStringLiteral("risk"), risk}
                };
                const bool undoRecorded = undoManager
                    && undoSerialAfter > undoSerialBefore;
                response.insert(QStringLiteral("undoRecorded"), undoRecorded);
                if (undoRecorded)
                    response.insert(QStringLiteral("undoDescription"),
                                    undoManager->undoDescription());
                return response;
            }
            setError(err, QStringLiteral("command not found: %1").arg(id));
            return {};
        })
    }, runCommandOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("split_clip"),
        QStringLiteral("Splits the specified clip into two at the given time. timeSec is the timeline absolute time (sec, not a clip-local offset). Positions within 0.05 sec of the clip start/end are refused. After splitting, the left side keeps the original clipIndex and the right side becomes clipIndex+1, shifting later clip indexes by 1. Clips with the same linkGroup (e.g. paired audio) are split at the same time. For range cuts, call split_clip(start) → split_clip(end, clipIndex+1) → delete_clip(middle, ripple:true) in order, re-checking indexes with get_timeline after each operation. kind/trackIndex default to video track 0. clipIndex is the get_timeline index. Destructive timeline operation, revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("Split position. Timeline absolute time (sec), not clip-relative. Positions within 0.05 sec of the clip start/end are refused with 'split point is outside the clip'.")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("timeSec")}),
        guardedWrite(QStringLiteral("split_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("timeSec")},
                                        err))
                return {};
            double timeSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("timeSec"), &timeSec, err))
                return {};
            if (timeSec < 0.0)
                return setError(err, QStringLiteral("timeSec must be non-negative")), QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            // 分割点の範囲判定は Timeline::splitClipByIndex が
            // splitAtPlayhead と同じ 0.05 秒マージンで行う。ここで別の閾値を
            // 持つと境界付近で判定とメッセージが食い違うので持たない。

            Timeline* currentTimeline = timeline();
            if (!currentTimeline->splitClipByIndex(
                    target.audio, target.trackIndex, target.clipIndex, timeSec, err))
                return {};
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("newClipCount"), target.track->clipCount()},
                {QStringLiteral("leftIndex"), target.clipIndex},
                {QStringLiteral("rightIndex"), target.clipIndex + 1}
            };
        })
    }, splitClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("delete_clip"),
        QStringLiteral("Deletes the specified clip, closing the gap with later clips if needed. Clips with the same linkGroup (e.g. paired audio) are deleted together. Later clip indexes shift by 1 after deletion. ripple:true closes the gap with later clips (default false). Only tracks sharing the deleted clip's linkGroup close the gap; other tracks (B-roll on V2, BGM on A2) do not shift. kind/trackIndex default to video track 0. clipIndex is the get_timeline index. Destructive timeline operation, revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("ripple"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("default"), false},
                {QStringLiteral("description"),
                 QStringLiteral("true to close the gap with later clips after deletion (default false)")}
            }}
        }), {QStringLiteral("clipIndex")}),
        guardedWrite(QStringLiteral("delete_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("ripple")},
                                        err))
                return {};
            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            bool ripple = false;
            if (args.contains(QStringLiteral("ripple"))) {
                if (!args.value(QStringLiteral("ripple")).isBool()) {
                    setError(err, QStringLiteral("ripple must be a boolean"));
                    return {};
                }
                ripple = args.value(QStringLiteral("ripple")).toBool();
            }

            Timeline* currentTimeline = timeline();
            if (!currentTimeline->deleteClipByIndex(
                    target.audio, target.trackIndex, target.clipIndex, ripple, err))
                return {};
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("remainingClipCount"), target.track->clipCount()}
            };
        })
    }, deleteClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("move_clip"),
        QStringLiteral("Moves the specified clip to the given start time. newStartSec is the timeline absolute time (sec). Moves to another track if needed and reorders even in contiguous layouts. Unplaceable requests return ok:false with the reason and the actually placeable time. kind/trackIndex default to video track 0. clipIndex is the get_timeline index. Destructive timeline operation, revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("newStartSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("Destination timeline absolute time (sec). Not a clip-local time.")}
            }},
            {QStringLiteral("newTrackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"),
                 QStringLiteral("Destination 0-based track number. Defaults to the current track")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("newStartSec")}),
        guardedWrite(QStringLiteral("move_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("newStartSec"),
                                         QStringLiteral("newTrackIndex")},
                                        err))
                return {};
            double newStartSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("newStartSec"),
                                      &newStartSec, err)) {
                return {};
            }
            if (newStartSec < 0.0)
                return setError(err, QStringLiteral("newStartSec must be non-negative")), QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            int newTrackIndex = target.trackIndex;
            if (!nonNegativeInteger(args, QStringLiteral("newTrackIndex"),
                                    target.trackIndex, &newTrackIndex, err))
                return {};
            Timeline* currentTimeline = timeline();
            Timeline::MoveClipResult moveResult;
            if (!currentTimeline->moveClipByIndex(
                    target.audio, target.trackIndex, target.clipIndex, newStartSec,
                    newTrackIndex, &moveResult, err)) {
                // 既定プロジェクトは V1/A1 の 1 段しかない。LLM がトラックを増やす
                // 手段 (run_command のトラック追加コマンド) をエラー文で案内する。
                if (err && err->contains(QStringLiteral("newTrackIndex"))) {
                    const QString needle = target.audio
                        ? QStringLiteral("Add Audio Track")
                        : QStringLiteral("Add Video Track");
                    for (const auto& command : m_window->m_favoritableActions) {
                        if (!command.label.contains(needle))
                            continue;
                        *err += QStringLiteral(". To add a track, run id \"%1\" (%2) via run_command")
                                    .arg(command.id, command.label);
                        break;
                    }
                }
                return {};
            }
            if (!moveResult.reason.isEmpty()) {
                return QJsonObject{
                    {QStringLiteral("ok"), false},
                    // 既存の move_clip 応答キーを失わないよう、失敗時も
                    // startSec は actualStartSec と同じ値で返す。
                    {QStringLiteral("startSec"), moveResult.actualStartSec},
                    {QStringLiteral("actualStartSec"), moveResult.actualStartSec},
                    {QStringLiteral("reason"), moveResult.reason},
                    {QStringLiteral("trackIndex"), newTrackIndex}
                };
            }
            if (moveResult.moved)
                m_window->setWindowModified(true);
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                // startSec は既存ツールの応答キーとして維持する。
                {QStringLiteral("startSec"), moveResult.actualStartSec},
                {QStringLiteral("actualStartSec"), moveResult.actualStartSec},
                {QStringLiteral("trackIndex"), moveResult.trackIndex},
                {QStringLiteral("clipIndex"), moveResult.clipIndex}
            };
        })
    }, moveClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("match_frame"),
        QStringLiteral("Opens the video clip at the playhead position (or timeSec) in the source monitor at the source time reflecting speed, reverse playback, and remapping. Prefers the selected video track; uses V1 if none applies."),
        objectSchema(QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"),
                 QStringLiteral("Timeline absolute time (sec). Defaults to the current playhead position")}
            }}
        }),
        guardedWrite(QStringLiteral("match_frame"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("timeSec")}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            double timeSec = timeline()->playheadPosition();
            if (args.contains(QStringLiteral("timeSec"))) {
                if (!requiredFiniteNumber(args, QStringLiteral("timeSec"),
                                          &timeSec, err)) {
                    return {};
                }
                if (timeSec < 0.0) {
                    return setError(err, QStringLiteral("timeSec must be non-negative")),
                           QJsonObject();
                }
            }

            Timeline::MatchFrameResult match;
            if (!timeline()->matchFrame(timeSec, &match, err))
                return {};

            m_window->openInSourceMonitor(match.filePath, match.sourceSec);
            return QJsonObject{
                {QStringLiteral("filePath"), match.filePath},
                {QStringLiteral("sourceSec"), match.sourceSec},
                {QStringLiteral("clipIndex"), match.clipIndex}
            };
        })
    }, matchFrameOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("replace_clip"),
        QStringLiteral("Replaces the specified clip's media with the media at filePath. Preserves position, inPoint, and length as much as possible; audio in the same linkGroup is also replaced if the new media has audio. Returns a warning if the new media is shorter. Revertible with a single Ctrl+Z / undo."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("filePath"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("Replacement media file")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("filePath")}),
        guardedWrite(QStringLiteral("replace_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(
                    args,
                    {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                     QStringLiteral("clipIndex"), QStringLiteral("filePath")},
                    err)) {
                return {};
            }

            QString filePath;
            if (!requiredString(args, QStringLiteral("filePath"), &filePath, err))
                return {};
            if (filePath.isEmpty()) {
                return setError(err, QStringLiteral("File not found: %1")
                                         .arg(filePath)),
                       QJsonObject();
            }

            ClipTarget target;
            Timeline *currentTimeline = timeline();
            if (!readClipTarget(args, m_window, currentTimeline, &target, err))
                return {};

            QString message;
            if (!currentTimeline->replaceClipMedia(
                    target.audio ? TrackKind::Audio : TrackKind::Video,
                    target.trackIndex, target.clipIndex, filePath,
                    QFileInfo(filePath).fileName(), 0.0, &message)) {
                return setError(err, message.isEmpty()
                                         ? QStringLiteral("Cannot replace the clip")
                                         : message),
                       QJsonObject();
            }

            m_window->setWindowModified(true);
            syncSelectionAfterEdit();
            QJsonObject response{{QStringLiteral("ok"), true}};
            if (!message.isEmpty())
                response.insert(QStringLiteral("warning"), message);
            return response;
        })
    }, replaceClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("relink_media"),
        QStringLiteral("Relinks missing media or LUT paths in bulk. Only changes when every mapping 'to' is an existing file; updates the active / nested sequence in a single undo."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("mapping"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("minItems"), 1},
                {QStringLiteral("items"), schemaWithRequired(QJsonObject{
                    {QStringLiteral("from"), QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("string")}
                    }},
                    {QStringLiteral("to"), QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("string")}
                    }}
                }, {QStringLiteral("from"), QStringLiteral("to")})}
            }}
        }, {QStringLiteral("mapping")}),
        guardedWrite(QStringLiteral("relink_media"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("mapping")}, err))
                return {};
            const QJsonValue mappingValue = args.value(QStringLiteral("mapping"));
            if (!mappingValue.isArray() || mappingValue.toArray().isEmpty()) {
                return setError(err, QStringLiteral("mapping must be a non-empty array")),
                       QJsonObject();
            }

            QHash<QString, QString> mapping;
            const QJsonArray entries = mappingValue.toArray();
            for (int index = 0; index < entries.size(); ++index) {
                if (!entries.at(index).isObject()) {
                    return setError(err, QStringLiteral("mapping[%1] must be an object")
                                             .arg(index)),
                           QJsonObject();
                }
                const QJsonObject entry = entries.at(index).toObject();
                if (!rejectUnknownArguments(
                        entry, {QStringLiteral("from"), QStringLiteral("to")}, err)) {
                    return {};
                }
                QString from;
                QString to;
                if (!requiredString(entry, QStringLiteral("from"), &from, err)
                    || !requiredString(entry, QStringLiteral("to"), &to, err)) {
                    return {};
                }
                if (from.isEmpty() || to.isEmpty()) {
                    return setError(err, QStringLiteral("mapping[%1] from/to must not be empty")
                                             .arg(index)),
                           QJsonObject();
                }
                mapping.insert(from, to);
            }

            Timeline *currentTimeline = timeline();
            if (!m_window || !currentTimeline)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            QString relinkError;
            if (!m_window->relinkMediaPaths(mapping, &relinkError))
                return setError(err, relinkError), QJsonObject();

            m_window->setWindowModified(true);
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("relinked"), mapping.size()}
            };
        })
    }, relinkMediaOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_clip_property"),
        QStringLiteral("Sets the specified clip's properties. property and valid ranges: volume 0..2, opacity 0..1, speed 0.25..4, pan -1..1, videoScale 0.1..10, reversed true/false, autoOrient true/false. speed and reversed also apply simultaneously to video and audio in the same linkGroup (linkedApplied in the response). Check current values with get_timeline; revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("property"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                // enum を出しておくと LLM が存在しないプロパティ名を投げてこない。
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("volume"), QStringLiteral("opacity"),
                    QStringLiteral("speed"), QStringLiteral("pan"),
                    QStringLiteral("videoScale"), QStringLiteral("reversed"),
                    QStringLiteral("autoOrient")
                }},
                {QStringLiteral("description"),
                 QStringLiteral("Target. volume / opacity / speed / pan / videoScale / reversed / autoOrient")}
            }},
            {QStringLiteral("value"), QJsonObject{
                {QStringLiteral("oneOf"), QJsonArray{
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}},
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}
                }},
                {QStringLiteral("description"),
                 QStringLiteral("Value for the property. reversed / autoOrient are boolean, others are number.")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("property"), QStringLiteral("value")}),
        guardedWrite(QStringLiteral("set_clip_property"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("property"),
                                         QStringLiteral("value")},
                                        err))
                return {};
            QString property;
            if (!requiredString(args, QStringLiteral("property"), &property, err))
                return {};
            const bool isReversedProperty = property == QStringLiteral("reversed");
            const bool isAutoOrientProperty = property == QStringLiteral("autoOrient");
            const bool isBooleanProperty = isReversedProperty || isAutoOrientProperty;
            double value = 0.0;
            bool booleanValue = false;
            if (isBooleanProperty) {
                const QJsonValue rawValue = args.value(QStringLiteral("value"));
                if (!rawValue.isBool()) {
                    return setError(
                               err,
                               QStringLiteral("value must be a boolean for %1").arg(property)),
                           QJsonObject();
                }
                booleanValue = rawValue.toBool();
            } else if (!requiredFiniteNumber(args, QStringLiteral("value"), &value, err)) {
                return {};
            }

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};

            Timeline* currentTimeline = timeline();
            // speed と reversed はリンクした映像・音声を同じ再生方向/尺に
            // 保つ。他のプロパティは指定クリップだけに適用する。
            const bool applyToLinked = property == QStringLiteral("speed")
                || isReversedProperty;
            if (isReversedProperty) {
                if (!currentTimeline->setClipReversed(
                        target.audio ? TrackKind::Audio : TrackKind::Video,
                        target.trackIndex, target.clipIndex,
                        booleanValue, /*applyToLinked=*/true)) {
                    return setError(err, QStringLiteral("clip reverse update failed")),
                           QJsonObject();
                }
            } else if (isAutoOrientProperty) {
                if (!currentTimeline->setClipAutoOrientEnabled(
                        target.audio ? TrackKind::Audio : TrackKind::Video,
                        target.trackIndex, target.clipIndex, booleanValue)) {
                    return setError(err, QStringLiteral("clip auto-orient update failed")),
                           QJsonObject();
                }
            } else if (!currentTimeline->setClipPropertyByIndex(
                           target.audio, target.trackIndex, target.clipIndex,
                           property, value, err, applyToLinked)) {
                return {};
            }
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("property"), property},
                {QStringLiteral("value"), isBooleanProperty
                    ? QJsonValue(booleanValue) : QJsonValue(value)},
                {QStringLiteral("linkedApplied"), applyToLinked}
            };
        })
    }, setClipPropertyOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("music_remix"),
        QStringLiteral("Reconstructs the audio clip in segments at beat boundaries and auto-adjusts to the target duration. kind is audio only. Media with fewer than 2 beatTimes is left unchanged and treated as an error. Revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("kind"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("audio")}}
            }},
            {QStringLiteral("trackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}
            }},
            {QStringLiteral("clipIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}
            }},
            {QStringLiteral("targetSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("exclusiveMinimum"), 0.0},
                {QStringLiteral("maximum"), remix::kMaxTargetSec}
            }}
        }, {QStringLiteral("kind"), QStringLiteral("trackIndex"),
            QStringLiteral("clipIndex"), QStringLiteral("targetSec")}),
        guardedWrite(QStringLiteral("music_remix"),
                     [this](const QJsonObject &args, QString *err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("targetSec")},
                                        err)) {
                return {};
            }
            if (args.value(QStringLiteral("kind")).toString()
                    != QStringLiteral("audio")) {
                return setError(err, QStringLiteral("music_remix supports audio only")),
                       QJsonObject();
            }
            double targetSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("targetSec"),
                                      &targetSec, err)) {
                return {};
            }
            if (targetSec <= 0.0)
                return setError(err, QStringLiteral("Please specify targetSec as a finite value greater than 0")),
                       QJsonObject();
            if (targetSec > remix::kMaxTargetSec)
                return setError(err, QStringLiteral("Please specify targetSec as 86400 seconds or less")),
                       QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            if (!target.audio)
                return setError(err, QStringLiteral("music_remix supports audio only")),
                       QJsonObject();

            QVector<double> beatTimes;
            QString detectionError;
            if (!detectMusicRemixBeats(target.track->clips().at(target.clipIndex),
                                       &beatTimes, nullptr, &detectionError)) {
                return setError(err, detectionError), QJsonObject();
            }
            const ClipInfo &clip = target.track->clips().at(target.clipIndex);
            const remix::Plan plan = remix::planRemix(
                beatTimes, clip.effectiveDuration(), targetSec, remix::Config{});
            if (!plan.valid)
                return setError(err, plan.error), QJsonObject();
            QString applyError;
            if (!timeline()->applyMusicRemix(target.trackIndex, target.clipIndex,
                                             plan, false, &applyError)) {
                return setError(err, applyError), QJsonObject();
            }
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), QStringLiteral("audio")},
                {QStringLiteral("trackIndex"), target.trackIndex},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("targetSec"), targetSec},
                {QStringLiteral("resultDuration"), plan.resultDuration},
                {QStringLiteral("segmentCount"), plan.segments.size()}
            };
        })
    }, musicRemixOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("dialogue_level"),
        QStringLiteral("Analyzes the audio clip's short-term loudness and generates a volume envelope that levels dialogue volume. kind is audio only. Revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("kind"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("audio")}}
            }},
            {QStringLiteral("trackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}
            }},
            {QStringLiteral("clipIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}
            }},
            {QStringLiteral("targetLufs"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("default"), -18.0}
            }}
        }, {QStringLiteral("kind"), QStringLiteral("trackIndex"),
            QStringLiteral("clipIndex")}),
        guardedWrite(QStringLiteral("dialogue_level"),
                     [this](const QJsonObject &args, QString *err) -> QJsonObject {
            if (!rejectUnknownArguments(
                    args, {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                           QStringLiteral("clipIndex"), QStringLiteral("targetLufs")},
                    err)) {
                return {};
            }
            if (args.value(QStringLiteral("kind")).toString()
                    != QStringLiteral("audio")) {
                return setError(
                           err, QStringLiteral("dialogue_level supports audio only")),
                       QJsonObject();
            }

            leveler::Config config;
            if (args.contains(QStringLiteral("targetLufs"))) {
                if (!requiredFiniteNumber(args, QStringLiteral("targetLufs"),
                                          &config.targetShortTermLufs, err)) {
                    return {};
                }
            }

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            if (!target.audio) {
                return setError(
                           err, QStringLiteral("dialogue_level supports audio only")),
                       QJsonObject();
            }

            leveler::Analysis analysis;
            if (!analyzeDialogueClip(target.track->clips().at(target.clipIndex),
                                     config, &analysis, err)) {
                return {};
            }
            QString applyError;
            if (!timeline()->applyDialogueLevel(
                    target.trackIndex, target.clipIndex,
                    analysis.envelope, &applyError)) {
                return setError(err, applyError), QJsonObject();
            }
            syncSelectionAfterEdit();
            const double measuredMin = analysis.hasMeasuredLufs
                ? analysis.minMeasuredLufs : -70.0;
            const double measuredMax = analysis.hasMeasuredLufs
                ? analysis.maxMeasuredLufs : -70.0;
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), QStringLiteral("audio")},
                {QStringLiteral("trackIndex"), target.trackIndex},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("targetLufs"), config.targetShortTermLufs},
                {QStringLiteral("pointCount"), analysis.envelope.size()},
                {QStringLiteral("measuredLufsMin"), measuredMin},
                {QStringLiteral("measuredLufsMax"), measuredMax}
            };
        })
    }, dialogueLevelOutputSchema));

    QJsonObject dynamicZoomInputSchema = schemaWithRequired(
        mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("preset"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("zoomIn"), QStringLiteral("zoomOut"),
                    QStringLiteral("panLeft"), QStringLiteral("panRight"),
                    QStringLiteral("panUp"), QStringLiteral("panDown")
                }},
                {QStringLiteral("description"),
                 QStringLiteral("Preset. Cannot be specified together with start/end")}
            }},
            {QStringLiteral("start"), dynamicZoomRectSchema()},
            {QStringLiteral("end"), dynamicZoomRectSchema()},
            {QStringLiteral("easing"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("linear"), QStringLiteral("easeInOut")
                }},
                {QStringLiteral("default"), QStringLiteral("easeInOut")}
            }}
        }), {QStringLiteral("clipIndex")});
    dynamicZoomInputSchema.insert(
        QStringLiteral("description"),
        QStringLiteral("Specify either preset or start/end. Specifying both is an error. h for start/end is optional and ignored if specified; fixed to the canvas aspect ratio"));
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("dynamic_zoom"),
        QStringLiteral("Applies dynamic zoom to the specified video clip, generating start/end keyframes for position and scale. Specify either preset or start/end; specifying both is an error. h for start/end is optional and ignored if specified; fixed to the canvas aspect ratio. Revertible with a single undo."),
        dynamicZoomInputSchema,
        guardedWrite(QStringLiteral("dynamic_zoom"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(
                    args,
                    {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                     QStringLiteral("clipIndex"), QStringLiteral("preset"),
                     QStringLiteral("start"), QStringLiteral("end"),
                     QStringLiteral("easing")}, err)) {
                return {};
            }

            const bool hasPreset = args.contains(QStringLiteral("preset"));
            const bool hasStart = args.contains(QStringLiteral("start"));
            const bool hasEnd = args.contains(QStringLiteral("end"));
            if (hasPreset && (hasStart || hasEnd)) {
                return setError(err, QStringLiteral("preset and start/end cannot be combined")),
                       QJsonObject();
            }
            if (!hasPreset && !(hasStart && hasEnd)) {
                return setError(err, QStringLiteral("preset or both start and end are required")),
                       QJsonObject();
            }
            if (!hasPreset && hasStart != hasEnd) {
                return setError(err, QStringLiteral("start and end must be specified together")),
                       QJsonObject();
            }
            const bool ignoredHeight = !hasPreset
                && (args.value(QStringLiteral("start")).toObject().contains(
                        QStringLiteral("h"))
                    || args.value(QStringLiteral("end")).toObject().contains(
                        QStringLiteral("h")));

            dynzoom::Rect start;
            dynzoom::Rect end;
            if (hasPreset) {
                QString preset;
                if (!requiredString(args, QStringLiteral("preset"), &preset, err))
                    return {};
                if (!dynamicZoomPresetFrames(preset, &start, &end)) {
                    return setError(err, QStringLiteral("preset is not a valid dynamic zoom preset")),
                           QJsonObject();
                }
            } else if (!readDynamicZoomRect(args, QStringLiteral("start"), &start, err)
                       || !readDynamicZoomRect(args, QStringLiteral("end"), &end, err)) {
                return {};
            }

            dynzoom::Easing easing = dynzoom::Easing::EaseInOut;
            if (args.contains(QStringLiteral("easing"))) {
                QString easingName;
                if (!requiredString(args, QStringLiteral("easing"), &easingName, err))
                    return {};
                if (easingName == QStringLiteral("linear"))
                    easing = dynzoom::Easing::Linear;
                else if (easingName != QStringLiteral("easeInOut"))
                    return setError(err, QStringLiteral("easing must be linear or easeInOut")),
                           QJsonObject();
            }

            ClipTarget target;
            Timeline *currentTimeline = timeline();
            if (!readClipTarget(args, m_window, currentTimeline, &target, err))
                return {};
            if (target.audio)
                return setError(err, QStringLiteral("dynamic_zoom supports video clips only")),
                       QJsonObject();
            if (target.track->isLocked())
                return setError(err, QStringLiteral("track is locked")), QJsonObject();
            if (!currentTimeline->applyDynamicZoom(
                    TrackKind::Video, target.trackIndex, target.clipIndex,
                    start, end, easing)) {
                return setError(err, QStringLiteral("dynamic zoom update failed")),
                       QJsonObject();
            }

            m_window->setWindowModified(true);
            syncSelectionAfterEdit();
            const KeyframeManager& keyframes =
                target.track->clips().at(target.clipIndex).keyframes;
            const auto trackCount = [&keyframes](const QString& property) {
                const KeyframeTrack *track = keyframes.track(property);
                return track ? track->count() : 0;
            };
            const QJsonObject counts{
                {QStringLiteral("positionX"), trackCount(QStringLiteral("positionX"))},
                {QStringLiteral("positionY"), trackCount(QStringLiteral("positionY"))},
                {QStringLiteral("scaleX"), trackCount(QStringLiteral("scaleX"))},
                {QStringLiteral("scaleY"), trackCount(QStringLiteral("scaleY"))}
            };
            QJsonObject response{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), target.trackIndex},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("keyframeCount"),
                 counts.value(QStringLiteral("positionX")).toInt()
                     + counts.value(QStringLiteral("positionY")).toInt()
                     + counts.value(QStringLiteral("scaleX")).toInt()
                     + counts.value(QStringLiteral("scaleY")).toInt()},
                {QStringLiteral("keyframeCounts"), counts}
            };
            if (ignoredHeight) {
                response.insert(
                    QStringLiteral("warning"),
                    QStringLiteral("Ignored the specified h value since h is fixed to the canvas aspect ratio"));
            }
            return response;
        })
    }, dynamicZoomOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_clip_label"),
        QStringLiteral("Sets the specified clip's label color. label is one of none / red / orange / yellow / green / cyan / blue / purple / pink. kind/trackIndex default to video track 0. clipIndex is the get_timeline index. Check the new value with get_timeline's label; revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("label"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), clipLabelEnum()},
                {QStringLiteral("description"),
                 QStringLiteral("Label color. none to clear")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("label")}),
        guardedWrite(QStringLiteral("set_clip_label"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("label")},
                                        err)) {
                return {};
            }
            QString labelId;
            if (!requiredString(args, QStringLiteral("label"), &labelId, err))
                return {};
            ClipLabel label = ClipLabel::None;
            if (!parseClipLabel(labelId, &label)) {
                return setError(err,
                                QStringLiteral("label must be one of: %1")
                                    .arg(clipLabelIds().join(QStringLiteral(", ")))),
                       QJsonObject();
            }

            ClipTarget target;
            Timeline* currentTimeline = timeline();
            if (!readClipTarget(args, m_window, currentTimeline, &target, err))
                return {};
            if (target.track->isLocked())
                return setError(err, QStringLiteral("track is locked")), QJsonObject();

            const TrackKind kind = target.audio ? TrackKind::Audio : TrackKind::Video;
            if (!currentTimeline->setClipLabel(kind, target.trackIndex,
                                               target.clipIndex, label)) {
                return setError(err, QStringLiteral("clip label could not be set")),
                       QJsonObject();
            }
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), target.audio
                    ? QStringLiteral("audio") : QStringLiteral("video")},
                {QStringLiteral("trackIndex"), target.trackIndex},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("label"), clipLabelToString(label)}
            };
        })
    }, setClipLabelOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("trim_clip"),
        QStringLiteral("Trims the specified video clip at the timeline absolute time (sec) timeSec. edge=in keeps the clip start position and makes the content at timeSec the new head, closing the following part leftward by (timeSec − start) (RippleIn). edge=out sets the tail to timeSec and closes the gap with following clips (RippleOut). kind supports video only; audio clips in the same linkGroup (e.g. A1) are trimmed by the same amount to stay in sync with video. ripple defaults to true. ripple:false is refused since the current trim engine has no non-ripple types. Destructive timeline operation, revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("edge"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("in"), QStringLiteral("out")
                }},
                {QStringLiteral("description"),
                 QStringLiteral("Edge to trim. in is the head, out is the tail")}
            }},
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"),
                 QStringLiteral("Target position. Timeline absolute time (sec), not clip-relative time")}
            }},
            {QStringLiteral("ripple"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("default"), true},
                {QStringLiteral("description"),
                 QStringLiteral("Ripple that closes the gap with following clips. Default true. false is currently unsupported")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("edge"),
            QStringLiteral("timeSec")}),
        guardedWrite(QStringLiteral("trim_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("edge"),
                                         QStringLiteral("timeSec"), QStringLiteral("ripple")},
                                        err))
                return {};
            QString edge;
            if (!requiredString(args, QStringLiteral("edge"), &edge, err))
                return {};
            if (edge != QStringLiteral("in") && edge != QStringLiteral("out"))
                return setError(err, QStringLiteral("edge must be in or out")), QJsonObject();

            double timeSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("timeSec"), &timeSec, err))
                return {};
            if (timeSec < 0.0)
                return setError(err, QStringLiteral("timeSec must be non-negative")), QJsonObject();

            bool ripple = true;
            if (args.contains(QStringLiteral("ripple"))) {
                if (!args.value(QStringLiteral("ripple")).isBool())
                    return setError(err, QStringLiteral("ripple must be a boolean")), QJsonObject();
                ripple = args.value(QStringLiteral("ripple")).toBool();
            }
            if (!ripple)
                return setError(err, QStringLiteral("ripple:false is not supported by the trim engine")),
                       QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            if (target.audio)
                return setError(err, QStringLiteral("trim_clip supports video clips only")), QJsonObject();
            if (target.track->isLocked())
                return setError(err, QStringLiteral("track is locked")), QJsonObject();

            const trimops::TrimType trimType = edge == QStringLiteral("in")
                ? trimops::TrimType::RippleIn : trimops::TrimType::RippleOut;
            const double deltaSec = timeSec
                - (edge == QStringLiteral("in") ? target.startSec : target.endSec);
            Timeline* currentTimeline = timeline();
            if (!currentTimeline->applyTrimLinked(target.track, target.clipIndex,
                                                  trimType, deltaSec, err))
                return {};
            // Timeline::applyTrimLinked (TimelineTrack::applyTrim) emits modified()
            // but deliberately does not push an undo state; keep this MCP
            // operation (video + linked audio) as one undo step.
            currentTimeline->saveUndoState(QStringLiteral("Trim"));
            syncSelectionAfterEdit();

            const ClipInfo& trimmed = target.track->clips().at(target.clipIndex);
            const double newStartSec = target.startSec;
            const double newEndSec = newStartSec + trimmed.effectiveDuration();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), target.trackIndex},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("edge"), edge},
                {QStringLiteral("ripple"), ripple},
                {QStringLiteral("startSec"), newStartSec},
                {QStringLiteral("endSec"), newEndSec}
            };
        })
    }, trimClipOutputSchema));

    for (bool decompose : {false, true}) {
        const QString name = decompose ? QStringLiteral("decompose_render_in_place")
                                       : QStringLiteral("render_in_place");
        QJsonObject properties = clipProperties;
        properties.remove(QStringLiteral("kind"));
        if (!decompose) {
            properties.insert(QStringLiteral("kind"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("video")}}});
            properties.insert(QStringLiteral("codec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("h264"), QStringLiteral("prores")}}});
            properties.insert(QStringLiteral("handlesSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.0}, {QStringLiteral("maximum"), 5.0}});
        }
        QJsonObject outputProperties{
            {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}};
        QStringList requiredOutput{QStringLiteral("ok")};
        if (!decompose) {
            outputProperties.insert(QStringLiteral("outputPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
            for (const QString &key : {QStringLiteral("replaced"), QStringLiteral("linkedAudioReplaced")})
                outputProperties.insert(key, QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}});
            requiredOutput << QStringLiteral("outputPath") << QStringLiteral("replaced") << QStringLiteral("linkedAudioReplaced");
        }
        m_registry->registerTool(withOutputSchema({name,
            decompose ? QStringLiteral("Restores the pre-bake video and linked audio in a single operation. Revertible.")
                      : QStringLiteral("Bakes effects into the video and linked audio, then replaces them. Waits for completion. Revertible."),
            schemaWithRequired(properties, decompose
                ? QStringList{QStringLiteral("trackIndex"), QStringLiteral("clipIndex")}
                : QStringList{QStringLiteral("kind"), QStringLiteral("trackIndex"), QStringLiteral("clipIndex")}),
            guardedWrite(name, [this, decompose](const QJsonObject &args, QString *err) -> QJsonObject {
                const QStringList allowed = decompose
                    ? QStringList{QStringLiteral("trackIndex"), QStringLiteral("clipIndex")}
                    : QStringList{QStringLiteral("kind"), QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
                                  QStringLiteral("codec"), QStringLiteral("handlesSec")};
                if (!rejectUnknownArguments(args, allowed, err)) return {};
                if (!args.contains(QStringLiteral("trackIndex")))
                    return setError(err, QStringLiteral("trackIndex is required")), QJsonObject();
                if (!decompose && args.value(QStringLiteral("kind")).toString() != QStringLiteral("video"))
                    return setError(err, QStringLiteral("Please specify video for kind")), QJsonObject();
                ClipTarget target;
                if (!readClipTarget(args, m_window, timeline(), &target, err)) return {};
                if (decompose) {
                    if (!renderinplace::decomposeRenderInPlace(*timeline(), target.trackIndex, target.clipIndex))
                        return setError(err, QStringLiteral("Cannot restore the original clip")), QJsonObject();
                    syncSelectionAfterEdit();
                    return QJsonObject{{QStringLiteral("ok"), true}};
                }
                renderinplace::Options options;
                options.projectFilePath = m_window->m_projectFilePath;
                options.fps = qMax(1, m_window->m_projectConfig.fps);
                if (args.contains(QStringLiteral("codec"))
                    && !requiredString(args, QStringLiteral("codec"), &options.codec, err)) return {};
                if (args.contains(QStringLiteral("handlesSec"))
                    && !finiteNumberForMcp(args, QStringLiteral("handlesSec"), &options.handlesSec, err)) return {};
                QString path;
                if (!renderinplace::renderClipInPlace(*timeline(), target.trackIndex, target.clipIndex, options, &path, err))
                    return {};
                bool linkedAudio = false;
                for (const auto *track : timeline()->audioTracks())
                    for (const auto &clip : track->clips())
                        linkedAudio |= clip.filePath == path && bool(clip.renderInPlaceOriginal);
                syncSelectionAfterEdit();
                return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("outputPath"), path},
                    {QStringLiteral("replaced"), true}, {QStringLiteral("linkedAudioReplaced"), linkedAudio}};
            })
        }, outputSchemaOf(outputProperties, requiredOutput)));
    }

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_transition"),
        QStringLiteral("video sets a transition on the V1 clip; audio sets one on the clip of the specified audio track. video's type is a TransitionType identifier; None clears it. For audio, only CrossDissolve (adjacent crossfade with constant power), FadeIn, and FadeOut are allowed; other types including None are an error. Audio changes are not mirrored to the video side. durationSec is in seconds, default 0.5, range 0.1..5.0. alignment is Center / Start / End (default Center); easing is Linear / EaseIn / EaseOut / EaseInOut (default Linear). Destructive timeline operation, revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("type"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), transitionTypeEnum()},
                {QStringLiteral("description"),
                 QStringLiteral("TransitionType identifier. None clears it for video. For kind=audio, only CrossDissolve / FadeIn / FadeOut are allowed")}
            }},
            {QStringLiteral("alignment"), transitionIdentifierSchema(transitionAlignmentNames())},
            {QStringLiteral("easing"), transitionIdentifierSchema(transitionEasingNames())},
            {QStringLiteral("softness"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.0}, {QStringLiteral("maximum"), 1.0}}},
            {QStringLiteral("borderWidth"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.0}, {QStringLiteral("maximum"), 50.0}}},
            {QStringLiteral("borderColor"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("pattern"), QStringLiteral("^#[0-9a-fA-F]{6}$")}}},
            {QStringLiteral("durationSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.1},
                {QStringLiteral("maximum"), 5.0},
                {QStringLiteral("default"), 0.5},
                {QStringLiteral("description"),
                 QStringLiteral("Transition duration (sec). 0.1..5.0, default 0.5. Ignored for None")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("type")}),
        guardedWrite(QStringLiteral("set_transition"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("type"),
                                         QStringLiteral("durationSec"), QStringLiteral("alignment"),
                                         QStringLiteral("easing"), QStringLiteral("softness"),
                                         QStringLiteral("borderWidth"), QStringLiteral("borderColor")},
                                        err))
                return {};
            QString typeName;
            if (!requiredString(args, QStringLiteral("type"), &typeName, err))
                return {};
            TransitionType type = TransitionType::None;
            if (!transitionTypeFromName(typeName, &type))
                return setError(err, QStringLiteral("type is not a valid TransitionType identifier")),
                       QJsonObject();

            int alignmentIndex = 0;
            int easingIndex = 0;
            const auto parseIdentifier = [&](const QString& key, const QStringList& names,
                                              int* index) {
                if (!args.contains(key)) return true;
                QString name;
                if (!requiredString(args, key, &name, err)) return false;
                *index = names.indexOf(name);
                return *index >= 0 || setError(err, key + QStringLiteral(" identifier is invalid"));
            };
            if (!parseIdentifier(QStringLiteral("alignment"), transitionAlignmentNames(), &alignmentIndex)
                || !parseIdentifier(QStringLiteral("easing"), transitionEasingNames(), &easingIndex))
                return {};
            const auto alignment = static_cast<TransitionAlignment>(alignmentIndex);
            const auto easing = static_cast<TransitionEasing>(easingIndex);
            double softness = 0.0, borderWidth = 0.0;
            if (args.contains(QStringLiteral("softness"))
                && !finiteNumberForMcp(args, QStringLiteral("softness"), &softness, err)) return {};
            if (args.contains(QStringLiteral("borderWidth"))
                && !finiteNumberForMcp(args, QStringLiteral("borderWidth"), &borderWidth, err)) return {};
            if (softness < 0.0 || softness > 1.0 || borderWidth < 0.0 || borderWidth > 50.0)
                return setError(err, QStringLiteral("Please specify softness as 0..1 and border width as 0..50")), QJsonObject();
            QColor borderColor = Qt::white;
            if (args.contains(QStringLiteral("borderColor"))) {
                QString color;
                if (!requiredString(args, QStringLiteral("borderColor"), &color, err)) return {};
                bool valid = color.size() == 7 && color.startsWith(QLatin1Char('#'));
                for (int i = 1; i < color.size(); ++i) {
                    const ushort c = color.at(i).unicode();
                    valid = valid && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
                }
                if (!valid) return setError(err, QStringLiteral("Please specify the border color as #RRGGBB")), QJsonObject();
                borderColor = QColor(color);
            }
            const bool ignoredEdges = !supportsEdgeParams(type)
                && (args.contains(QStringLiteral("softness")) || args.contains(QStringLiteral("borderWidth"))
                    || args.contains(QStringLiteral("borderColor")));
            const QString edgeWarning = args.contains(QStringLiteral("softness"))
                ? QStringLiteral("Softness is ignored for this type")
                : QStringLiteral("Borders are ignored for this type");

            double durationSec = 0.5;
            if (!positiveFiniteNumber(args, QStringLiteral("durationSec"), 0.5,
                                      &durationSec, err))
                return {};
            if (durationSec < 0.1 || durationSec > 5.0)
                return setError(err, QStringLiteral("durationSec must be in range [0.1, 5.0] seconds")),
                       QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};

            if (target.audio) {
                if (target.track->isLocked())
                    return setError(err, QStringLiteral("track is locked")), QJsonObject();
                if (type != TransitionType::CrossDissolve
                    && type != TransitionType::FadeIn
                    && type != TransitionType::FadeOut) {
                    return setError(err, QStringLiteral(
                        "audio set_transition supports CrossDissolve, FadeIn, or FadeOut only")),
                           QJsonObject();
                }
                Timeline* currentTimeline = timeline();
                QString audioError;
                const auto before = snapshotTrackClips(currentTimeline);
                auto clips = target.track->clips();
                const bool applied = type == TransitionType::CrossDissolve
                    ? audioxfade::applyCrossfade(clips, target.clipIndex, durationSec, &audioError)
                    : audioxfade::applyFade(clips, target.clipIndex,
                        type == TransitionType::FadeIn
                            ? AudioFadeEdge::In : AudioFadeEdge::Out,
                        durationSec, &audioError);
                if (!applied)
                    return setError(err, audioError), QJsonObject();

                auto setShape = [&](Transition& transition) {
                    transition.alignment = alignment;
                    transition.easing = easing;
                };
                if (type == TransitionType::FadeIn)
                    setShape(clips[target.clipIndex].leadIn);
                else
                    setShape(clips[target.clipIndex].trailOut);
                if (type == TransitionType::CrossDissolve)
                    setShape(clips[target.clipIndex + 1].leadIn);
                target.track->setClips(clips);
                auto mattes = currentTimeline->trackMatteEntries();
                auto parents = currentTimeline->clipParentEntries();
                // The public parent carrier also contains the sequence store,
                // which is not a clip key and must survive the clip-only remap.
                const QString sequenceStoreKey = timeline_nesting::sequenceStoreParentKey();
                const QString sequenceStore = parents.take(sequenceStoreKey);
                remapTimelineCarrierAfterMutation(currentTimeline, mattes, before);
                remapClipParentEntriesAfterMutation(currentTimeline, parents, before);
                if (!sequenceStore.isEmpty())
                    parents.insert(sequenceStoreKey, sequenceStore);
                currentTimeline->setTrackMatteEntries(mattes);
                currentTimeline->setClipParentEntries(parents);
                currentTimeline->saveUndoState(type == TransitionType::CrossDissolve
                    ? QStringLiteral("Audio Crossfade")
                    : type == TransitionType::FadeIn ? QStringLiteral("Audio Fade In")
                                                     : QStringLiteral("Audio Fade Out"));
                emit target.track->modified();

                const ClipInfo& updated = target.track->clips().at(target.clipIndex);
                QJsonObject response{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("kind"), QStringLiteral("audio")},
                    {QStringLiteral("trackIndex"), target.trackIndex},
                    {QStringLiteral("clipIndex"), target.clipIndex},
                    {QStringLiteral("type"), typeName},
                    {QStringLiteral("durationSec"), durationSec},
                    {QStringLiteral("leadIn"), transitionToJson(updated.leadIn)},
                    {QStringLiteral("trailOut"), transitionToJson(updated.trailOut)}
                };
                if (ignoredEdges) response.insert(QStringLiteral("warning"), edgeWarning);
                return response;
            }

            if (target.trackIndex != 0)
                return setError(err, QStringLiteral("set_transition supports video track 0 only")),
                       QJsonObject();
            if (type == TransitionType::None) {
                const ClipInfo& targetClip = target.track->clips().at(target.clipIndex);
                if (targetClip.leadIn.type == TransitionType::None
                    && targetClip.trailOut.type == TransitionType::None) {
                    return setError(err, QStringLiteral("clip has no transition to clear")),
                           QJsonObject();
                }
            }

            Timeline* currentTimeline = timeline();
            bool previousAudio = false;
            int previousTrack = -1;
            int previousClip = -1;
            for (int trackIndex = 0;
                 trackIndex < currentTimeline->videoTracks().size(); ++trackIndex) {
                TimelineTrack* track = currentTimeline->videoTracks().at(trackIndex);
                if (track && track->selectedClip() >= 0) {
                    previousTrack = trackIndex;
                    previousClip = track->selectedClip();
                    break;
                }
            }
            if (previousTrack < 0) {
                for (int trackIndex = 0;
                     trackIndex < currentTimeline->audioTracks().size(); ++trackIndex) {
                    TimelineTrack* track = currentTimeline->audioTracks().at(trackIndex);
                    if (track && track->selectedClip() >= 0) {
                        previousAudio = true;
                        previousTrack = trackIndex;
                        previousClip = track->selectedClip();
                        break;
                    }
                }
            }

            auto restoreSelection = [&]() {
                QString ignored;
                if (previousTrack >= 0)
                    currentTimeline->selectClipByIndex(previousAudio, previousTrack,
                                                       previousClip, &ignored);
                else
                    currentTimeline->clearSelection();
                syncSelectionAfterEdit();
            };
            QString selectionError;
            if (!currentTimeline->selectClipByIndex(false, 0, target.clipIndex,
                                                    &selectionError)) {
                restoreSelection();
                return setError(err, selectionError), QJsonObject();
            }

            Transition transition;
            transition.type = type;
            transition.duration = durationSec;
            transition.alignment = alignment;
            transition.easing = easing;
            if (supportsEdgeParams(type)) {
                transition.softness = softness;
                transition.borderWidth = borderWidth;
                transition.borderColor = borderColor;
            }
            if (type == TransitionType::None)
                currentTimeline->clearTransitionsOnSelected();
            else
                currentTimeline->applyTransitionToSelected(transition);
            restoreSelection();

            const ClipInfo& updated = target.track->clips().at(target.clipIndex);
            QJsonObject response{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("type"), transitionTypeNames().at(static_cast<int>(type))},
                {QStringLiteral("durationSec"),
                 type == TransitionType::None ? 0.0 : durationSec},
                {QStringLiteral("leadIn"), transitionToJson(updated.leadIn)},
                {QStringLiteral("trailOut"), transitionToJson(updated.trailOut)}
            };
            if (ignoredEdges) response.insert(QStringLiteral("warning"), edgeWarning);
            return response;
        })
    }, setTransitionOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("add_text_overlay"),
        QStringLiteral("Adds normal text/telop to V1. startSec and endSec are timeline absolute times (sec); endSec must be after startSec. It attaches to all V1 clips overlapping the range, so it still shows across clip boundaries (error if no overlapping clip; the response's clipIndices lists the attached clips). x / y are normalized coordinates 0..1, fontSize is 6..256 in points (default 32), color is QColor/CSS format (default #ffffff). Revertible with Ctrl+Z / the undo tool."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("text"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("Text to display. Empty string is not allowed")}
            }},
            {QStringLiteral("startSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"), QStringLiteral("Display start position (sec, timeline absolute time)")}
            }},
            {QStringLiteral("endSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"), QStringLiteral("Display end position (sec, timeline absolute time)")}
            }},
            {QStringLiteral("x"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.0},
                {QStringLiteral("maximum"), 1.0},
                {QStringLiteral("default"), 0.5},
                {QStringLiteral("description"), QStringLiteral("Center X normalized coordinate 0..1, default 0.5")}
            }},
            {QStringLiteral("y"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.0},
                {QStringLiteral("maximum"), 1.0},
                {QStringLiteral("default"), 0.85},
                {QStringLiteral("description"), QStringLiteral("Center Y normalized coordinate 0..1, default 0.85")}
            }},
            {QStringLiteral("fontSize"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 6},
                {QStringLiteral("maximum"), 256},
                {QStringLiteral("default"), 32},
                {QStringLiteral("description"), QStringLiteral("Font size (pt), 6..256, default 32")}
            }},
            {QStringLiteral("color"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("default"), QStringLiteral("#ffffff")},
                {QStringLiteral("description"), QStringLiteral("Text color. QColor/CSS format, default #ffffff")}
            }}
        }, {QStringLiteral("text"), QStringLiteral("startSec"), QStringLiteral("endSec")}),
        guardedWrite(QStringLiteral("add_text_overlay"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("text"), QStringLiteral("startSec"),
                                         QStringLiteral("endSec"), QStringLiteral("x"),
                                         QStringLiteral("y"), QStringLiteral("fontSize"),
                                         QStringLiteral("color")},
                                        err))
                return {};
            QString text;
            if (!requiredString(args, QStringLiteral("text"), &text, err))
                return {};
            if (text.trimmed().isEmpty())
                return setError(err, QStringLiteral("text must not be empty")), QJsonObject();

            double startSec = 0.0;
            double endSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("startSec"), &startSec, err)
                || !requiredFiniteNumber(args, QStringLiteral("endSec"), &endSec, err))
                return {};
            if (startSec < 0.0 || endSec < 0.0)
                return setError(err, QStringLiteral("text times must be non-negative")), QJsonObject();
            if (endSec <= startSec)
                return setError(err, QStringLiteral("endSec must be greater than startSec")),
                       QJsonObject();

            double x = 0.5;
            double y = 0.85;
            if (args.contains(QStringLiteral("x"))) {
                if (!finiteNumberForMcp(args, QStringLiteral("x"), &x, err))
                    return {};
                if (x < 0.0 || x > 1.0)
                    return setError(err, QStringLiteral("x must be in range [0, 1]")),
                           QJsonObject();
            }
            if (args.contains(QStringLiteral("y"))) {
                if (!finiteNumberForMcp(args, QStringLiteral("y"), &y, err))
                    return {};
                if (y < 0.0 || y > 1.0)
                    return setError(err, QStringLiteral("y must be in range [0, 1]")),
                           QJsonObject();
            }

            int fontSize = 32;
            if (!positiveInteger(args, QStringLiteral("fontSize"), 32,
                                 &fontSize, err))
                return {};
            if (fontSize < 6 || fontSize > 256)
                return setError(err, QStringLiteral("fontSize must be in range [6, 256]")),
                       QJsonObject();

            QString colorText = QStringLiteral("#ffffff");
            if (args.contains(QStringLiteral("color"))
                && !requiredString(args, QStringLiteral("color"), &colorText, err))
                return {};
            const QColor color(colorText);
            if (!color.isValid())
                return setError(err, QStringLiteral("color must be a valid QColor/CSS color")),
                       QJsonObject();

            Timeline* currentTimeline = timeline();
            if (!m_window || !currentTimeline)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            EnhancedTextOverlay overlay;
            overlay.text = text;
            QFont font = overlay.font;
            font.setPointSize(fontSize);
            overlay.font = font;
            overlay.color = color;
            overlay.backgroundColor = QColor(0, 0, 0, 0);
            overlay.x = x;
            overlay.y = y;
            overlay.startTime = startSec;
            overlay.endTime = endSec;
            // レンダラはその時刻にアクティブな V1 クリップのオーバーレイだけを焼き込む
            // ので、区間と重なる全クリップへ付ける (clip 0 固定だと 2 個目以降の
            // クリップの時間帯に出したテキストが表示されない)。
            const QVector<int> touchedClips =
                currentTimeline->addTextOverlayToVideoClipsInRange(overlay, startSec, endSec);
            if (touchedClips.isEmpty()) {
                return setError(err, QStringLiteral("V1 has no clip overlapping startSec..endSec (%1..%2 sec). Check clip time ranges with get_timeline")
                                         .arg(startSec).arg(endSec)),
                       QJsonObject();
            }
            if (m_window->m_player)
                m_window->m_player->setTextOverlays(currentTimeline->timelineTextOverlays());

            QJsonArray clipIndices;
            for (int clipIndex : touchedClips)
                clipIndices.append(clipIndex);
            const int index = currentTimeline->videoTracks().first()->clips()
                                  .at(touchedClips.first()).textManager.count() - 1;
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("index"), index},
                {QStringLiteral("clipIndices"), clipIndices},
                {QStringLiteral("text"), overlay.text},
                {QStringLiteral("startSec"), overlay.startTime},
                {QStringLiteral("endSec"), overlay.endTime},
                {QStringLiteral("x"), overlay.x},
                {QStringLiteral("y"), overlay.y},
                {QStringLiteral("fontSize"), overlay.font.pointSize()},
                {QStringLiteral("color"), overlay.color.name(QColor::HexArgb)}
            };
        })
    }, addTextOverlayOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("add_caption"),
        QStringLiteral("Generates it internally if the subtitle editor is not open (not shown on screen). Adds 1 entry to the subtitle editor's subtitle list,"
                       "Call apply_captions to reflect it on the timeline. This operation itself is not undoable."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("text"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("startSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }},
            {QStringLiteral("endSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }}
        }, {QStringLiteral("text"), QStringLiteral("startSec"), QStringLiteral("endSec")}),
        guardedWrite(QStringLiteral("add_caption"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("text"), QStringLiteral("startSec"),
                                         QStringLiteral("endSec")}, err))
                return {};
            QString text;
            if (!requiredString(args, QStringLiteral("text"), &text, err))
                return {};
            double startSec = 0.0;
            double endSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("startSec"), &startSec, err)
                || !requiredFiniteNumber(args, QStringLiteral("endSec"), &endSec, err)) {
                return {};
            }
            if (startSec < 0.0 || endSec < 0.0)
                return setError(err, QStringLiteral("caption times must be non-negative")), QJsonObject();
            if (endSec <= startSec) {
                setError(err, QStringLiteral("endSec must be greater than startSec"));
                return {};
            }
            constexpr double kMaxCaptionSec =
                static_cast<double>(std::numeric_limits<qint64>::max()) / 1000.0;
            if (startSec > kMaxCaptionSec || endSec > kMaxCaptionSec)
                return setError(err, QStringLiteral("caption time is too large")), QJsonObject();
            CaptionEditorDialog* dialog = m_window
                ? m_window->ensureCaptionEditorDialog() : nullptr;
            if (!dialog)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            caption::Track track = dialog->track();
            const QList<caption::Clip> oldClips = track.clips();
            const qint64 startMs = qRound64(startSec * 1000.0);
            const qint64 endMs = qRound64(endSec * 1000.0);
            if (endMs <= startMs) {
                setError(err, QStringLiteral("endSec must be greater than startSec"));
                return {};
            }
            int insertIndex = 0;
            for (const caption::Clip& clip : oldClips) {
                if (clip.startMs <= startMs)
                    ++insertIndex;
            }

            // 字幕は CaptionEditorDialog が持つ caption::Track 側の状態で、
            // Timeline::currentState() のスナップショットには入らない。ここで
            // saveUndoState を呼ぶと「押しても何も戻らない Ctrl+Z」を 1 段積むだけ
            // になるので呼ばない。取り消しは字幕エディタ側の責務。
            caption::Clip captionClip;
            captionClip.startMs = startMs;
            captionClip.endMs = endMs;
            captionClip.text = text;
            track.addClip(captionClip);
            track.sortByStart();
            dialog->setTrack(track);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("index"), insertIndex},
                {QStringLiteral("captionCount"), track.clipCount()}
            };
        })
    }, addCaptionOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("apply_captions"),
        QStringLiteral("Applies the subtitles held in the subtitle editor to the timeline as a V1 one-word subtitle overlay (same path as the subtitle editor's \"Apply one-word subtitles to timeline\"). Existing generated one-word subtitles are replaced. Revertible with Ctrl+Z / the undo tool (only the timeline side is reverted; the subtitle editor's list is not)."),
        objectSchema(),
        guardedWrite(QStringLiteral("apply_captions"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("Editor or timeline is not available")),
                       QJsonObject();

            CaptionEditorDialog* dialog = m_window->ensureCaptionEditorDialog();
            if (!dialog)
                return setError(err, QStringLiteral("Editor or timeline is not available")),
                       QJsonObject();
            const caption::Track track = dialog->track();
            if (track.clipCount() <= 0)
                return setError(err,
                                QStringLiteral("No subtitles to apply. Please add some with add_caption.")),
                       QJsonObject();

            for (const caption::Clip& clip : track.clips()) {
                if (clip.text.trimmed().isEmpty())
                    return setError(err,
                                    QStringLiteral("Empty captions cannot be applied to the timeline.")),
                           QJsonObject();
                if (clip.endMs <= clip.startMs)
                    return setError(err,
                                    QStringLiteral("Caption end time must be after start time.")),
                           QJsonObject();
            }

            QString error;
            int appliedCount = 0;
            if (!m_window->applyCaptionEditorTrackToTimeline(&error, &appliedCount))
                return setError(err, error), QJsonObject();

            m_window->updateEditActions();
            Timeline* currentTimeline = timeline();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("appliedCount"), appliedCount},
                {QStringLiteral("captionCount"), dialog->track().clipCount()},
                {QStringLiteral("timelineCaptionCount"), currentTimeline
                    ? currentTimeline->generatedCaptionOverlays().size() : 0}
            };
        })
    }, applyCaptionsOutputSchema));

    const QJsonObject captionListOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("captionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("captionCount")});

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("remove_caption"),
        QStringLiteral("Deletes 1 subtitle at index (captions[].index from get_captions) from the subtitle editor's list. Call apply_captions to reflect it on the timeline. This operation itself is not undoable."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("index"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}
            }}
        }, {QStringLiteral("index")}),
        guardedWrite(QStringLiteral("remove_caption"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("index")}, err))
                return {};
            if (!args.contains(QStringLiteral("index")))
                return setError(err, QStringLiteral("index is required")), QJsonObject();
            int index = -1;
            if (!nonNegativeInteger(args, QStringLiteral("index"), -1, &index, err))
                return {};
            CaptionEditorDialog* dialog = m_window
                ? m_window->ensureCaptionEditorDialog() : nullptr;
            if (!dialog)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            caption::Track track = dialog->track();
            if (index < 0 || index >= track.clipCount()) {
                return setError(err, QStringLiteral("index is out of range (%1 subtitles: 0..%2)")
                                         .arg(track.clipCount()).arg(track.clipCount() - 1)),
                       QJsonObject();
            }
            track.removeClipAt(index);
            dialog->setTrack(track);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("captionCount"), track.clipCount()}
            };
        })
    }, captionListOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("clear_captions"),
        QStringLiteral("Empties the subtitle editor's list. Generated subtitles already on the timeline stay as-is, so note that you cannot then call apply_captions to remove them (an empty list cannot be applied). Revert the timeline side with the undo tool. This operation itself is not undoable."),
        objectSchema(),
        guardedWrite(QStringLiteral("clear_captions"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            CaptionEditorDialog* dialog = m_window
                ? m_window->ensureCaptionEditorDialog() : nullptr;
            if (!dialog)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            caption::Track track = dialog->track();
            track.clear();
            dialog->setTrack(track);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("captionCount"), track.clipCount()}
            };
        })
    }, captionListOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_playhead"),
        QStringLiteral("Moves the playhead to the specified time. If timeSec is out of range, it is clamped to [0, total timeline duration] without error, and the actually set position is returned as playheadSec. Also seeks VideoPlayer; while stopped, the preview updates to the frame at the specified time (rendered after the event loop). playing in the response indicates whether it was playing before the call; previewSeekRequested indicates whether a seek was requested to VideoPlayer. Does not change edit state and does not affect timeline edit Undo / redo."),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("Timeline absolute time (sec). Out-of-range values are clamped to [0, total timeline duration].")}
            }}
        }, {QStringLiteral("timeSec")}),
        guardedWrite(QStringLiteral("set_playhead"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("timeSec")}, err))
                return {};
            double timeSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("timeSec"), &timeSec, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (timeSec < 0.0)
                timeSec = 0.0;
            const double duration = qMax(0.0, timeline()->totalDuration());
            timeSec = qMin(timeSec, duration);
            timeline()->setPlayheadPosition(timeSec);
            VideoPlayer* player = m_window->m_player;
            const bool playing = player && player->isPlaying();
            if (player)
                player->seek(qRound(timeSec * 1000.0));
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("playheadSec"), timeSec},
                {QStringLiteral("playing"), playing},
                {QStringLiteral("previewSeekRequested"), player != nullptr}
            };
        })
    }, setPlayheadOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("undo"),
        QStringLiteral("Destructive operation that undoes the last timeline change. Recorded changes can be reverted with Ctrl+Z / the undo tool."),
        objectSchema(),
        guardedWrite(QStringLiteral("undo"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (!timeline()->canUndo())
                return QJsonObject{{QStringLiteral("ok"), false},
                                   {QStringLiteral("reason"), QStringLiteral("nothing to undo")}};
            timeline()->undo();
            syncSelectionAfterEdit();
            return QJsonObject{{QStringLiteral("ok"), true}};
        })
    }, undoOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("redo"),
        QStringLiteral("Destructive operation that reapplies the last undone timeline change. Changes can be re-redone with Ctrl+Y / the redo tool."),
        objectSchema(),
        guardedWrite(QStringLiteral("redo"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (!timeline()->canRedo())
                return QJsonObject{{QStringLiteral("ok"), false},
                                   {QStringLiteral("reason"), QStringLiteral("nothing to redo")}};
            timeline()->redo();
            syncSelectionAfterEdit();
            return QJsonObject{{QStringLiteral("ok"), true}};
        })
    }, redoOutputSchema));
}

} // namespace mcp
