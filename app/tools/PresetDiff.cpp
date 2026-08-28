// SankoPresetDiff - the PROMOTION instrument (permanent tool).
//
// Usage: SankoPresetDiff <override.sankobrush>
//
// Loads a built-in OVERRIDE file, finds the stock recipe it shadows in
// builtinRoster(), and prints every field that differs, in recipe
// vocabulary ("spacing: 0.08 -> 0.05") - the report the user reviews
// BEFORE any recipe edit happens.
//
// THE COMPLETENESS GUARANTEE, mechanical rather than promised: after
// printing, the tool applies its ENTIRE field vocabulary from the override
// onto a copy of the stock brush and requires the result to be
// BYTE-IDENTICAL to the override through the codec. If any tuned state
// lies outside this tool's vocabulary, the reconstruction mismatches and
// the tool says STOP - the promotion must not proceed by working around
// it. Image-bearing fields (custom tip, custom grain) are declared
// unpromotable by design: a recipe cannot carry an image diff reviewably.
//
// Exit codes: 0 = diff printed and reconstruction proved complete;
// 2 = STOP (unpromotable or vocabulary gap); 1 = usage/load errors.

#include "brushlib/BrushPresetCodec.h"
#include "brushlib/BuiltinRoster.h"

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

#include <cstdio>

using brushlib::BrushPreset;
using brushlib::BrushPresetCodec;

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

QString num(qreal v) { return QString::number(v, 'g', 6); }

QString curveText(const PressureCurve &c)
{
    QString s = QStringLiteral("{");
    const auto pts = c.controlPoints();
    for (int i = 0; i < pts.size(); ++i)
        s += QStringLiteral("%1(%2,%3)")
                 .arg(i ? QStringLiteral(" ") : QString())
                 .arg(num(pts.at(i).x()))
                 .arg(num(pts.at(i).y()));
    return s + QStringLiteral("}");
}

struct Differ
{
    const ::Brush &stock;
    const ::Brush &tuned;
    ::Brush &rebuilt; // stock copy the tuned values are applied onto
    int diffs = 0;
    bool stop = false;

    template <typename Get, typename Set, typename Fmt>
    void field(const char *name, Get get, Set set, Fmt fmt)
    {
        const auto s = get(stock);
        const auto t = get(tuned);
        set(rebuilt, t); // reconstruction applies EVERY field, not just diffs
        if (s != t) {
            ++diffs;
            out() << "  " << name << ": " << fmt(s) << " -> " << fmt(t)
                  << Qt::endl;
        }
    }
    void curve(const char *name, PressureCurve &(::Brush::*accessor)())
    {
        const PressureCurve &s = (const_cast<::Brush &>(stock).*accessor)();
        const PressureCurve &t = (const_cast<::Brush &>(tuned).*accessor)();
        (rebuilt.*accessor)().setControlPoints(t.controlPoints());
        if (s.controlPoints() != t.controlPoints()) {
            ++diffs;
            out() << "  " << name << ": " << curveText(s) << " -> "
                  << curveText(t) << Qt::endl;
        }
    }
    void image(const char *name, const QImage &s, const QImage &t)
    {
        if (s.isNull() != t.isNull()
            || (!s.isNull() && s != t)) {
            stop = true;
            out() << "  STOP: " << name
                  << " differs - an image field cannot be expressed in the "
                     "recipe vocabulary; this override is not promotable "
                     "as-is."
                  << Qt::endl;
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: SankoPresetDiff <override.sankobrush>\n");
        return 1;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    BrushPreset over;
    if (!BrushPresetCodec::loadPreset(f.readAll(), over)) {
        std::fprintf(stderr, "not a valid preset file\n");
        return 1;
    }
    const BrushPreset *stockPreset = nullptr;
    const QVector<BrushPreset> roster = brushlib::builtinRoster();
    for (const BrushPreset &p : roster)
        if (p.id == over.id)
            stockPreset = &p;
    if (!stockPreset) {
        std::fprintf(stderr, "override id %s has no stock roster entry\n",
                     qPrintable(over.id));
        return 1;
    }
    out() << "OVERRIDE: " << over.id << " (\"" << stockPreset->name
          << "\")" << Qt::endl;

    const ::Brush &S = stockPreset->brush;
    const ::Brush &T = over.brush;
    if (S.dualBrushEnabled() || T.dualBrushEnabled()) {
        out() << "  STOP: dual-brush presets carry a secondary payload this "
                 "tool does not walk; promote by hand with care."
              << Qt::endl;
        return 2;
    }

    ::Brush rebuilt = S;
    Differ d{S, T, rebuilt};
    using B = ::Brush;
    const auto fnum = [](qreal v) { return num(v); };
    const auto fint = [](int v) { return QString::number(v); };
    const auto fbool = [](bool v) {
        return v ? QStringLiteral("true") : QStringLiteral("false");
    };
    const auto fcol = [](const QColor &c) { return c.name(QColor::HexArgb); };

    d.field("size", [](const B &b) { return b.size(); },
            [](B &b, int x) { b.setSize(x); }, fint);
    d.field("spacing", [](const B &b) { return b.spacing(); },
            [](B &b, qreal x) { b.setSpacing(x); }, fnum);
    d.field("opacity", [](const B &b) { return b.opacity(); },
            [](B &b, qreal x) { b.setOpacity(x); }, fnum);
    d.field("flow", [](const B &b) { return b.flow(); },
            [](B &b, qreal x) { b.setFlow(x); }, fnum);
    d.field("hardness", [](const B &b) { return b.hardness(); },
            [](B &b, qreal x) { b.setHardness(x); }, fnum);
    d.field("color", [](const B &b) { return b.color(); },
            [](B &b, const QColor &x) { b.setColor(x); }, fcol);
    d.field("toolMode", [](const B &b) { return qint32(b.toolMode()); },
            [](B &b, qint32 x) { b.setToolMode(B::ToolMode(x)); }, fint);
    d.field("smudgeStrength", [](const B &b) { return b.smudgeStrength(); },
            [](B &b, qreal x) { b.setSmudgeStrength(x); }, fnum);
    d.field("tiltAffectsShape",
            [](const B &b) { return b.tiltAffectsShape(); },
            [](B &b, bool x) { b.setTiltAffectsShape(x); }, fbool);
    d.field("rotationAffectsShape",
            [](const B &b) { return b.rotationAffectsShape(); },
            [](B &b, bool x) { b.setRotationAffectsShape(x); }, fbool);
    d.field("maxTiltElongation",
            [](const B &b) { return b.maxTiltElongation(); },
            [](B &b, qreal x) { b.setMaxTiltElongation(x); }, fnum);
    d.field("scatterAlong", [](const B &b) { return b.scatterAlong(); },
            [](B &b, qreal x) { b.setScatterAlong(x); }, fnum);
    d.field("scatterPerpendicular",
            [](const B &b) { return b.scatterPerpendicular(); },
            [](B &b, qreal x) { b.setScatterPerpendicular(x); }, fnum);
    d.field("scatterCount", [](const B &b) { return b.scatterCount(); },
            [](B &b, int x) { b.setScatterCount(x); }, fint);
    d.field("sizeJitter", [](const B &b) { return b.sizeJitter(); },
            [](B &b, qreal x) { b.setSizeJitter(x); }, fnum);
    d.field("angleJitter", [](const B &b) { return b.angleJitter(); },
            [](B &b, qreal x) { b.setAngleJitter(x); }, fnum);
    d.field("roundnessJitter",
            [](const B &b) { return b.roundnessJitter(); },
            [](B &b, qreal x) { b.setRoundnessJitter(x); }, fnum);
    d.field("spacingJitter", [](const B &b) { return b.spacingJitter(); },
            [](B &b, qreal x) { b.setSpacingJitter(x); }, fnum);
    d.field("grainScale", [](const B &b) { return b.grainScale(); },
            [](B &b, qreal x) { b.setGrainScale(x); }, fnum);
    d.field("grainDepth", [](const B &b) { return b.grainDepth(); },
            [](B &b, qreal x) { b.setGrainDepth(x); }, fnum);
    d.field("grainContrast", [](const B &b) { return b.grainContrast(); },
            [](B &b, qreal x) { b.setGrainContrast(x); }, fnum);
    d.field("grainRotation", [](const B &b) { return b.grainRotation(); },
            [](B &b, qreal x) { b.setGrainRotation(x); }, fnum);
    d.field("grainMode", [](const B &b) { return qint32(b.grainMode()); },
            [](B &b, qint32 x) { b.setGrainMode(B::GrainMode(x)); }, fint);
    d.field("grainAffectsColor",
            [](const B &b) { return b.grainAffectsColor(); },
            [](B &b, bool x) { b.setGrainAffectsColor(x); }, fbool);
    d.field("grainPreset",
            [](const B &b) { return qint32(b.grainPreset()); },
            [](B &b, qint32 x) {
                if (B::GrainPreset(x) != B::GrainPreset::Custom)
                    b.setGrainPreset(B::GrainPreset(x));
            },
            fint);
    d.image("customGrain",
            S.grainPreset() == B::GrainPreset::Custom ? S.grainTexture()
                                                      : QImage(),
            T.grainPreset() == B::GrainPreset::Custom ? T.grainTexture()
                                                      : QImage());
    d.field("hueJitter", [](const B &b) { return b.hueJitter(); },
            [](B &b, qreal x) { b.setHueJitter(x); }, fnum);
    d.field("saturationJitter",
            [](const B &b) { return b.saturationJitter(); },
            [](B &b, qreal x) { b.setSaturationJitter(x); }, fnum);
    d.field("brightnessJitter",
            [](const B &b) { return b.brightnessJitter(); },
            [](B &b, qreal x) { b.setBrightnessJitter(x); }, fnum);
    d.image("customTip", S.customShape(), T.customShape());
    if (!T.customShape().isNull() && T.customShape() != S.customShape()) {
        // handled by d.image above (stop set); nothing more to apply
    } else {
        rebuilt.setCustomShape(T.customShape());
    }
    d.field("dualBlendMode",
            [](const B &b) { return qint32(b.dualBlendMode()); },
            [](B &b, qint32 x) { b.setDualBlendMode(B::DualBlendMode(x)); },
            fint);
    d.field("dualMasterOpacity",
            [](const B &b) { return b.dualMasterOpacity(); },
            [](B &b, qreal x) { b.setDualMasterOpacity(x); }, fnum);
    d.curve("sizePressureCurve", &B::sizePressureCurve);
    d.curve("opacityPressureCurve", &B::opacityPressureCurve);
    d.curve("hardnessPressureCurve", &B::hardnessPressureCurve);
    d.curve("flowPressureCurve", &B::flowPressureCurve);
    d.curve("scatterPressureCurve", &B::scatterPressureCurve);
    d.curve("smudgePressureCurve", &B::smudgePressureCurve);
    d.curve("sizeJitterPressureCurve", &B::sizeJitterPressureCurve);
    d.curve("angleJitterPressureCurve", &B::angleJitterPressureCurve);
    d.curve("roundnessJitterPressureCurve", &B::roundnessJitterPressureCurve);
    d.curve("spacingJitterPressureCurve", &B::spacingJitterPressureCurve);
    d.curve("grainDepthPressureCurve", &B::grainDepthPressureCurve);
    d.curve("hueJitterPressureCurve", &B::hueJitterPressureCurve);
    d.curve("saturationJitterPressureCurve",
            &B::saturationJitterPressureCurve);
    d.curve("brightnessJitterPressureCurve",
            &B::brightnessJitterPressureCurve);
    d.curve("fgBgJitterPressureCurve", &B::fgBgJitterPressureCurve);
    for (int i = 0; i < int(B::kDynamicPropertyCount); ++i) {
        const auto prop = B::DynamicProperty(i);
        const QString sName =
            QStringLiteral("controlSource[%1]").arg(i);
        const QString mName =
            QStringLiteral("controlMinimum[%1]").arg(i);
        const QByteArray sBytes = sName.toLatin1();
        const QByteArray mBytes = mName.toLatin1();
        d.field(sBytes.constData(),
                [prop](const B &b) { return qint32(b.controlSource(prop)); },
                [prop](B &b, qint32 x) {
                    b.setControlSource(prop, B::ControlSource(x));
                },
                fint);
        d.field(mBytes.constData(),
                [prop](const B &b) { return b.controlMinimum(prop); },
                [prop](B &b, qreal x) { b.setControlMinimum(prop, x); },
                fnum);
    }
    d.field("tipAngle", [](const B &b) { return b.tipAngle(); },
            [](B &b, qreal x) { b.setTipAngle(x); }, fnum);
    d.field("tipRoundness", [](const B &b) { return b.tipRoundness(); },
            [](B &b, qreal x) { b.setTipRoundness(x); }, fnum);
    d.field("tipFlipX", [](const B &b) { return b.tipFlipX(); },
            [](B &b, bool x) { b.setTipFlipX(x); }, fbool);
    d.field("tipFlipY", [](const B &b) { return b.tipFlipY(); },
            [](B &b, bool x) { b.setTipFlipY(x); }, fbool);
    d.field("fadeDistance", [](const B &b) { return b.fadeDistance(); },
            [](B &b, qreal x) { b.setFadeDistance(x); }, fnum);
    d.field("wetEdges", [](const B &b) { return b.wetEdges(); },
            [](B &b, qreal x) { b.setWetEdges(x); }, fnum);
    d.field("buildUp", [](const B &b) { return b.buildUp(); },
            [](B &b, qreal x) { b.setBuildUp(x); }, fnum);
    d.field("fgBgJitter", [](const B &b) { return b.fgBgJitter(); },
            [](B &b, qreal x) { b.setFgBgJitter(x); }, fnum);
    d.field("backgroundColor",
            [](const B &b) { return b.backgroundColor(); },
            [](B &b, const QColor &x) { b.setBackgroundColor(x); }, fcol);
    d.field("purity", [](const B &b) { return b.purity(); },
            [](B &b, qreal x) { b.setPurity(x); }, fnum);
    d.field("colorDynamicsPerTip",
            [](const B &b) { return b.colorDynamicsPerTip(); },
            [](B &b, bool x) { b.setColorDynamicsPerTip(x); }, fbool);
    d.field("dualMode", [](const B &b) { return qint32(b.dualMode()); },
            [](B &b, qint32 x) { b.setDualMode(B::DualMode(x)); }, fint);
    d.field("noise", [](const B &b) { return b.noise(); },
            [](B &b, qreal x) { b.setNoise(x); }, fnum);
    d.field("textureBlendMode",
            [](const B &b) { return qint32(b.textureBlendMode()); },
            [](B &b, qint32 x) {
                b.setTextureBlendMode(B::TextureBlendMode(x));
            },
            fint);
    d.field("eraseMode", [](const B &b) { return b.eraseMode(); },
            [](B &b, bool x) { b.setEraseMode(x); }, fbool);

    if (d.stop)
        return 2;
    out() << QStringLiteral("%1 field(s) differ").arg(d.diffs) << Qt::endl;

    // THE RECONSTRUCTION CHECK: the vocabulary above, applied in full, must
    // reproduce the override byte-for-byte. Anything it missed means the
    // promotion CANNOT proceed through this tool's report - STOP.
    if (BrushPresetCodec::saveBrush(rebuilt)
        != BrushPresetCodec::saveBrush(T)) {
        out() << "STOP: reconstruction mismatch - the override contains "
                 "state outside this tool's field vocabulary. Do not "
                 "promote; extend the tool (and its vocabulary) first."
              << Qt::endl;
        return 2;
    }
    out() << "reconstruction check: the diff above is COMPLETE "
             "(vocabulary reproduces the override byte-for-byte)"
          << Qt::endl;
    return 0;
}
