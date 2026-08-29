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
        b.setSize(3); b.setHardness(0.70); b.setOpacity(0.55);
        b.setGrainDepth(0.30);
        b.opacityPressureCurve().setControlPoints(curve2(0.35, 0.9));
    });
    r << make(kSketching, QStringLiteral("2H Pencil"), [&](::Brush &b) {
        sketchBase(b);
        b.setSize(3); b.setHardness(0.78); b.setOpacity(0.40);
        b.setGrainDepth(0.25);
        b.sizePressureCurve().setControlPoints(curve2(0.7, 1.0));
    });
    r << make(kSketching, QStringLiteral("4H Pencil"), [&](::Brush &b) {
        sketchBase(b);
        b.setSize(2); b.setHardness(0.85); b.setOpacity(0.28);
        b.setGrainDepth(0.18);
        b.sizePressureCurve().setControlPoints(curve2(0.85, 1.0));
    });
    r << make(kSketching, QStringLiteral("2B Pencil"), [&](::Brush &b) {
        sketchBase(b);
        b.setSize(5); b.setHardness(0.52); b.setOpacity(0.85);
        b.setGrainDepth(0.45);
        b.sizePressureCurve().setControlPoints(curve2(0.25, 1.0));
    });
    r << make(kSketching, QStringLiteral("4B Pencil"), [&](::Brush &b) {
        sketchBase(b);
        b.setSize(6); b.setHardness(0.42); b.setOpacity(0.92);
        b.setGrainDepth(0.55);
        b.setTiltAffectsShape(true); b.setMaxTiltElongation(2.2);
    });
    r << make(kSketching, QStringLiteral("6B Pencil"), [&](::Brush &b) {
        sketchBase(b);
        b.setSize(8); b.setHardness(0.34); b.setOpacity(0.97);
        b.setGrainDepth(0.65); b.setSpacing(0.06);
        b.setTiltAffectsShape(true); b.setMaxTiltElongation(3.0);
    });
    r << make(kSketching, QStringLiteral("Mechanical Pencil"),
              [&](::Brush &b) {
        sketchBase(b);
        b.setSize(2); b.setHardness(0.88); b.setOpacity(0.8);
        b.setGrainDepth(0.15);
        b.sizePressureCurve().setControlPoints(curve2(1.0, 1.0)); // fixed width
        b.setTiltAffectsShape(false); // the lead is clutched: no shoulder
    });
    r << make(kSketching, QStringLiteral("Blue Pencil"), [&](::Brush &b) {
        sketchBase(b);
        b.setSize(4); b.setHardness(0.60); b.setOpacity(0.5);
        b.setGrainDepth(0.30);
        b.setColor(QColor(0x4a, 0x90, 0xd9)); // non-photo blue
    });
    r << make(kSketching, QStringLiteral("Charcoal Pencil"),
              [&](::Brush &b) {
        sketchBase(b);
        b.setSize(6); b.setHardness(0.45); b.setOpacity(0.9);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setGrainDepth(0.7); b.setRoundnessJitter(0.15);
    });

    // ---- DRAWING: committed, opaque, chunky media; Static grain ---------
    auto drawBase = [](::Brush &b) {
        b.setGrainMode(B::GrainMode::StaticCanvas);
        b.setSpacing(0.1);
        b.setOpacity(1.0);
        b.sizePressureCurve().setControlPoints(curve2(0.5, 1.0));
        b.opacityPressureCurve().setControlPoints(curve2(0.7, 1.0));
    };
    r << make(kDrawing, QStringLiteral("Conte Crayon"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(12); b.setHardness(0.5);
        b.setGrainPreset(B::GrainPreset::Chalk); b.setGrainDepth(0.6);
        b.setTiltAffectsShape(true); b.setMaxTiltElongation(2.5);
    });
    r << make(kDrawing, QStringLiteral("Soft Pastel"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(18); b.setHardness(0.3); b.setFlow(0.8);
        b.setGrainPreset(B::GrainPreset::Chalk); b.setGrainDepth(0.75);
        b.setRoundnessJitter(0.2);
    });
    r << make(kDrawing, QStringLiteral("Hard Pastel"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(14); b.setHardness(0.6);
        b.setGrainPreset(B::GrainPreset::Chalk); b.setGrainDepth(0.5);
    });
    r << make(kDrawing, QStringLiteral("Charcoal Stick"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(16); b.setHardness(0.35);
        b.setGrainPreset(B::GrainPreset::Charcoal); b.setGrainDepth(0.8);
        b.setRotationAffectsShape(true);
    });
    r << make(kDrawing, QStringLiteral("Compressed Charcoal"),
              [&](::Brush &b) {
        drawBase(b);
        b.setSize(20); b.setHardness(0.25);
        b.setGrainPreset(B::GrainPreset::Charcoal); b.setGrainDepth(0.95);
    });
    r << make(kDrawing, QStringLiteral("Chalk"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(14); b.setHardness(0.4);
        b.setGrainPreset(B::GrainPreset::Chalk); b.setGrainDepth(0.65);
        b.setColor(QColor(0xf2, 0xf2, 0xf2));
    });
    r << make(kDrawing, QStringLiteral("Graphite Block"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(24); b.setHardness(0.55);
        b.setGrainPreset(B::GrainPreset::Paper); b.setGrainDepth(0.5);
        b.setCustomShape(flatTip(0.28));
        b.setTiltAffectsShape(true); b.setMaxTiltElongation(4.0);
    });
    r << make(kDrawing, QStringLiteral("Grease Pencil"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(8); b.setHardness(0.45);
        b.setGrainPreset(B::GrainPreset::Paper); b.setGrainDepth(0.2);
        b.setFlow(0.95);
        b.flowPressureCurve().setControlPoints(curve2(1.0, 1.0)); // waxy
    });
    r << make(kDrawing, QStringLiteral("Sanguine"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(12); b.setHardness(0.5);
        b.setGrainPreset(B::GrainPreset::Chalk); b.setGrainDepth(0.55);
        b.setColor(QColor(0xc0, 0x5a, 0x3a));
    });
    r << make(kDrawing, QStringLiteral("Sepia"), [&](::Brush &b) {
        drawBase(b);
        b.setSize(12); b.setHardness(0.5);
        b.setGrainPreset(B::GrainPreset::Chalk); b.setGrainDepth(0.55);
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
    r << make(kInking, QStringLiteral("Brush Pen"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(14); b.setSpacing(0.03);
        b.sizePressureCurve().setControlPoints(curve2(0.08, 1.0));
    });
    r << make(kInking, QStringLiteral("Calligraphy"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(16);
        b.setCustomShape(flatTip(0.22));
        b.setRotationAffectsShape(true);
    });
    r << make(kInking, QStringLiteral("Dry Ink"), [&](::Brush &b) {
        inkBase(b);
        b.setSize(12);
        b.setGrainPreset(B::GrainPreset::Charcoal);
        b.setGrainMode(B::GrainMode::Rolling);
        b.setGrainDepth(0.5);
    });
    r << make(kInking, QStringLiteral("Ink Bleed"), [&](::Brush &b) {
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

