#include "BuiltinRoster.h"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <functional>

namespace brushlib {
namespace {

// --- Procedural tip masks --------------------------------------------------
// Deterministic grayscale masks (white = opaque). Kept small; the engine
// rescales tips per stamp size.

QImage tipCanvas()
{
    QImage img(128, 128, QImage::Format_Grayscale8);
    img.fill(Qt::black);
    return img;
}

QImage flatTip(qreal widthRatio) // wide, thin rectangle (chisel / flat)
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    const qreal w = 128.0, h = 128.0 * widthRatio;
    p.drawRoundedRect(QRectF(0, (128 - h) / 2.0, w, h), h / 3.0, h / 3.0);
    return img;
}

QImage squareTip()
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawRoundedRect(QRectF(14, 14, 100, 100), 18, 18); // the Figma
    return img;                                          // "Rounded Square"
}

QImage ovalTip(qreal roundness) // filbert
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(64, 64), 60, 60 * roundness);
    return img;
}

QImage starTip()
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    QPainterPath path;
    for (int i = 0; i < 8; ++i) {
        const qreal a = i * M_PI / 4.0;
        const qreal r = (i % 2 == 0) ? 60.0 : 22.0;
        const QPointF pt(64 + r * qCos(a), 64 + r * qSin(a));
        if (i == 0)
            path.moveTo(pt);
        else
            path.lineTo(pt);
    }
    path.closeSubpath();
    p.drawPath(path);
    return img;
}

QImage hatchTip() // three parallel strokes
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(Qt::white, 12, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(20, 34), QPointF(108, 34));
    p.drawLine(QPointF(20, 64), QPointF(108, 64));
    p.drawLine(QPointF(20, 94), QPointF(108, 94));
    return img;
}

QImage streakTip() // broken bristle streaks (dual-brush secondary)
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int ys[] = {18, 34, 47, 62, 74, 90, 105};
    const int lens[] = {70, 96, 58, 108, 66, 92, 52};
    const int xs[] = {30, 10, 44, 8, 36, 18, 48};
    for (int i = 0; i < 7; ++i) {
        p.setPen(QPen(QColor(255, 255, 255, 200 - i * 12), 6, Qt::SolidLine,
                      Qt::RoundCap));
        p.drawLine(QPointF(xs[i], ys[i]), QPointF(xs[i] + lens[i], ys[i]));
    }
    return img;
}

QImage barTip() // hard offset bar (glitch secondary)
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawRect(QRectF(0, 20, 128, 22));
    p.drawRect(QRectF(0, 86, 128, 14));
    return img;
}

QImage dotTip() // small hard dot (splatter secondary)
{
    QImage img = tipCanvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(64, 64), 26, 26);
    return img;
}

// Deterministic procedural grains for the two custom-grain brushes.
QImage halftoneGrain()
{
    QImage img(64, 64, QImage::Format_Grayscale8);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            p.drawEllipse(QPointF(8 + x * 16, 8 + y * 16), 5.5, 5.5);
    return img;
}

QImage saltGrain()
{
    QImage img(64, 64, QImage::Format_Grayscale8);
    img.fill(Qt::black);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    // Fixed pseudo-random speckle (hand-rolled LCG — deterministic forever).
    quint32 state = 0x5EEDu;
    for (int i = 0; i < 90; ++i) {
        state = state * 1664525u + 1013904223u;
        const int x = int(state >> 16) % 64;
        state = state * 1664525u + 1013904223u;
        const int y = int(state >> 16) % 64;
        state = state * 1664525u + 1013904223u;
        const qreal r = 1.0 + (state >> 16) % 100 / 40.0;
        p.drawEllipse(QPointF(x, y), r, r);
    }
    return img;
}

// --- Recipe helpers --------------------------------------------------------

QVector<QPointF> curve2(qreal y0, qreal y1)
{
    return {{0.0, y0}, {1.0, y1}};
}

QString slug(QString name)
{
    name = name.toLower();
    QString out;
    for (const QChar c : name)
        out += c.isLetterOrNumber() ? c : QChar('-');
    return out;
}

BrushPreset make(const QString &category, const QString &name,
                 const std::function<void(::Brush &)> &recipe)
{
    BrushPreset p;
    p.name = name;
    p.category = category;
    p.builtin = true;
    p.id = QStringLiteral("builtin/%1/%2").arg(slug(category), slug(name));
    recipe(p.brush);
    return p;
}

// Display-name rename with the id KEPT (2026-09-05). The id is normally
// the slug of the name, and everything the shelf keys by id - Recent,
// hidden, favourites, UI renames, and the Overrides folder (a built-in
// override whose id no longer exists is silently dropped as stale) -
// would detach if a rename moved it. So a renamed built-in keeps its
// original id as a fossil of the old name: "Rich Ink" is still
// builtin/inking/ink-bleed. Stability is what "do not change preset ids"
// asks for; an id migration would touch five keyed stores for nothing
// the user can see.
BrushPreset make(const QString &category, const QString &name,
                 const QString &keptId,
                 const std::function<void(::Brush &)> &recipe)
{
    BrushPreset p = make(category, name, recipe);
    p.id = keptId;
    return p;
}

} // namespace

QStringList builtinCategories()
{
    // Watercolor was removed as a CATEGORY (its 10 brushes now live under
    // Painting — see the remap at the end of builtinRoster()). Painting
    // therefore lists 22 brushes to every other category's 10; accepted as
    // is, deliberately not rebalanced.
    return {QStringLiteral("Sketching"), QStringLiteral("Drawing"),
            QStringLiteral("Inking"),    QStringLiteral("Painting"),
            QStringLiteral("Artistic")};
}

QVector<BrushPreset> builtinRoster()
{
    using B = ::Brush;
    QVector<BrushPreset> r;
    const QString kSketching = QStringLiteral("Sketching");
    const QString kDrawing = QStringLiteral("Drawing");
    const QString kInking = QStringLiteral("Inking");
    const QString kPainting = QStringLiteral("Painting");
    const QString kArtistic = QStringLiteral("Artistic");
    const QString kWatercolor = QStringLiteral("Watercolor");

    // ---- SKETCHING: translucent, Rolling paper grain, build-up ----------
    auto sketchBase = [](::Brush &b) {
        b.setGrainPreset(B::GrainPreset::Paper);
        b.setGrainMode(B::GrainMode::Rolling);
        b.setSpacing(0.08);
        b.setFlow(0.85);
        b.sizePressureCurve().setControlPoints(curve2(0.35, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.25, 1.0));
    };
    r << make(kSketching, QStringLiteral("HB Pencil"), [&](::Brush &b) {
        // PROMOTED WHOLESALE from the user's "HB Pencil Variation"
        // (2026-08-29) - the first user-preset promotion, replacing the
        // stock recipe entirely. Every value below was READ from the
        // saved .sankobrush, never retyped from memory, and (b11) PINS
        // the claim: the committed fixture of that file, with ONLY size
        // changed, must saveBrush() byte-identically to this recipe. The
        // custom tip ships as a versioned asset (brush_assets.qrc)
        // because images cannot be expressed in recipe vocabulary - the
        // "code plus versioned assets" rule, HANDOFF 2026-08-29.
        // Deliberately NOT built on sketchBase: the field list is the
        // promotion record.
        b.setSize(36);   // the single approved delta (variation drew at 4)
        b.setSpacing(0.08);
        b.setOpacity(0.55); // the HB ceiling - confirmed kept: strokes
                            // top out at mid-grey, the paper shows through
        b.setFlow(0.2);     // gradual buildup toward that ceiling
        b.setHardness(0.62);
        b.setSpacingJitter(0.078125);
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/hb_pencil_tip.png")));
        b.setGrainPreset(B::GrainPreset::Paper);
        b.setGrainScale(40.0);
        b.setGrainDepth(0.55);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        // Depth minimum 1.0: tooth stays fully present at LIGHT pressure
        // (the default pressure response would fade grain exactly where a
        // real pencil shows it most).
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.15);
        b.sizePressureCurve().setControlPoints(curve2(0.35, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.25, 1.0));
    });
    r << make(kSketching, QStringLiteral("H Pencil"), [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP, batch two (2026-08-30): tuned from the batch-one
        // hand-test - "the hard end was under-textured" (4H's verdict
        // applied family-wide: deeper AND finer tooth than the v1 table).
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_h_tip.png")));
        b.setSize(25); // family default (2026-08-30)
        b.setHardness(0.70); // INERT with a custom tip (see 4H)
        b.setOpacity(0.48);
        // v3 treatment scaled from the 4H diagnosis (Paper cannot tooth;
        // see 4H): Charcoal-preset texture, deep valleys, de-saturated
        // overlap. Softer scaling than 4H per grade order.
        b.setFlow(0.30);
        b.setSpacing(0.11);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.80);
        b.setGrainScale(32.0);
        b.setGrainContrast(2.2);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.12);
        b.opacityPressureCurve().setControlPoints(curve2(0.35, 0.9));
    });
    r << make(kSketching, QStringLiteral("2H Pencil"), [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP, batch two (2026-08-30): hard-end texture raised per
        // the batch-one verdict, sitting between 4H and H.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_2h_tip.png")));
        b.setSize(25); // family default (2026-08-30)
        b.setHardness(0.78); // INERT with a custom tip (see 4H)
        b.setOpacity(0.38);
        // v3 treatment scaled from the 4H diagnosis (Paper cannot tooth;
        // see 4H): between H and 4H on the grade ladder.
        b.setFlow(0.28);
        b.setSpacing(0.12);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.85);
        b.setGrainScale(30.0);
        b.setGrainContrast(2.5);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.10);
        b.sizePressureCurve().setControlPoints(curve2(0.7, 1.0));
    });
    r << make(kSketching, QStringLiteral("4H Pencil"), [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP (2026-08-30, first calibration batch with 2B/6B):
        // the user's scanned 4H stamp, asset per the code-plus-versioned-
        // assets rule. Tuning v1 - iteration with the user expected.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_4h_tip.png")));
        b.setSize(25); // v2: the user's chosen default across the family
        b.setHardness(0.85); // INERT with a custom tip: the falloff only
                             // shapes PROCEDURAL tips. Kept as the grade
                             // ladder's record; edge character now comes
                             // from the stamp and Noise.
        b.setOpacity(0.30);  // faint ceiling: a 4H tops out light
        // v3 (MEASURED, 2026-08-30, after v2 "still smooth"): the Paper
        // preset texture CANNOT produce visible tooth - its values
        // cluster ~0.8, raising contrast clamps it flatter, max spread 5
        // at any depth (probe-measured; see HANDOFF). Charcoal preset at
        // fine scale has real dark texels; depth 0.9 makes valleys reach
        // near paper-white (depth is the valley FLOOR: modulation lives
        // in [1-depth, 1]). Flow down + spacing up de-saturate the
        // overlap so the modulation survives into the stroke. Probe:
        // spread 3 (v2) -> 27 (this recipe), valleys p5=4/255.
        b.setFlow(0.25);
        b.setSpacing(0.12);
        b.setGrainPreset(B::GrainPreset::Charcoal); // texture SOURCE, not
                                                    // identity: at scale
                                                    // 30 it reads as fine
                                                    // paper tooth
        b.setGrainDepth(0.90);
        b.setGrainScale(30.0);
        b.setGrainContrast(3.0);
        b.setGrainMode(B::GrainMode::StaticCanvas); // tooth on the paper
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.08);    // dry edge, nudged with the tooth
        b.sizePressureCurve().setControlPoints(curve2(0.85, 1.0));
    });
    r << make(kSketching, QStringLiteral("2B Pencil"), [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP (2026-08-30, first calibration batch with 4H/6B):
        // the user's scanned 2B stamp. Tuning v1 - iteration expected.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_2b_tip.png")));
        b.setSize(25); // v2: size only - the feel passed hand-testing
                       // as-is ("feels good") and is the soft-end
                       // calibration reference the others move around
        b.setHardness(0.52); // INERT with a custom tip (see 4H)
        b.setOpacity(0.70);  // mid-dark ceiling: darker than HB's 0.55,
                             // well short of 6B
        b.setFlow(0.22);     // rich buildup - passes darken gradually
        b.setGrainDepth(0.50);
        b.setGrainScale(40.0);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.15);
        b.setAngleJitter(0.05); // slight per-dab rotation: organic, not loose
        b.sizePressureCurve().setControlPoints(curve2(0.25, 1.0));
    });
    r << make(kSketching, QStringLiteral("4B Pencil"), [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP, batch two (2026-08-30): the soft-end spread widened
        // per the batch-one verdict - 4B sits deliberately BETWEEN the
        // 2B reference and the darker v2 6B: scale progression 40/48/56,
        // opacity-floor progression 0.25/0.32/0.40 across 2B/4B/6B.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_4b_tip.png")));
        b.setSize(25); // family default (2026-08-30)
        b.setHardness(0.42); // INERT with a custom tip (see 4H)
        b.setOpacity(0.85);
        b.setFlow(0.20);
        b.setGrainDepth(0.60);
        b.setGrainScale(48.0);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.20);
        b.setAngleJitter(0.08);
        b.setTiltAffectsShape(true); b.setMaxTiltElongation(2.2);
        b.opacityPressureCurve().setControlPoints(curve2(0.32, 1.0));
    });
    r << make(kSketching, QStringLiteral("6B Pencil"), [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP (2026-08-30, first calibration batch with 4H/2B):
        // the user's scanned 6B stamp. Tuning v1 - iteration expected.
        // The soft extreme of the graphite ladder: it must feel
        // SIGNIFICANTLY softer and darker than 2B.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_6b_tip.png")));
        b.setSize(25); // v2: the user's chosen default across the family
        b.setHardness(0.34); // INERT with a custom tip (see 4H)
        // v2 (hand-tested 2026-08-30, "too similar to 2B - a bit
        // darker"): the naive lever was flow, and it was DECLINED - a
        // faster deposit makes 6B build like 2B and collapses the
        // instrument distinction. Instead: the opacity CURVE FLOOR rises
        // 0.25 -> 0.4, so soft graphite BITES DARK at first touch (the
        // felt "darker", and physically true of a 6B), the ceiling rises
        // to 0.97 for deeper blacks, and the texture gap to 2B widens in
        // the same move (coarser scale 48 -> 56, noise 0.22 -> 0.26).
        // v3 (MEASURED, 2026-08-30): v2's flow-0.18 theory was WRONG in
        // the direction that mattered - the probe showed 2B out-darkens
        // 6B at EVERY pressure and pass (up to 63/255), because the 6B
        // STAMP is far sparser than the 2B stamp (per-dab coverage the
        // settings never show; see HANDOFF "stamp sparsity"). The
        // ceilings never matter: nothing approaches them in real
        // drawing. Flow 0.45 compensates for the sparse stamp - probe
        // ratios 6B/2B = 1.86 / 1.58 / 1.21 across pressures.
        b.setOpacity(0.97);
        b.setFlow(0.45);
        b.setGrainDepth(0.70);
        b.setGrainScale(56.0); // coarsest graphite tooth of the ladder
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.26);      // crumbly dry edges
        b.setAngleJitter(0.10);
        b.setSpacing(0.06);
        b.setTiltAffectsShape(true); b.setMaxTiltElongation(3.0);
        // Deeper size swell than sketchBase: light touch is a whisper,
        // pressure blooms the stroke.
        b.sizePressureCurve().setControlPoints(curve2(0.2, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.4, 1.0));
    });
    r << make(kSketching, QStringLiteral("Mechanical Pencil"),
              [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP, batch two (2026-08-30): consistency over character -
        // the batch-one feedback does not reach this one (unchanged from
        // the v1 table). Highest flow of the set: no build-y behaviour,
        // just a dependable line.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_mech_tip.png")));
        b.setSize(25); // family default (2026-08-30); the fixed-width
                       // curve and near-zero grain ARE the mechanical
                       // identity - they hold at any size
        b.setHardness(0.88); // INERT with a custom tip (see 4H)
        b.setOpacity(0.65);
        b.setFlow(0.50);
        b.setGrainDepth(0.12);
        b.setGrainScale(40.0);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.04);
        b.setSpacing(0.06); // the smoothest line of the family
        b.sizePressureCurve().setControlPoints(curve2(1.0, 1.0)); // fixed width
        b.setTiltAffectsShape(false); // the lead is clutched: no shoulder
    });
    r << make(kSketching, QStringLiteral("Blue Pencil"), [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP, batch two (2026-08-30): animation blue-pencil -
        // light ceiling, deeper-than-graphite grain for the visible
        // texture the intent asks for. Unchanged from the v1 table.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_blue_tip.png")));
        b.setSize(25); // family default (2026-08-30)
        b.setHardness(0.60); // INERT with a custom tip (see 4H)
        b.setOpacity(0.45);
        b.setFlow(0.30);
        b.setGrainDepth(0.40);
        b.setGrainScale(40.0);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.12);
        b.setAngleJitter(0.05);
        b.setColor(QColor(0x4a, 0x90, 0xd9)); // non-photo blue
    });
    r << make(kSketching, QStringLiteral("Charcoal Pencil"),
              [&](::Brush &b) {
        sketchBase(b);
        // STAMP TIP, batch two (2026-08-30): the 6B v2 direction taken
        // further - the roughest of the family. Heavy Noise is the
        // breakup lever now that hardness is inert; the opacity floor
        // matches v2 6B (charcoal bites dark immediately).
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/pencil_charcoal_tip.png")));
        b.setSize(25); // family default (2026-08-30); Ch/6B deposit
                       // ratios re-measured at this size - see HANDOFF
        b.setHardness(0.45); // INERT with a custom tip (see 4H)
        b.setOpacity(0.92);
        // v2 (MEASURED, 2026-08-30, "should be darker"): the stamp
        // census generalised the 6B sparsity finding - the charcoal scan
        // deposits 29/255 mean coverage per dab (vs 2B's 47) AND the
        // heaviest grain (depth 0.75) eats each deposit; at the old flow
        // 0.25 the full-pressure stroke measured 36/255 against 6B's 104.
        // RE-MEASURED AT SIZE 25 (the family default changed effective
        // per-dab coverage - stamp downsampling is part of the sparsity
        // mechanism, see HANDOFF): the size-6-calibrated flow 0.85
        // over-shot to Ch/6B 2.18 at LIGHT pressure, violating the
        // approved skate-on-the-tooth character. This recipe restores the
        // approved shape at size 25: Ch/6B = 0.90 / 0.94 / 1.06 across
        // pressures - skates light, parity mid, out-darks 6B leaned on.
        // The roughness levers (grain, noise, jitters, the crumbly stamp)
        // remain the identity, untouched.
        b.setFlow(0.50);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setGrainDepth(0.75);
        b.setGrainScale(64.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.30);
        b.setAngleJitter(0.15);
        b.setRoundnessJitter(0.15);
        b.setSpacing(0.07);
        // Size-25 shape levers (measured with the flow above): the size
        // floor thins light strokes, the opacity floor lightens them -
        // together they put the light end BELOW 6B where charcoal skates.
        b.sizePressureCurve().setControlPoints(curve2(0.22, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.28, 1.0));
    });

    // ---- DRAWING: committed, opaque, chunky media; Static grain ---------
    auto drawBase = [](::Brush &b) {
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setSpacing(0.1);
        b.setOpacity(1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.7, 1.0));
    };
    // Drawing batch two (2026-09-04): the seven remaining stamps, each
    // MEASURED at its approved size (probe: 160-pt stroke, 1000x240,
    // core band +/-4 px; figures are light-pressure first-touch /
    // full-pressure / interior spread p95-p5 / where the light stroke
    // ends in px from the spine). Census: every stamp has a HARD rim
    // (50%->10% within 0.025R) and a granular interior (pixel std ~=
    // mean), so a frayed edge can only come from the Soft Pastel
    // technique - pressure-INVERTED perpendicular scatter - and the
    // interior spread floors near ~60 whatever the grain depth. All
    // seven use the Charcoal grain preset (Chalk/Paper ceiling at
    // spread ~35). "Build-up" is compositing arithmetic on first-touch
    // density (HANDOFF), so it is reported, never targeted.
    r << make(kDrawing, QStringLiteral("Conte Crayon"), [&](::Brush &b) {
        drawBase(b);
        // The firm one of the conte trio: densest first touch, crispest
        // edge (ends at 11 px, one past the disc itself - scatter 0.20
        // is a hint of dryness, not a fray). MEASURED @25: 59.7 / 163.8
        // / spread 86 / ends 11.
        // RE-SCANNED 2026-09-04 (user's new 1536x1024 scan, rendered at
        // true aspect - the engine's tip-extent change): tuning UNCHANGED
        // by instruction; re-measured 71.5 / 153.5 / spread 77 / ends 8.
        // Deltas reported, not compensated - the hand decides.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_conte_tip.png")));
        b.setSize(25);
        b.setHardness(0.5); // INERT with a custom tip (see 4H Pencil)
        b.setTiltAffectsShape(true); // INERT with a custom tip: tilt
        b.setMaxTiltElongation(2.5); // elongation measured 39/39/38 px
        b.setOpacity(0.85);
        b.setFlow(0.65);
        b.setScatterPerpendicular(0.20);
        b.setScatterCount(1);
        b.scatterPressureCurve().setControlPoints(curve2(1.0, 0.15));
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.5);
        b.setGrainContrast(2.5);
        b.setGrainScale(40.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.5, 1.0));
    });
    r << make(kDrawing, QStringLiteral("Soft Pastel"), [&](::Brush &b) {
        drawBase(b);
        // STAMP TIP, Drawing calibration batch (2026-08-30, with
        // Compressed Charcoal and Grease Pencil); edge/tooth pass from
        // the hand-test verdict "soft-density mark with a hard
        // silhouette". The stamp's own rim IS hard (radial census:
        // 145->32->1 across 0.83R..0.98R, ~1-2 px at stroke scale), so
        // the fray comes from the ENGINE: pressure-INVERTED perpendicular
        // scatter (full fray at light pressure, dense tight spine at
        // full - a light pastel pass ends in sputter, a hard press lays
        // a solid mark) with scatterCount 2 refilling the thinned spine
        // and flow dropped 0.14 -> 0.10 to compensate the doubled dabs.
        // Tooth: Charcoal preset at depth 1.0 - the only preset that
        // opens real valleys (Chalk ceilings at spread ~35 even at
        // depth 1.0). MEASURED at size 35: first-touch 36.5 / build
        // x2.53 / full-pressure 108.9 (the confirmed-good build
        // character, preserved) with interior spread 25 -> 57 and the
        // cross-section cliff at 12 px replaced by a graded sputter
        // tail (occupancy 55/36/13/1% over dy 12-18, zero at 20). The
        // old roundnessJitter 0.2 stays dropped (unmeasured).
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_softpastel_tip.png")));
        b.setSize(35);
        b.setHardness(0.3); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.85);
        b.setFlow(0.10);
        b.setScatterPerpendicular(0.75);
        b.setScatterCount(2);
        b.scatterPressureCurve().setControlPoints(curve2(1.0, 0.15));
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(1.0);
        b.setGrainScale(48.0);
        b.setGrainContrast(3.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.12);
        b.setAngleJitter(0.12);
        b.sizePressureCurve().setControlPoints(curve2(0.4, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.35, 1.0));
    });
    r << make(kDrawing, QStringLiteral("Hard Pastel"), [&](::Brush &b) {
        drawBase(b);
        // Firmer than Soft Pastel on every axis the hand can read: 55%
        // denser first touch, near-full occupancy (99% vs 84%), a
        // shorter run-out (scatter 0.35 vs 0.75: ends 16 vs 19). Spread
        // 62 is the stamp's own interior (depth 0.45 only reached 58).
        // MEASURED @35: 56.6 / 125.1 / spread 62 / ends 16.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_hardpastel_tip.png")));
        b.setSize(35);
        b.setHardness(0.6); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.90);
        b.setFlow(0.28);
        b.setScatterPerpendicular(0.35);
        b.setScatterCount(1);
        b.scatterPressureCurve().setControlPoints(curve2(1.0, 0.15));
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.6);
        b.setGrainContrast(3.0);
        b.setGrainScale(48.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.5, 1.0));
    });
    r << make(kDrawing, QStringLiteral("Charcoal Stick"), [&](::Brush &b) {
        drawBase(b);
        // The airy half of the charcoal pair: rides the tooth (depth
        // 1.0, 85% occupancy, spread 92) and frays (scatter 0.50, ends
        // 17). Calibrated JOINTLY with Compressed Charcoal after the
        // hand-test read the old procedural Stick as darker: Compressed
        // now sits 1.76x above this at first touch and 1.64x at full
        // pressure. The old rotationAffectsShape is dropped, unmeasured.
        // MEASURED @35: 49.0 / 117.8 / spread 92 / ends 17.
        // RE-SCANNED 2026-09-04 (user's new 992x1585 scan, true aspect):
        // tuning UNCHANGED by instruction; re-measured 27.9 / 74.7 /
        // spread 65 / ends 17 - the tall narrow scan deposits far less
        // per dab. Pair polarity vs Compressed now 2.5x at first touch.
        // Deltas reported, not compensated - the hand decides.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_charstick_tip.png")));
        b.setSize(35);
        b.setHardness(0.35); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.95);
        b.setFlow(0.40);
        b.setScatterPerpendicular(0.50);
        b.setScatterCount(1);
        b.scatterPressureCurve().setControlPoints(curve2(1.0, 0.15));
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(1.0);
        b.setGrainContrast(3.0);
        b.setGrainScale(48.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.4, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.4, 1.0));
    });
    r << make(kDrawing, QStringLiteral("Compressed Charcoal"),
              [&](::Brush &b) {
        drawBase(b);
        // STAMP TIP, Drawing calibration batch (2026-08-30). The
        // dense/dark half of the charcoal pair. Grain depth deliberately
        // DOWN from the old 0.95: compressed charcoal fills the tooth
        // where the stick rides it - the pair's distinction lives in
        // deposit rate and breakup, not darkness alone. Flow RAISED
        // 0.55 -> 0.70 in batch two (2026-09-04), calibrated jointly
        // against the NEW stamped Charcoal Stick: the hand-test had
        // read the old procedural Stick as darker, and a technically
        // correct 1.48x margin was judged not enough - 0.70 puts this
        // 1.76x above the Stick at first touch (86.0 vs 49.0) and
        // 1.64x at full pressure (192.8 vs 117.8), with a hard filled
        // edge (99% occupancy, ends 10) against the Stick's fray.
        // MEASURED @30: 86.0 / 192.8 / spread 96 / ends 10.
        // RE-SCANNED 2026-09-04 (user's new 1024x1536 scan, true aspect):
        // tuning UNCHANGED by instruction; re-measured 70.2 / 169.6 /
        // spread 89 / ends 11. Deltas reported, not compensated.
        b.setCustomShape(QImage(
            QStringLiteral(":/brushes/draw_compcharcoal_tip.png")));
        b.setSize(30);
        b.setHardness(0.25); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.97);
        b.setFlow(0.70);
        b.setSpacing(0.08);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.6);
        b.setGrainScale(48.0);
        b.setGrainContrast(2.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.12);
        b.setAngleJitter(0.08);
        b.sizePressureCurve().setControlPoints(curve2(0.35, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.35, 1.0));
    });
    r << make(kDrawing, QStringLiteral("Chalk"), [&](::Brush &b) {
        drawBase(b);
        // Inherits the FULL Soft Pastel technique (scatter 0.75, count
        // 2, depth 1.0): dry powder that runs out. Distinct from Soft
        // Pastel by a brighter first touch (45 vs 36.5) and a more
        // broken tooth (spread 79 vs 57).
        // NO IDENTITY COLOUR (2026-09-05): the #f2f2f2 white it used to
        // carry was adopted while active and drew near-white on white
        // paper - invisible (max contrast 13/255 even at full alpha; at
        // the measured light-pressure alpha of 58/255, 3/255). Chalk is
        // defined by being DRY, not by being white: it now inherits the
        // user's colour like the other dry media, and white chalk is a
        // colour the user picks. Sanguine and Sepia keep theirs - the
        // colour IS the medium there.
        // MEASURED @30: 45.2 / 129.9 / spread 79 / ends 18.
        // RE-SCANNED 2026-09-04 (user's new 1254x1254 scan): tuning
        // UNCHANGED by instruction; re-measured 58.1 / 145.2 / spread 95
        // / ends 18. Deltas reported, not compensated.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_chalk_tip.png")));
        b.setSize(30);
        b.setHardness(0.4); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.90);
        b.setFlow(0.28);
        b.setScatterPerpendicular(0.75);
        b.setScatterCount(2);
        b.scatterPressureCurve().setControlPoints(curve2(1.0, 0.15));
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(1.0);
        b.setGrainContrast(3.0);
        b.setGrainScale(48.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.4, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.35, 1.0));
    });
    r << make(kDrawing, QStringLiteral("Graphite Block"), [&](::Brush &b) {
        drawBase(b);
        // A broad even band, no scatter: distinct from the graphite
        // pencils by width (40 vs 25), evenness (98% occupancy vs 6B's
        // 86%) and a darker laydown at both pressures. MEASURED STAMP
        // LIMITATION (item-10 clause, milder than Grease): the "even
        // sheen" target of spread 20-35 is unreachable - it floors at
        // ~57-60 with grain nearly off and spacing halved, because it
        // is the scan's own interior texture. User chose to draw it
        // rather than re-scan on a prediction. The old flatTip and the
        // tilt widening are gone with the stamp: tilt elongation is
        // INERT with a custom tip (measured 39/39/38 px), like
        // hardness. MEASURED @40: 51.3 / 139.4 / spread 60 / ends 17.
        b.setCustomShape(QImage(
            QStringLiteral(":/brushes/draw_graphiteblock_tip.png")));
        b.setSize(40);
        b.setHardness(0.55); // INERT with a custom tip (see 4H Pencil)
        b.setTiltAffectsShape(true); // INERT with a custom tip (see
        b.setMaxTiltElongation(4.0); // above); kept as the recipe intent
        b.setOpacity(0.85);
        b.setFlow(0.30);
        b.setSpacing(0.08);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.3);
        b.setGrainContrast(2.0);
        b.setGrainScale(30.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.7, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.4, 1.0));
    });
    r << make(kDrawing, QStringLiteral("Grease Pencil"), [&](::Brush &b) {
        drawBase(b);
        // STAMP TIP, Drawing calibration batch (2026-08-30). MEASURED at
        // the approved size 25: mean 165.5 at half pressure, fast waxy
        // laydown (buildup x1.46), full-pressure 228 - the sparse
        // 20/255-coverage stamp (the census outlier) compensated with
        // flow 0.85 and the family's tightest spacing. A MEASURED STAMP
        // LIMITATION is on record (item-10 clause, HANDOFF): "solid,
        // consistent" asked for tooth spread <= 8, but the scan's own
        // crumbly texture floors at ~71 with grain fully OFF - spacing
        // 0.03 averages it down to ~77 and no setting can go lower. If
        // the hand-test wants it smoother, the fix is a denser re-scan,
        // not another tuning round.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_grease_tip.png")));
        b.setSize(25);
        b.setHardness(0.45); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.95);
        b.setFlow(0.85);
        b.setSpacing(0.03);
        b.setGrainPreset(B::GrainPreset::Paper);
        b.setGrainDepth(0.15);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.setNoise(0.03);
        b.sizePressureCurve().setControlPoints(curve2(0.7, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.flowPressureCurve().setControlPoints(curve2(1.0, 1.0)); // waxy
    });
    // Sanguine and Sepia are the softer two of the conte trio (scatter
    // 0.35, ends 12, ~52 first touch vs Conte's 59.7). Their flows
    // DIFFER (0.40 vs 0.48) only to cancel the stamps' coverage
    // difference (50.2 vs 43.5): they are meant to land at the same
    // density, and colour is their distinction (identity colours ride
    // the shipped design-(b) behaviour).
    r << make(kDrawing, QStringLiteral("Sanguine"), [&](::Brush &b) {
        drawBase(b);
        // MEASURED @25: 51.6 / 125.5 / spread 67 / ends 12.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_sanguine_tip.png")));
        b.setSize(25);
        b.setHardness(0.5); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.90);
        b.setFlow(0.40);
        b.setScatterPerpendicular(0.35);
        b.setScatterCount(1);
        b.scatterPressureCurve().setControlPoints(curve2(1.0, 0.15));
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.6);
        b.setGrainContrast(2.5);
        b.setGrainScale(40.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.setColor(QColor(0xc0, 0x5a, 0x3a));
    });
    r << make(kDrawing, QStringLiteral("Sepia"), [&](::Brush &b) {
        drawBase(b);
        // MEASURED @25: 52.1 / 135.5 / spread 69 / ends 12.
        // RE-SCANNED 2026-09-04 (user's new 1774x887 scan, true aspect -
        // the widest of the set, half as tall as it is wide): tuning
        // UNCHANGED by instruction; re-measured 56.2 / 158.8 / spread 90
        // / ends 8. Deltas reported, not compensated.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/draw_sepia_tip.png")));
        b.setSize(25);
        b.setHardness(0.5); // INERT with a custom tip (see 4H Pencil)
        b.setOpacity(0.90);
        b.setFlow(0.48);
        b.setScatterPerpendicular(0.35);
        b.setScatterCount(1);
        b.scatterPressureCurve().setControlPoints(curve2(1.0, 0.15));
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainDepth(0.6);
        b.setGrainContrast(2.5);
        b.setGrainScale(40.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.setColor(QColor(0x70, 0x42, 0x1e));
    });

    // ---- INKING: hard, tight spacing, steep size curves ------------------
    auto inkBase = [](::Brush &b) {
        b.setHardness(1.0);
        b.setSpacing(0.04);
        b.setOpacity(1.0);
        b.setFlow(1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.2, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(1.0, 1.0));
    };
    r << make(kInking, QStringLiteral("Studio Pen"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(8);
        b.sizePressureCurve().setControlPoints(
            {{0.0, 0.15}, {0.6, 0.7}, {1.0, 1.0}});
    });
    r << make(kInking, QStringLiteral("Technical Pen"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(4);
        b.sizePressureCurve().setControlPoints(curve2(1.0, 1.0)); // uniform
        b.setTiltAffectsShape(false); // a technical pen never elongates
    });
    // Inking Part B (2026-09-05): scanned stamps, measured per brush at
    // the approved sizes (probe: 160-pt line, 1000x240, core rows +/-2;
    // "edge" = column-averaged 90->10 width / per-column transition
    // width; taper = ramped-pressure stroke, slow AND fast). Census:
    // seven of the eight scans are near-binary (0-6% midtones) and every
    // one measures a 1 px edge at its size - crisp is stamp-supplied, not
    // tuned. None trips the hard-edge CPU/GPU fault (<= 1/255, 0.000%).
    // The taper lever is the size-pressure FLOOR, set PER BRUSH here -
    // inkBase keeps its 0.2 so nothing outside the stamped brushes
    // changes. Tapers were verified fast (25 px point gaps) as well as
    // slow: the resampler interpolates pressure, so the two agree.
    r << make(kInking, QStringLiteral("Brush Pen"), [&](::Brush &b) {
        inkBase(b);
        // Clean and solid, the taper is the whole point. MEASURED @14:
        // full-pressure core 253 / occupancy 100% / edge 1 px (per-column
        // 0.0); ramped stroke starts and ends at 1 px, no pen-down or
        // lift blob, slow and fast alike (floor 0.05 and 0.08 gave 2 px
        // blobs; the old 0.08 floor is what blunted the start). Spacing
        // 0.03 kept.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/ink_brushpen_tip.png")));
        b.setSize(14);
        b.setSpacing(0.03);
        b.sizePressureCurve().setControlPoints(curve2(0.02, 1.0));
    });
    r << make(kInking, QStringLiteral("Calligraphy"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(16);
        b.setCustomShape(flatTip(0.22));
        b.setRotationAffectsShape(true);
    });
    r << make(kInking, QStringLiteral("Dry Ink"), [&](::Brush &b) {
        inkBase(b);
        // Rough and broken but CRISP. The scan alone is not broken at
        // full pressure - overlapping dabs at any spacing union into a
        // solid line (occupancy 100%) - and sparse spacing only makes a
        // dab-string. The breakup comes from Charcoal grain at depth 1.0
        // in STATIC canvas mode: fixed paper holes punched through every
        // dab (Rolling grain averages across dabs into a soft 1 px
        // transition; Static stays binary). MEASURED @12: full-pressure
        // core 155 / occupancy 65% / per-column edge 0.3 px (the 4 px
        // column-averaged width is the skips, not blur); half pressure
        // occupancy 27% - a dry brush skating; taper from the true start
        // 1/1/1/1 px, no blob, slow and fast.
        b.setCustomShape(
            QImage(QStringLiteral(":/brushes/ink_dryink_tip.png")));
        b.setSize(12);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setGrainDepth(1.0);
        b.setGrainContrast(3.0);
        b.setGrainScale(30.0);
        b.setControlMinimum(B::DynamicProperty::GrainDepth, 1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.02, 1.0));
    });
    // Renamed from "Ink Bleed" (2026-09-05), id kept - see the keptId
    // make() overload above.
    r << make(kInking, QStringLiteral("Rich Ink"),
              QStringLiteral("builtin/inking/ink-bleed"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(10); b.setHardness(0.55);
        b.opacityPressureCurve().setControlPoints(
            {{0.0, 0.4}, {0.5, 0.9}, {1.0, 1.0}});
    });
    r << make(kInking, QStringLiteral("Fine Liner"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(2);
        b.sizePressureCurve().setControlPoints(curve2(1.0, 1.0));
        b.setTiltAffectsShape(false); // felt tip: constant width by design
    });
    r << make(kInking, QStringLiteral("Ink Line & Splatter"),
              [&](::Brush &b) {
        inkBase(b);
        b.setSize(8);
        b.setDualBrushEnabled(true);
        ::Brush &s = b.secondaryBrush();
        s.setSize(6); s.setHardness(1.0); s.setSpacing(0.5);
        s.setCustomShape(dotTip());
        s.setScatterAlong(0.6); s.setScatterPerpendicular(0.8);
        s.setScatterCount(3); s.setSizeJitter(0.6);
        b.setDualBlendMode(B::DualBlendMode::NormalOver);
        b.setDualMasterOpacity(0.9);
    });
    r << make(kInking, QStringLiteral("Splatter"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(10); b.setSpacing(0.5);
        b.setScatterAlong(0.8); b.setScatterPerpendicular(0.6);
        b.setScatterCount(4); b.setSizeJitter(0.6);
    });
    r << make(kInking, QStringLiteral("Marker"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(18); b.setHardness(0.35); b.setOpacity(0.6);
        b.opacityPressureCurve().setControlPoints(curve2(1.0, 1.0));
        // Our marker is the round-tip kind whose look IS the constant juicy
        // line (a chisel marker would keep tilt, but that isn't this brush).
        b.setTiltAffectsShape(false);
    });

    // ---- PAINTING: opaque wet media --------------------------------------
    auto paintBase = [](::Brush &b) {
        b.setSpacing(0.06);
        b.setFlow(0.85);
        b.sizePressureCurve().setControlPoints(curve2(0.6, 1.0));
        b.flowPressureCurve().setControlPoints(curve2(0.4, 1.0));
    };
    r << make(kPainting, QStringLiteral("Round Brush"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(20); b.setHardness(0.45);
    });
    r << make(kPainting, QStringLiteral("Flat Brush"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(24); b.setHardness(0.6);
        b.setCustomShape(flatTip(0.35));
        b.setRotationAffectsShape(true);
    });
    r << make(kPainting, QStringLiteral("Filbert"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(22); b.setHardness(0.5);
        b.setCustomShape(ovalTip(0.55));
        b.setRotationAffectsShape(true);
    });
    r << make(kPainting, QStringLiteral("Bristle"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(22); b.setHardness(0.5);
        b.setDualBrushEnabled(true);
        ::Brush &s = b.secondaryBrush();
        s.setSize(22); s.setSpacing(0.08);
        s.setCustomShape(streakTip());
        s.setRotationAffectsShape(true);
        b.setDualBlendMode(B::DualBlendMode::LinearBurn);
        b.setDualMasterOpacity(0.9);
    });
    r << make(kPainting, QStringLiteral("Dry Brush"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(20); b.setHardness(0.55); b.setFlow(0.5);
        b.setGrainPreset(B::GrainPreset::Canvas);
        b.setGrainMode(B::GrainMode::Rolling);
        b.setGrainDepth(0.65);
    });
    r << make(kPainting, QStringLiteral("Gouache"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(18); b.setHardness(0.4); b.setOpacity(0.95);
        b.setGrainPreset(B::GrainPreset::Canvas); b.setGrainDepth(0.3);
        // PROMOTED from the user's override (tuned 2026-08-01, promoted
        // 2026-08-28): a light pen paints at ~10% size instead of
        // paintBase's 60% - a much deeper pressure taper. Rounded from the
        // override's slider value 0.104167 by decision at promotion.
        b.sizePressureCurve().setControlPoints(curve2(0.1, 1.0));
    });
    r << make(kPainting, QStringLiteral("Acrylic"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(20); b.setHardness(0.55); b.setFlow(0.9);
        b.setGrainPreset(B::GrainPreset::Canvas); b.setGrainDepth(0.45);
    });
    r << make(kPainting, QStringLiteral("Oil Paint"), [&](::Brush &b) {
        paintBase(b);
        // Paint-mode: smudgeStrength is inert outside Smudge tool mode, so
        // the wet-oil character comes from heavy canvas grain + full flow.
        b.setSize(22); b.setHardness(0.5); b.setFlow(0.95);
        b.setGrainPreset(B::GrainPreset::Canvas); b.setGrainDepth(0.55);
        b.setGrainMode(B::GrainMode::Rolling);
    });
    r << make(kPainting, QStringLiteral("Blender"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(24); b.setHardness(0.35);
        b.setToolMode(B::ToolMode::Smudge); // pure blender -> RGBA16
        b.setSmudgeStrength(0.85);
        b.smudgePressureCurve().setControlPoints(curve2(0.4, 1.0));
    });
    r << make(kPainting, QStringLiteral("Smudge Soft"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(28); b.setHardness(0.2);
        b.setToolMode(B::ToolMode::Smudge); // -> RGBA16
        b.setSmudgeStrength(0.95);
    });
    r << make(kPainting, QStringLiteral("Large Airbrush"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(60); b.setHardness(0.08); b.setFlow(0.25);
        b.setSpacing(0.05);
        b.opacityPressureCurve().setControlPoints(curve2(0.15, 0.8));
    });
    r << make(kPainting, QStringLiteral("Palette Knife"), [&](::Brush &b) {
        paintBase(b);
        b.setSize(26); b.setHardness(0.9); b.setSpacing(0.12);
        b.setCustomShape(flatTip(0.16));
        b.setRotationAffectsShape(true);
    });

    // ---- ARTISTIC: texture & FX ------------------------------------------
    auto artBase = [](::Brush &b) {
        b.setSpacing(0.1);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
    };
    r << make(kArtistic, QStringLiteral("Rounded Square"), [&](::Brush &b) {
        artBase(b);
        b.setSize(20); b.setHardness(0.9);
        b.setCustomShape(squareTip());
        b.setRotationAffectsShape(true);
        b.setTiltAffectsShape(false); // tilt elongation would break the
    });                               // geometric identity of the stamp
    r << make(kArtistic, QStringLiteral("Dynamic Stroke"), [&](::Brush &b) {
        artBase(b);
        b.setSize(16); b.setHardness(0.7);
        b.setSizeJitter(0.5); b.setAngleJitter(0.4);
        b.setSpacingJitter(0.3);
    });
    r << make(kArtistic, QStringLiteral("Stipple"), [&](::Brush &b) {
        artBase(b);
        b.setSize(8); b.setHardness(1.0); b.setSpacing(0.8);
        b.setScatterAlong(0.5); b.setScatterPerpendicular(0.5);
        b.setScatterCount(3); b.setSizeJitter(0.4);
    });
    r << make(kArtistic, QStringLiteral("Hatching"), [&](::Brush &b) {
        artBase(b);
        b.setSize(24); b.setHardness(0.8);
        b.setCustomShape(hatchTip());
    });
    r << make(kArtistic, QStringLiteral("Confetti"), [&](::Brush &b) {
        artBase(b);
        b.setSize(12); b.setHardness(0.9); b.setSpacing(0.6);
        b.setScatterAlong(0.7); b.setScatterPerpendicular(0.9);
        b.setScatterCount(4);
        b.setHueJitter(0.6); b.setSaturationJitter(0.3); // -> RGBA16
        b.setSizeJitter(0.5); b.setAngleJitter(0.5);
    });
    r << make(kArtistic, QStringLiteral("Chromatic"), [&](::Brush &b) {
        artBase(b);
        b.setSize(18); b.setHardness(0.5);
        b.setHueJitter(0.25); // -> RGBA16
    });
    r << make(kArtistic, QStringLiteral("Glitch"), [&](::Brush &b) {
        artBase(b);
        b.setSize(20); b.setHardness(1.0);
        b.setDualBrushEnabled(true);
        ::Brush &s = b.secondaryBrush();
        s.setSize(20); s.setSpacing(0.15);
        s.setCustomShape(barTip());
        s.setAngleJitter(0.2);
        b.setDualBlendMode(B::DualBlendMode::Subtract);
        b.setDualMasterOpacity(0.8);
    });
    r << make(kArtistic, QStringLiteral("Halftone"), [&](::Brush &b) {
        artBase(b);
        b.setSize(26); b.setHardness(0.7);
        b.setCustomGrain(halftoneGrain());
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setGrainDepth(0.9);
    });
    r << make(kArtistic, QStringLiteral("Sparkle"), [&](::Brush &b) {
        artBase(b);
        b.setSize(14); b.setHardness(0.9); b.setSpacing(0.5);
        b.setCustomShape(starTip());
        b.setScatterAlong(0.6); b.setScatterPerpendicular(0.7);
        b.setScatterCount(3);
        b.setBrightnessJitter(0.5); // -> RGBA16
        b.setSizeJitter(0.6); b.setAngleJitter(0.5);
    });
    r << make(kArtistic, QStringLiteral("Ribbon"), [&](::Brush &b) {
        artBase(b);
        b.setSize(22); b.setHardness(0.85); b.setSpacing(0.05);
        b.setCustomShape(flatTip(0.2));
        b.setRotationAffectsShape(true);
        b.setRoundnessJitter(0.25);
    });

    // ---- WATERCOLOR: translucent wet washes ------------------------------
    auto waterBase = [](::Brush &b) {
        b.setGrainPreset(B::GrainPreset::Paper);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setSpacing(0.06);
        b.setFlow(0.9);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.3, 0.9));
    };
    r << make(kWatercolor, QStringLiteral("Wet Round"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(22); b.setHardness(0.15); b.setOpacity(0.35);
    });
    r << make(kWatercolor, QStringLiteral("Wet Flat"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(26); b.setHardness(0.2); b.setOpacity(0.35);
        b.setCustomShape(flatTip(0.4));
        b.setRotationAffectsShape(true);
    });
    r << make(kWatercolor, QStringLiteral("Soft Wash"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(40); b.setHardness(0.1); b.setOpacity(0.2);
    });
    r << make(kWatercolor, QStringLiteral("Granulating Wash"),
              [&](::Brush &b) {
        waterBase(b);
        b.setSize(32); b.setHardness(0.15); b.setOpacity(0.3);
        b.setGrainDepth(0.6);
        b.setGrainAffectsColor(true); // pigment granulation -> RGBA16
    });
    r << make(kWatercolor, QStringLiteral("Wet-on-Wet"), [&](::Brush &b) {
        waterBase(b);
        // A true Smudge-mode blender (the engine's one wet-drag mechanism):
        // pulls existing washes around like water. Softer and larger than
        // Painting's Blender, with paper grain breaking the drag. -> RGBA16
        b.setSize(28); b.setHardness(0.12);
        b.setToolMode(B::ToolMode::Smudge);
        b.setSmudgeStrength(0.55);
        b.smudgePressureCurve().setControlPoints(curve2(0.3, 1.0));
        b.setGrainDepth(0.3);
    });
    r << make(kWatercolor, QStringLiteral("Bleed Edge"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(24); b.setHardness(0.18); b.setOpacity(0.32);
        b.setGrainDepth(0.45);
        b.setGrainAffectsColor(true); // -> RGBA16
        b.opacityPressureCurve().setControlPoints(
            {{0.0, 0.15}, {0.5, 0.5}, {1.0, 0.95}});
    });
    r << make(kWatercolor, QStringLiteral("Dry Wash"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(26); b.setHardness(0.25); b.setOpacity(0.4);
        b.setFlow(0.6);
        b.setGrainPreset(B::GrainPreset::Canvas);
        b.setGrainMode(B::GrainMode::Rolling);
        b.setGrainDepth(0.5);
    });
    r << make(kWatercolor, QStringLiteral("Salt Texture"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(30); b.setHardness(0.15); b.setOpacity(0.3);
        b.setCustomGrain(saltGrain());
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setGrainDepth(0.8);
        b.setGrainAffectsColor(true); // -> RGBA16
    });
    r << make(kWatercolor, QStringLiteral("Spatter Wash"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(16); b.setHardness(0.3); b.setOpacity(0.3);
        b.setSpacing(0.45);
        b.setScatterAlong(0.7); b.setScatterPerpendicular(0.6);
        b.setScatterCount(3); b.setSizeJitter(0.5);
    });
    r << make(kWatercolor, QStringLiteral("Detail Round"), [&](::Brush &b) {
        waterBase(b);
        b.setSize(6); b.setHardness(0.3); b.setOpacity(0.45);
    });

    // The Watercolor CATEGORY is retired; its ten brushes live on under
    // Painting. The entries above are still built with kWatercolor so their
    // IDs keep the original "builtin/watercolor/<name>" slug — the id is
    // opaque, and favourites / Recent / hidden / overrides all reference
    // ids, so no user shelf state or override needs migrating. Only the
    // display CATEGORY is remapped here. Entry ORDER is also unchanged,
    // which is why the combined preview SHA in SankoBrushLibraryTest does
    // not move (it hashes preview images in roster order, and previews
    // depend only on the brush).
    for (BrushPreset &p : r)
        if (p.category == kWatercolor)
            p.category = kPainting;

    return r;
}

} // namespace brushlib

