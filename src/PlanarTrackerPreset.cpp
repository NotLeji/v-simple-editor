#include "PlanarTrackerPreset.h"
#include <QJsonValue>

namespace planar_tracker_preset {

const std::array<PlanarTrackerPreset, 5>& builtinPresets()
{
    static const std::array<PlanarTrackerPreset, 5> kPresets = {{
        // 1. generic-default
        {
            "generic-default",
            "General Default",
            "Medium motion and patch size. Generic defaults that work for most footage.",
            16.0, 32.0, 0.3, 0
        },
        // 2. precise-feature
        {
            "precise-feature",
            "Precise Feature Tracking",
            "Track small features (face parts, product logos) with high precision. Narrow search, low damping.",
            8.0, 24.0, 0.1, 0
        },
        // 3. robust-motion
        {
            "robust-motion",
            "Large Motion Tracking",
            "Handles large motion. Wide search + high damping absorbs jitter.",
            32.0, 48.0, 0.5, 0
        },
        // 4. low-light-noisy
        {
            "low-light-noisy",
            "Low-Light Noise",
            "For low-light, noisy footage. Larger patches ensure robustness.",
            24.0, 64.0, 0.6, 0
        },
        // 5. fast-preview
        {
            "fast-preview",
            "Fast Preview",
            "For fast previews. Balanced parameters prioritizing frame rate.",
            12.0, 24.0, 0.2, 0
        },
    }};
    return kPresets;
}

std::optional<PlanarTrackerPreset> findBuiltin(const QString& id)
{
    for (const auto& p : builtinPresets()) {
        if (p.id == id)
            return p;
    }
    return std::nullopt;
}

QJsonObject toJson(const PlanarTrackerPreset& p)
{
    QJsonObject obj;
    obj["id"]                = p.id;
    obj["displayName"]       = p.displayName;
    obj["description"]       = p.description;
    obj["searchRadiusPx"]    = p.searchRadiusPx;
    obj["patchSizePx"]       = p.patchSizePx;
    obj["dampingFactor"]     = p.dampingFactor;
    obj["maxFramesPerCall"]  = p.maxFramesPerCall;
    return obj;
}

std::optional<PlanarTrackerPreset> fromJson(const QJsonObject& j)
{
    // Required string fields
    if (!j.contains("id") || !j.contains("displayName"))
        return std::nullopt;

    PlanarTrackerPreset p;
    p.id          = j["id"].toString();
    p.displayName = j["displayName"].toString();
    if (p.id.isEmpty() || p.displayName.isEmpty())
        return std::nullopt;

    // searchRadiusPx in [4.0, 64.0]
    const QJsonValue srv = j.value("searchRadiusPx");
    if (!srv.isDouble())
        return std::nullopt;
    const double sr = srv.toDouble();
    if (sr < 4.0 || sr > 64.0)
        return std::nullopt;
    p.searchRadiusPx = sr;

    // patchSizePx in [16.0, 128.0]
    const QJsonValue psv = j.value("patchSizePx");
    if (!psv.isDouble())
        return std::nullopt;
    const double ps = psv.toDouble();
    if (ps < 16.0 || ps > 128.0)
        return std::nullopt;
    p.patchSizePx = ps;

    // dampingFactor in [0.0, 1.0]
    const QJsonValue dfv = j.value("dampingFactor");
    if (!dfv.isDouble())
        return std::nullopt;
    const double df = dfv.toDouble();
    if (df < 0.0 || df > 1.0)
        return std::nullopt;
    p.dampingFactor = df;

    // maxFramesPerCall >= 0
    const QJsonValue mfv = j.value("maxFramesPerCall");
    if (!mfv.isDouble())
        return std::nullopt;
    const int mf = mfv.toInt();
    if (mf < 0)
        return std::nullopt;
    p.maxFramesPerCall = mf;

    // description: optional, no validation
    p.description = j.value("description").toString();

    return p;
}

void applyToPlanarTracker(planar::Tracker* tracker, const PlanarTrackerPreset& p) {
    if (!tracker) return;
    planar::TrackParams params;
    params.searchRadiusPx   = p.searchRadiusPx;
    params.patchSizePx      = p.patchSizePx;
    params.dampingFactor    = p.dampingFactor;
    params.maxFramesPerCall = p.maxFramesPerCall;
    tracker->setParams(params);
}

} // namespace planar_tracker_preset
