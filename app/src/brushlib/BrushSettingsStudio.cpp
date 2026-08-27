#include "SankoTheme.h"
#include "BrushSettingsStudio.h"

#include "BrushPresetCodec.h"
#include "ScratchCanvas.h"

#include <QApplication>
#include <QColorDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QUndoCommand>
#include <QVBoxLayout>

namespace brushlib {

using namespace studio;

namespace {

constexpr int kEdgeBand = 8;
const QString kGeoKey = QStringLiteral("storyboard/brushStudio/v1/geo");
const QString kStabKey = QStringLiteral("paint/v1/stabilization");

QString fmtPercent(double v)
{
    return QString::number(qRound(v * 100.0)) + QStringLiteral("%");
}
QString fmtPixels(double v)
{
    return QString::number(qRound(v)) + QStringLiteral(" px");
}
QString fmtMultiplier(double v)
{
    return QString::number(v, 'f', 1) + QChar(0x00D7);
}
QString fmtDegrees(double v)
{
    return QString::number(qRound(v)) + QChar(0x00B0);
}
QString fmtCount(double v)
{
    return QString::number(qRound(v));
}

// 40x26 thumbnail of the tip WITH the static transform applied — a
// miniature of the engine's documented per-stamp affine (backward map:
// rotate, compress tip-local X, flip the tip-local sample axes), so the
// four static numbers have a picture. Approved divergence: previously the
// thumbnail showed the untransformed custom shape.
QImage tipTransformThumbnail(const ::Brush &b)
{
    constexpr int kW = 40, kH = 26;
    QImage out(kW, kH, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    const qreal angle = qDegreesToRadians(b.tipAngle());
    const qreal cosine = std::cos(angle), sine = std::sin(angle);
    const qreal compression = b.tipRoundness();
    const qreal signX = b.tipFlipX() ? -1.0 : 1.0;
    const qreal signY = b.tipFlipY() ? -1.0 : 1.0;
    const QImage shape = b.customShape(); // null selects the procedural disc
    const qreal half = kH * 0.5;
    const QColor ink = kDragger; // #b3b3b3, the component ink
    for (int y = 0; y < kH; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < kW; ++x) {
            const qreal lx = (x - kW * 0.5 + 0.5) / half;
            const qreal ly = (y - kH * 0.5 + 0.5) / half;
            qreal tx = cosine * lx + sine * ly;
            const qreal ty = -sine * lx + cosine * ly;
            tx /= qMax(compression, 0.01);
            const qreal dist = std::hypot(tx, ty);
            if (dist >= 1.0)
                continue;
            qreal coverage = 0.0;
            if (!shape.isNull()) {
                const qreal u = signX * tx * 0.5 + 0.5;
                const qreal v = signY * ty * 0.5 + 0.5;
                const int sx = qBound(0, int(u * shape.width()),
                                      shape.width() - 1);
                const int sy = qBound(0, int(v * shape.height()),
                                      shape.height() - 1);
                coverage = shape.constScanLine(sy)[sx] / 255.0;
            } else if (b.hardness() >= 0.999 || dist <= b.hardness()) {
                coverage = 1.0;
            } else {
                const qreal t = (dist - b.hardness())
                    / qMax(1.0 - b.hardness(), 0.001);
                coverage = std::exp(-3.0 * t * t) * (1.0 - t);
            }
            const int a = qRound(coverage * 255.0);
            row[x] = qPremultiply(
                qRgba(ink.red(), ink.green(), ink.blue(), a));
        }
    }
    return out;
}

QImage loadImageFile(QWidget *parent, const QString &title)
{
    const QString path = QFileDialog::getOpenFileName(
        parent, title, QString(),
        QObject::tr("Images (*.png *.jpg *.jpeg *.bmp)"));
    if (path.isEmpty())
        return QImage();
    return QImage(path);
}

} // namespace

// Sidebar section row (Figma 274:27..65): 37px, radius 6; active = the
// accent pill with white semi-bold text, idle = dim medium text.
class StudioSectionRow : public QWidget
{
public:
    StudioSectionRow(const QString &name, std::function<void()> onClick,
                     QWidget *parent = nullptr)
        : QWidget(parent)
        , m_name(name)
        , m_onClick(std::move(onClick))
    {
        setFixedHeight(37);
        setCursor(Qt::PointingHandCursor);
    }
    void setActive(bool active)
    {
        m_active = active;
        update();
    }
    void setDimmed(bool dimmed)
    {
        m_dimmed = dimmed;
        setCursor(dimmed ? Qt::ArrowCursor : Qt::PointingHandCursor);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setOpacity(m_dimmed ? 0.35 : 1.0);
        if (m_active) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(kAccent);
            p.drawRoundedRect(rect(), 6, 6);
        }
        QFont f(QStringLiteral("Inter"));
        f.setPixelSize(14);
        f.setWeight(m_active ? QFont::DemiBold : QFont::Medium);
        p.setFont(f);
        p.setPen(m_active ? QColor(Qt::white) : kTextDim);
        p.drawText(rect().adjusted(12, 0, -12, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, m_name);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && !m_dimmed && m_onClick)
            m_onClick();
    }

private:
    QString m_name;
    std::function<void()> m_onClick;
    bool m_active = false;
    bool m_dimmed = false;
};

// Identity-colour row (General): label + swatch capsule. Black is the
// "follow the app colour" sentinel from Phase 3, shown as text.
class StudioColorRow : public QWidget
{
public:
    // appColourWhenBlack: the identity-colour row renders black as the
    // "App colour" pill (Phase 3 semantics); rows whose colour is always
    // literal (the 6c background colour) pass false.
    StudioColorRow(const QString &label, std::function<void()> onClick,
                   QWidget *parent = nullptr, bool appColourWhenBlack = true)
        : QWidget(parent)
        , m_label(label)
        , m_onClick(std::move(onClick))
        , m_appColourWhenBlack(appColourWhenBlack)
    {
        setFixedHeight(23);
        setCursor(Qt::PointingHandCursor);
    }
    void setColor(const QColor &color)
    {
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setFont(labelFont());
        p.setPen(kTextDim);
        p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, m_label);
        p.setRenderHint(QPainter::Antialiasing, true);
        if (m_appColourWhenBlack && m_color == QColor(Qt::black)) {
            paintCapsule(p, QRect(width() - 82, 0, 82, 23),
                         QStringLiteral("App colour"));
        } else {
            const QRect r(width() - 55, 0, 55, 23);
            p.setPen(QPen(kBorder, 1));
            p.setBrush(m_color);
            p.drawRoundedRect(r, 11.5, 11.5);
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_onClick)
            m_onClick();
    }

private:
    QString m_label;
    std::function<void()> m_onClick;
    bool m_appColourWhenBlack = true;
    QColor m_color = Qt::black;
};

// One undo entry per committed gesture: full-session byte snapshots through
// the codec. Snapshot restore is inherently field-complete (walkBrush), so
// no per-field command can ever fall out of step with the Brush model.
class BrushEditCommand : public QUndoCommand
{
public:
    BrushEditCommand(BrushSettingsStudio *studio, QByteArray before,
                     QByteArray after)
        : m_studio(studio)
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
        setText(QStringLiteral("Edit Brush"));
    }
    void redo() override
    {
        if (m_firstRedo) { // the edit is already applied when pushed
            m_firstRedo = false;
            return;
        }
        m_studio->restoreSessionBytes(m_after);
    }
    void undo() override { m_studio->restoreSessionBytes(m_before); }

private:
    BrushSettingsStudio *m_studio;
    QByteArray m_before, m_after;
    bool m_firstRedo = true;
};

// ---------------------------------------------------------------------------

BrushSettingsStudio::BrushSettingsStudio(BrushLibraryModel *model,
                                         QWidget *anchor, QWidget *parent)
    : FloatingToolWindow(anchor, QString(), parent)
    , m_model(model)
{
    setMouseTracking(true);
    // Manager cooperation (the Phase 3 lesson): serve our last placed offset
    // as defaultOffset() so window moves/resizes re-clamp instead of
    // teleporting the studio to the anchor origin.
    setDefaultOffsetProvider([this] { return m_anchorOffset; });
    // The StoryboardPage's "background-color: #0a0a0a" stylesheet CASCADES
    // to every descendant, and plain-QWidget children render it as an
    // opaque fill OVER this window's painted chrome (the seam's red-probe
    // run showed the chrome surviving only in the 1px layout margin). Reset
    // descendants to transparent so the painted column fills show through;
    // widgets with their own stylesheets (buttons, pills, captions) still
    // win over this rule. QMenu would inherit the transparency, so it gets
    // its styling here, in the file's visual language.
    setStyleSheet(SankoTheme::themed("QWidget { background: transparent; }"
        "QMenu { background: #252528; color: #e6e6e6;"
        " border: 1px solid #2d2d31; }"
        "QMenu::item { padding: 4px 18px; }"
        "QMenu::item:selected { background: %PURPLE%; color: white; }"));
    buildUi();
}

double BrushSettingsStudio::storedStabilization()
{
    return QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"))
        .value(kStabKey, 0.0)
        .toDouble();
}

// ------------------------------------------------------------------ UI ----

QPushButton *BrushSettingsStudio::makeButton(const QString &text,
                                             bool accent) const
{
    auto *b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    if (accent)
        b->setStyleSheet(SankoTheme::themed("QPushButton { background: %PURPLE%; border: none;"
            " border-radius: 6px; color: white;"
            " font: 400 14px 'Inter'; padding: 0 16px; min-height: 32px; }"
            "QPushButton:hover { background: #8d80f7; }"));
    else
        b->setStyleSheet(QStringLiteral(
            "QPushButton { background: #252528; border: 1px solid #2d2d31;"
            " border-radius: 6px; color: #e6e6e6;"
            " font: 500 13px 'Inter'; padding: 0 14px; min-height: 30px; }"
            "QPushButton:hover { background: #2d2d31; }"
            "QPushButton:disabled { color: #55555a; }"));
    return b;
}

QLabel *BrushSettingsStudio::makeCaption(const QString &text) const
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(
        QStringLiteral("color: #6a6a70; font: 500 12px 'Inter';"));
    return l;
}

QWidget *BrushSettingsStudio::sectionPage(QVBoxLayout **outLayout) const
{
    auto *page = new QWidget;
    auto *l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(28); // Figma 274:71 properties-list gap
    *outLayout = l;
    return page;
}

void BrushSettingsStudio::addSection(const QString &name, QWidget *page)
{
    const int index = m_sectionPages.size();
    auto *row = new StudioSectionRow(name, [this, index] {
        selectSection(index);
    });
    m_sidebarRows->addWidget(row);
    m_sectionRows.append(row);
    m_sectionPages.append(page);
    m_sectionNames.append(name);
    m_pages->addWidget(page);
}

void BrushSettingsStudio::buildUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1); // inside the 1px painted border
    root->setSpacing(0);

    // --- Sidebar (274:24): 292w, #252528, title + section rows ------------
    m_sidebarWidget = new QWidget;
    m_sidebarWidget->setFixedWidth(kSidebarW);
    auto *sb = new QVBoxLayout(m_sidebarWidget);
    sb->setContentsMargins(24, 28, 16, 28);
    sb->setSpacing(24);
    auto *title = new QLabel(QStringLiteral("Brush Settings"));
    title->setStyleSheet(
        QStringLiteral("color: white; font: 700 20px 'Inter';"));
    auto *titleBlock = new QVBoxLayout;
    titleBlock->setContentsMargins(0, 0, 0, 0);
    titleBlock->setSpacing(4);
    titleBlock->addWidget(title);
    // The edited brush's name — the design has no name surface, but a
    // full-parameter editor without it invites editing the wrong brush.
    m_brushNameLabel = new QLabel;
    m_brushNameLabel->setStyleSheet(
        QStringLiteral("color: #96969b; font: 500 13px 'Inter';"));
    titleBlock->addWidget(m_brushNameLabel);
    sb->addLayout(titleBlock);
    m_sidebarRows = new QVBoxLayout;
    m_sidebarRows->setContentsMargins(0, 0, 0, 0);
    m_sidebarRows->setSpacing(2);
    sb->addLayout(m_sidebarRows);
    sb->addStretch(1);
    root->addWidget(m_sidebarWidget);

    // --- Canvas panel (274:112): header + scratch pad ---------------------
    m_canvasWidget = new QWidget;
    auto *cv = new QVBoxLayout(m_canvasWidget);
    cv->setContentsMargins(28, 28, 28, 28);
    cv->setSpacing(40); // header y28 h32, strokes at y100
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(16);
    auto *pill = new QLabel(QStringLiteral("Drawing Canvas"));
    pill->setStyleSheet(QStringLiteral(
        "background: #252528; border: 1px solid #2d2d31;"
        " border-radius: 16px; padding: 8px 16px; color: white;"
        " font: 600 13px 'Inter';"));
    header->addWidget(pill);
    header->addStretch(1);
    // Save Variation: the design shows only Cancel and Done; this third
    // action (styled exactly like Cancel) is the approved documented
    // divergence for "save as a new preset without touching the original".
    auto *saveVar = new QPushButton(QStringLiteral("Save Variation"));
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    const QString flat = QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        " color: #96969b; font: 500 14px 'Inter'; }"
        "QPushButton:hover { color: white; }");
    saveVar->setStyleSheet(flat);
    saveVar->setCursor(Qt::PointingHandCursor);
    cancel->setStyleSheet(flat);
    cancel->setCursor(Qt::PointingHandCursor);
    auto *done = makeButton(QStringLiteral("Done"), true);
    header->addWidget(saveVar);
    header->addWidget(cancel);
    header->addWidget(done);
    cv->addLayout(header);
    m_scratch = new ScratchCanvas;
    cv->addWidget(m_scratch, 1);
    root->addWidget(m_canvasWidget, 1);

    connect(saveVar, &QPushButton::clicked, this,
            &BrushSettingsStudio::saveVariationClicked);
    connect(cancel, &QPushButton::clicked, this,
            [this] { setVisible(false); });
    connect(done, &QPushButton::clicked, this,
            &BrushSettingsStudio::doneClicked);

    // --- Properties panel (274:69): 384w, title + section pages -----------
    m_propsWidget = new QWidget;
    m_propsWidget->setFixedWidth(kPropsW);
    auto *pr = new QVBoxLayout(m_propsWidget);
    // Right margin 22 + the permanently reserved 10 px scrollbar gutter =
    // the same 32 px visual margin as the left. See the scroll area below.
    pr->setContentsMargins(32, 28, 22, 28);
    pr->setSpacing(36);
    auto *propsHeader = new QHBoxLayout;
    propsHeader->setContentsMargins(0, 0, 0, 0);
    m_propsTitle = new QLabel;
    m_propsTitle->setStyleSheet(
        QStringLiteral("color: white; font: 600 18px 'Inter';"));
    propsHeader->addWidget(m_propsTitle);
    propsHeader->addStretch(1);
    // A|B scope switch: visible only while the dual brush is enabled.
    m_scopeSwitch = new StudioSegmentedRow(
        QString(), {QStringLiteral("A"), QStringLiteral("B")});
    m_scopeSwitch->setFixedWidth(2 * (QFontMetrics(capsuleFont())
                                          .horizontalAdvance(
                                              QStringLiteral("A"))
                                      + 20));
    m_scopeSwitch->hide();
    connect(m_scopeSwitch, &StudioSegmentedRow::chosen, this,
            [this](int idx) { setScope(idx == 1); });
    propsHeader->addWidget(m_scopeSwitch);
    pr->addLayout(propsHeader);
    m_propsScroll = new QScrollArea;
    m_propsScroll->setFrameShape(QFrame::NoFrame);
    m_propsScroll->setWidgetResizable(true);
    m_propsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The gutter is RESERVED permanently. With AsNeeded, the bar appearing
    // stole its width from a viewport whose content cannot shrink below
    // 320 (the Shape Control panel is fixed 320 wide), so the last stock-
    // scrollbar-width of every row clipped under the bar — and sections
    // short enough not to scroll laid out wider than ones that did, so
    // content shifted horizontally between sections. AlwaysOn + the shared
    // 10 px style keeps the viewport at exactly 320 in every section; the
    // app-wide style paints a disabled bar's handle transparent, so on a
    // short section the gutter reads as plain margin.
    m_propsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_propsScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"));
    m_pages = new QStackedWidget;
    m_propsScroll->setWidget(m_pages);
    pr->addWidget(m_propsScroll, 1);
    root->addWidget(m_propsWidget);

    // Sections. The Figma sidebar lists Stroke, Smoothing, Taper, Tip,
    // Texture, Rendering, Mixing, Color, Dynamics, General, Preview.
    // Approved divergences: Taper removed (no engine backing — deferred to
    // the same future engine mini-phase as Fall off) and Rendering renamed
    // DUAL BRUSH (its actual contents; the engine has no primary-brush
    // rendering parameters).
    addSection(QStringLiteral("Stroke"), buildStrokeSection());
    addSection(QStringLiteral("Smoothing"), buildSmoothingSection());
    addSection(QStringLiteral("Tip"), buildTipSection());
    addSection(QStringLiteral("Texture"), buildTextureSection());
    m_dualSectionIndex = m_sectionPages.size();
    addSection(QStringLiteral("Dual Brush"), buildDualBrushSection());
    addSection(QStringLiteral("Mixing"), buildMixingSection());
    addSection(QStringLiteral("Color"), buildColorSection());
    addSection(QStringLiteral("Dynamics"), buildDynamicsSection());
    addSection(QStringLiteral("General"), buildGeneralSection());
    addSection(QStringLiteral("Preview"), buildPreviewSection());
    selectSection(0);
}

void BrushSettingsStudio::selectSection(int index)
{
    if (index < 0 || index >= m_sectionPages.size())
        return;
    if (m_scopeB && index == m_dualSectionIndex)
        return; // Dual Brush is A-scope only
    m_currentSection = index;
    m_pages->setCurrentIndex(index);
    for (int i = 0; i < m_sectionRows.size(); ++i)
        m_sectionRows.at(i)->setActive(i == index);
    m_propsTitle->setText(m_sectionNames.at(index)
                          + QStringLiteral(" properties"));
}

// ------------------------------------------------------- edit plumbing ----

::Brush &BrushSettingsStudio::scopeBrush()
{
    if (m_scopeB && m_session.dualBrushEnabled())
        return m_session.secondaryBrush();
    return m_session;
}

const ::Brush &BrushSettingsStudio::scopeBrushConst() const
{
    if (m_scopeB && m_session.dualBrushEnabled())
        return m_session.secondaryBrush();
    return m_session;
}

void BrushSettingsStudio::setScope(bool scopeB)
{
    if (scopeB && !m_session.dualBrushEnabled())
        scopeB = false;
    if (scopeB == m_scopeB)
        return;
    m_scopeB = scopeB;
    if (m_scopeB && m_currentSection == m_dualSectionIndex)
        selectSection(0);
    syncAll();
}

void BrushSettingsStudio::beginGesture()
{
    if (!m_gestureBefore)
        m_gestureBefore = BrushPresetCodec::saveBrush(m_session);
}

void BrushSettingsStudio::commitGesture()
{
    if (m_gestureBefore) {
        QByteArray after = BrushPresetCodec::saveBrush(m_session);
        if (after != *m_gestureBefore)
            m_undo.push(new BrushEditCommand(this, *m_gestureBefore,
                                             std::move(after)));
        m_gestureBefore.reset();
    }
    syncAll();
}

void BrushSettingsStudio::applyInstant(
    const std::function<void(::Brush &)> &fn, bool tipInvalidating)
{
    QByteArray before = BrushPresetCodec::saveBrush(m_session);
    fn(scopeBrush());
    QByteArray after = BrushPresetCodec::saveBrush(m_session);
    if (after != before)
        m_undo.push(new BrushEditCommand(this, std::move(before),
                                         std::move(after)));
    pushToScratch(tipInvalidating);
    syncAll();
}

void BrushSettingsStudio::pushToScratch(bool tipInvalidating)
{
    m_scratch->setBrush(m_session, tipInvalidating);
}

void BrushSettingsStudio::restoreSessionBytes(const QByteArray &bytes)
{
    ::Brush restored;
    if (!BrushPresetCodec::loadBrush(bytes, restored))
        return;
    m_session = restored;
    m_gestureBefore.reset();
    if (m_scopeB && !m_session.dualBrushEnabled())
        m_scopeB = false; // undo removed the sub-brush this scope pointed at
    pushToScratch(true);
    syncAll();
}

void BrushSettingsStudio::syncAll()
{
    m_syncing = true;
    for (const auto &sync : m_syncers)
        sync();
    // Scope switch: only meaningful with a dual brush.
    const bool dual = m_session.dualBrushEnabled();
    m_scopeSwitch->setVisible(dual);
    m_scopeSwitch->setCurrentIndex(m_scopeB ? 1 : 0);
    if (m_dualSectionIndex >= 0)
        m_sectionRows.at(m_dualSectionIndex)->setDimmed(m_scopeB);
    m_brushNameLabel->setText(
        m_scopeB ? m_presetName + QStringLiteral("  —  Brush B")
                 : m_presetName);
    m_syncing = false;
}

bool BrushSettingsStudio::sessionDirty() const
{
    return BrushPresetCodec::saveBrush(m_session)
        != BrushPresetCodec::saveBrush(m_original);
}

// -------------------------------------------------------- row factories ---

StudioSlider *BrushSettingsStudio::addSlider(
    QVBoxLayout *layout, const QString &label, double min, double max,
    std::function<double(const ::Brush &)> get,
    std::function<void(::Brush &, double)> set,
    std::function<QString(double)> fmt, bool tipInvalidating, double step,
    double exponent, const CurveAccess &curve, bool curveInvalidatesTip,
    std::optional<::Brush::DynamicProperty> property,
    std::function<bool()> extraEnable)
{
    auto *s = new StudioSlider(label, min, max);
    s->setFormatter(std::move(fmt));
    if (step > 0.0)
        s->setStep(step);
    if (exponent != 1.0)
        s->setExponent(exponent);
    layout->addWidget(s);
    connect(s, &StudioSlider::valueChanged, this,
            [this, set, tipInvalidating](double v) {
                if (m_syncing)
                    return;
                beginGesture();
                set(scopeBrush(), v);
                pushToScratch(tipInvalidating);
            });
    connect(s, &StudioSlider::valueCommitted, this,
            [this](double, double) { commitGesture(); });
    if (curve && property) {
        // A dynamic property: the chip opens the full response drawer.
        s->enableCurveChip();
        QWidget *drawer = attachResponseDrawer(
            layout, curve, curveInvalidatesTip, *property, s, nullptr,
            std::move(extraEnable));
        connect(s, &StudioSlider::chipClicked, this, [s, drawer] {
            const bool show = !drawer->isVisible();
            drawer->setVisible(show);
            s->setChipExpanded(show);
        });
        m_syncers.append([this, s, curve] {
            s->setChipCurve(curve(const_cast<::Brush &>(scopeBrushConst())));
        });
    } else if (curve) {
        s->enableCurveChip();
        StudioCurveEditor *ed =
            attachCurveEditor(layout, curve, curveInvalidatesTip);
        connect(s, &StudioSlider::chipClicked, this, [s, ed] {
            const bool show = !ed->isVisible();
            ed->setVisible(show);
            s->setChipExpanded(show);
        });
        m_syncers.append([this, s, curve] {
            s->setChipCurve(curve(const_cast<::Brush &>(scopeBrushConst())));
        });
    }
    m_syncers.append([this, s, get] { s->setValue(get(scopeBrushConst())); });
    return s;
}

namespace {
// Order matches Brush::ControlSource; index-mapped both ways.
QStringList controlSourceNames()
{
    return {QStringLiteral("None"), QStringLiteral("Pressure"),
            QStringLiteral("Fade"), QStringLiteral("Tilt"),
            QStringLiteral("Direction")};
}
} // namespace

QWidget *BrushSettingsStudio::attachResponseDrawer(
    QVBoxLayout *layout, const CurveAccess &curve, bool curveInvalidatesTip,
    ::Brush::DynamicProperty property, StudioSlider *ownerSlider,
    StudioCurveRow *ownerRow, std::function<bool()> extraEnable)
{
    auto *drawer = new QWidget;
    auto *dl = new QVBoxLayout(drawer);
    dl->setContentsMargins(12, 4, 0, 4); // indented under the owner row
    dl->setSpacing(10);
    drawer->hide();
    layout->addWidget(drawer);

    auto *source = new StudioChoiceRow(QStringLiteral("Source"),
                                       controlSourceNames());
    dl->addWidget(source);
    connect(source, &StudioChoiceRow::chosen, this,
            [this, property](int index) {
                if (m_syncing)
                    return;
                applyInstant(
                    [property, index](::Brush &b) {
                        b.setControlSource(property,
                                           ::Brush::ControlSource(index));
                    },
                    false);
            });

    auto *minimum = new StudioSlider(QStringLiteral("Minimum"), 0.0, 1.0);
    minimum->setFormatter(fmtPercent);
    dl->addWidget(minimum);
    connect(minimum, &StudioSlider::valueChanged, this,
            [this, property](double v) {
                if (m_syncing)
                    return;
                beginGesture();
                scopeBrush().setControlMinimum(property, v);
                pushToScratch(false);
            });
    connect(minimum, &StudioSlider::valueCommitted, this,
            [this](double, double) { commitGesture(); });

    auto *noneNote = makeCaption(QStringLiteral(
        "Source None holds this property at its base value — minimum and "
        "curve have no effect."));
    dl->addWidget(noneNote);

    auto *ed = new StudioCurveEditor;
    dl->addWidget(ed);
    connect(ed, &StudioCurveEditor::curveChanged, this,
            [this, curve, curveInvalidatesTip](const PressureCurve &c) {
                if (m_syncing)
                    return;
                beginGesture();
                curve(scopeBrush()).setControlPoints(c.controlPoints());
                pushToScratch(curveInvalidatesTip);
            });
    connect(ed, &StudioCurveEditor::curveCommitted, this,
            [this] { commitGesture(); });
    m_lastCurveEditor = ed;

    m_syncers.append([this, property, source, minimum, noneNote, ed, curve,
                      ownerSlider, ownerRow,
                      extra = std::move(extraEnable)] {
        const ::Brush &b = scopeBrushConst();
        const auto src = b.controlSource(property);
        const int index = int(src);
        source->setCurrentIndex(index);
        minimum->setValue(b.controlMinimum(property));
        ed->setCurve(curve(const_cast<::Brush &>(b)));
        const bool rowLive = !extra || extra();
        const bool driven = src != ::Brush::ControlSource::None;
        source->setEnabled(rowLive);
        minimum->setEnabled(rowLive && driven);
        ed->setEnabled(rowLive && driven);
        noneNote->setVisible(!driven);
        const QString name = controlSourceNames().at(index);
        if (ownerSlider) {
            ownerSlider->setSourceCapsule(name);
            ownerSlider->setChipDimmed(!driven || !rowLive);
        }
        if (ownerRow) {
            ownerRow->setSourceCapsule(name);
            ownerRow->setChipDimmed(!driven || !rowLive);
        }
    });
    return drawer;
}

StudioCurveEditor *BrushSettingsStudio::attachCurveEditor(
    QVBoxLayout *layout, const CurveAccess &curve, bool tipInvalidating)
{
    auto *ed = new StudioCurveEditor;
    ed->hide();
    layout->addWidget(ed);
    connect(ed, &StudioCurveEditor::curveChanged, this,
            [this, curve, tipInvalidating](const PressureCurve &c) {
                if (m_syncing)
                    return;
                beginGesture();
                curve(scopeBrush()).setControlPoints(c.controlPoints());
                pushToScratch(tipInvalidating);
            });
    connect(ed, &StudioCurveEditor::curveCommitted, this,
            [this] { commitGesture(); });
    m_syncers.append([this, ed, curve] {
        ed->setCurve(curve(const_cast<::Brush &>(scopeBrushConst())));
    });
    m_lastCurveEditor = ed;
    return ed;
}

StudioCurveRow *BrushSettingsStudio::addCurveRow(
    QVBoxLayout *layout, const QString &label, const CurveAccess &curve,
    bool tipInvalidating, std::optional<::Brush::DynamicProperty> property,
    std::function<bool()> extraEnable)
{
    auto *row = new StudioCurveRow(label);
    layout->addWidget(row);
    if (property) {
        QWidget *drawer = attachResponseDrawer(
            layout, curve, tipInvalidating, *property, nullptr, row,
            std::move(extraEnable));
        connect(row, &StudioCurveRow::clicked, this, [row, drawer] {
            const bool show = !drawer->isVisible();
            drawer->setVisible(show);
            row->setExpanded(show);
        });
    } else {
        StudioCurveEditor *ed =
            attachCurveEditor(layout, curve, tipInvalidating);
        connect(row, &StudioCurveRow::clicked, this, [row, ed] {
            const bool show = !ed->isVisible();
            ed->setVisible(show);
            row->setExpanded(show);
        });
    }
    m_syncers.append([this, row, curve] {
        row->setCurve(curve(const_cast<::Brush &>(scopeBrushConst())));
    });
    return row;
}

StudioToggleRow *BrushSettingsStudio::addToggle(
    QVBoxLayout *layout, const QString &label,
    std::function<bool(const ::Brush &)> get,
    std::function<void(::Brush &, bool)> set, bool tipInvalidating)
{
    auto *t = new StudioToggleRow(label);
    layout->addWidget(t);
    connect(t, &StudioToggleRow::toggled, this,
            [this, set, tipInvalidating](bool on) {
                if (m_syncing)
                    return;
                applyInstant([set, on](::Brush &b) { set(b, on); },
                             tipInvalidating);
            });
    m_syncers.append(
        [this, t, get] { t->setChecked(get(scopeBrushConst())); });
    return t;
}

// ------------------------------------------------------------- sections ---

QWidget *BrushSettingsStudio::buildStrokeSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    // The design's Stroke rows: Spacing, Spacing Jitter, Jitter Lateral,
    // Jitter Linear (+ "Fall off", DROPPED — no engine backing; approved).
    addSlider(l, QStringLiteral("Spacing"), 0.01, 10.0,
              [](const ::Brush &b) { return b.spacing(); },
              [](::Brush &b, double v) { b.setSpacing(v); }, fmtPercent,
              false, 0.0, 3.0);
    addSlider(l, QStringLiteral("Spacing Jitter"), 0.0, 1.0,
              [](const ::Brush &b) { return b.spacingJitter(); },
              [](::Brush &b, double v) { b.setSpacingJitter(v); },
              fmtPercent, false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.spacingJitterPressureCurve();
              },
              false, ::Brush::DynamicProperty::SpacingJitter);
    addSlider(l, QStringLiteral("Jitter Lateral"), 0.0, 10.0,
              [](const ::Brush &b) { return b.scatterPerpendicular(); },
              [](::Brush &b, double v) { b.setScatterPerpendicular(v); },
              fmtPercent, false, 0.0, 2.0);
    addSlider(l, QStringLiteral("Jitter Linear"), 0.0, 10.0,
              [](const ::Brush &b) { return b.scatterAlong(); },
              [](::Brush &b, double v) { b.setScatterAlong(v); }, fmtPercent,
              false, 0.0, 2.0);
    // Not in the design (documented divergence): the engine's scatter is
    // per-dab COUNT as well as offset, and a hidden count is an invisible
    // parameter the mapping table promised to expose.
    addSlider(l, QStringLiteral("Scatter Count"), 1.0, 16.0,
              [](const ::Brush &b) { return double(b.scatterCount()); },
              [](::Brush &b, double v) { b.setScatterCount(qRound(v)); },
              fmtCount, false, 1.0);
    // Build-up (Phase 6b): a bare row — not a dynamic. It is the TIME
    // cadence of stamp emission (the wall-clock twin of Spacing, which is
    // why it lives beside it in Stroke): the walk consumes it BETWEEN
    // stamps, where no per-stamp resolved value exists for a control
    // source to drive. The deposit of each emitted stamp already responds
    // to pressure through the Flow and Opacity dynamics.
    addSlider(l, QStringLiteral("Build-up"), 0.0, 1.0,
              [](const ::Brush &b) { return b.buildUp(); },
              [](::Brush &b, double v) { b.setBuildUp(v); }, fmtPercent,
              false);
    l->addWidget(makeCaption(QStringLiteral(
        "Build-up keeps depositing while the pen is held still, like an "
        "airbrush — at full amount, sixty stamps a second. Most visible "
        "with Flow below 100%.")));

    addCurveRow(l, QStringLiteral("Scatter Dynamics"),
                [](::Brush &b) -> PressureCurve & {
                    return b.scatterPressureCurve();
                },
                false, ::Brush::DynamicProperty::Scatter);
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildSmoothingSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    // Option (c), as approved: an APP-LEVEL stabilization amount shown in
    // this section, stored in QSettings — never in the preset, never in the
    // session, never in the studio undo. 0 keeps today's fixed StrokeBuilder
    // smoothing byte-identical (the canvas skips the filter entirely).
    auto *s = new StudioSlider(QStringLiteral("Stabilization"), 0.0, 1.0);
    s->setFormatter(fmtPercent);
    s->setValue(storedStabilization());
    l->addWidget(s);
    connect(s, &StudioSlider::valueChanged, this,
            [this](double v) { emit stabilizationChanged(v); });
    connect(s, &StudioSlider::valueCommitted, this,
            [this](double, double after) {
                QSettings(QStringLiteral("SankoTV"),
                          QStringLiteral("SankoTV"))
                    .setValue(kStabKey, after);
                emit stabilizationChanged(after);
            });
    l->addWidget(makeCaption(QStringLiteral(
        "App-level input smoothing, applied to every brush before the "
        "engine stamps. Stored in preferences — not part of this "
        "preset.")));
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildTipSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);

    // Tip shape: procedural round vs a custom grayscale image. Reuses the
    // codec's PNG-embedded image slot — no second image path.
    auto *shapeRow = new QWidget;
    auto *sr = new QHBoxLayout(shapeRow);
    sr->setContentsMargins(0, 0, 0, 0);
    sr->setSpacing(8);
    auto *shapeLabel = new QLabel(QStringLiteral("Tip Shape"));
    shapeLabel->setStyleSheet(
        QStringLiteral("color: #96969b; font: 500 14px 'Inter';"));
    auto *thumb = new QLabel;
    thumb->setFixedSize(44, 30);
    thumb->setAlignment(Qt::AlignCenter);
    thumb->setStyleSheet(QStringLiteral(
        "background: #111112; border-radius: 6px; color: #96969b;"
        " font: 500 11px 'Inter';"));
    auto *loadTip = makeButton(QStringLiteral("Load…"), false);
    auto *roundTip = makeButton(QStringLiteral("Round"), false);
    sr->addWidget(shapeLabel);
    sr->addStretch(1);
    sr->addWidget(thumb);
    sr->addWidget(loadTip);
    sr->addWidget(roundTip);
    l->addWidget(shapeRow);
    connect(loadTip, &QPushButton::clicked, this, [this] {
        const QImage img =
            loadImageFile(this, tr("Load Tip Image"));
        if (img.isNull())
            return;
        applyInstant([&img](::Brush &b) { b.setCustomShape(img); }, true);
    });
    connect(roundTip, &QPushButton::clicked, this, [this] {
        applyInstant([](::Brush &b) { b.clearCustomShape(); }, true);
    });
    m_syncers.append([this, thumb, roundTip] {
        const ::Brush &b = scopeBrushConst();
        const bool transformed = b.tipAngle() != 0.0
            || b.tipRoundness() != 1.0 || b.tipFlipX() || b.tipFlipY();
        if (b.hasCustomShape() || transformed) {
            thumb->setText(QString());
            thumb->setPixmap(QPixmap::fromImage(tipTransformThumbnail(b)));
        } else {
            thumb->setPixmap(QPixmap());
            thumb->setText(QStringLiteral("Round"));
        }
        roundTip->setEnabled(b.hasCustomShape());
    });

    addSlider(l, QStringLiteral("Hardness"), 0.0, 1.0,
              [](const ::Brush &b) { return b.hardness(); },
              [](::Brush &b, double v) { b.setHardness(v); }, fmtPercent,
              true);
    // Static tip transform (dynamics Phase 2): applied to every stamp,
    // regardless of input — no source, no minimum, no curve, which is the
    // visible distinction from the jitters below that vary AROUND them.
    //
    // The Shape Control panel (Figma 345:99, wrapping the revised ring
    // node 341:30) is DIRECT MANIPULATION for the same two fields the
    // sliders below edit numerically: ring drag -> tipAngle, handle drag
    // -> tipRoundness, pivot click -> reset both. Its capsule is a
    // display-only readout — chrome, never a hit region.
    // (flips deliberately excluded: the ring does not display them, and a
    // control should not reset state it cannot show — they keep their
    // toggle rows). Sits ABOVE its two sliders as an addendum to the
    // approved mapping table. Edits run the slider gesture pattern —
    // beginGesture / per-move scratch push at the 40 ms non-tip tier /
    // one undo commit on release — and the syncer pass keeps ring and
    // sliders agreeing in both directions (silent setter, m_syncing
    // guard, QSignalBlocker belt-and-braces).
    auto *ringRow = new QWidget;
    auto *rr = new QHBoxLayout(ringRow);
    rr->setContentsMargins(0, 6, 0, 6);
    auto *tipRing = new StudioTipRing;
    rr->addStretch(1);
    rr->addWidget(tipRing);
    rr->addStretch(1);
    l->addWidget(ringRow);
    connect(tipRing, &StudioTipRing::angleEdited, this,
            [this](double degrees) {
                if (m_syncing)
                    return;
                beginGesture();
                scopeBrush().setTipAngle(degrees);
                pushToScratch(false);
            });
    connect(tipRing, &StudioTipRing::roundnessEdited, this,
            [this](double value) {
                if (m_syncing)
                    return;
                beginGesture();
                scopeBrush().setTipRoundness(value);
                pushToScratch(false);
            });
    connect(tipRing, &StudioTipRing::editCommitted, this,
            [this] { commitGesture(); });
    connect(tipRing, &StudioTipRing::resetRequested, this, [this] {
        if (m_syncing)
            return;
        applyInstant(
            [](::Brush &b) {
                b.setTipAngle(0.0);
                b.setTipRoundness(1.0);
            },
            false);
    });
    m_syncers.append([this, tipRing] {
        const QSignalBlocker block(tipRing);
        tipRing->setTipValues(scopeBrushConst().tipAngle(),
                              scopeBrushConst().tipRoundness());
    });

    addSlider(l, QStringLiteral("Tip Angle"), -180.0, 180.0,
              [](const ::Brush &b) { return b.tipAngle(); },
              [](::Brush &b, double v) { b.setTipAngle(v); }, fmtDegrees,
              false, 1.0);
    addSlider(l, QStringLiteral("Tip Roundness"), 0.01, 1.0,
              [](const ::Brush &b) { return b.tipRoundness(); },
              [](::Brush &b, double v) { b.setTipRoundness(v); },
              fmtPercent, false);
    addToggle(l, QStringLiteral("Flip Tip X"),
              [](const ::Brush &b) { return b.tipFlipX(); },
              [](::Brush &b, bool on) { b.setTipFlipX(on); }, false);
    addToggle(l, QStringLiteral("Flip Tip Y"),
              [](const ::Brush &b) { return b.tipFlipY(); },
              [](::Brush &b, bool on) { b.setTipFlipY(on); }, false);
    l->addWidget(makeCaption(
        QStringLiteral("Angle Jitter and Roundness Jitter vary around "
                       "Tip Angle and Tip Roundness.")));

    addSlider(l, QStringLiteral("Angle Jitter"), 0.0, 1.0,
              [](const ::Brush &b) { return b.angleJitter(); },
              [](::Brush &b, double v) { b.setAngleJitter(v); }, fmtPercent,
              false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.angleJitterPressureCurve();
              },
              false, ::Brush::DynamicProperty::AngleJitter);
    addSlider(l, QStringLiteral("Roundness Jitter"), 0.0, 1.0,
              [](const ::Brush &b) { return b.roundnessJitter(); },
              [](::Brush &b, double v) { b.setRoundnessJitter(v); },
              fmtPercent, false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.roundnessJitterPressureCurve();
              },
              false, ::Brush::DynamicProperty::RoundnessJitter);
    addToggle(l, QStringLiteral("Tilt Affects Shape"),
              [](const ::Brush &b) { return b.tiltAffectsShape(); },
              [](::Brush &b, bool on) { b.setTiltAffectsShape(on); }, false);
    StudioSlider *elong = addSlider(
        l, QStringLiteral("Max Tilt Elongation"), 1.0, 10.0,
        [](const ::Brush &b) { return b.maxTiltElongation(); },
        [](::Brush &b, double v) { b.setMaxTiltElongation(v); },
        fmtMultiplier, false);
    m_syncers.append([this, elong] {
        elong->setEnabled(scopeBrushConst().tiltAffectsShape());
    });
    addToggle(l, QStringLiteral("Rotation Affects Shape"),
              [](const ::Brush &b) { return b.rotationAffectsShape(); },
              [](::Brush &b, bool on) { b.setRotationAffectsShape(on); },
              false);
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildTextureSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    using GP = ::Brush::GrainPreset;

    auto *preset = new StudioChoiceRow(
        QStringLiteral("Grain"),
        {QStringLiteral("Paper"), QStringLiteral("Canvas"),
         QStringLiteral("Chalk"), QStringLiteral("Charcoal"),
         QStringLiteral("Custom")});
    l->addWidget(preset);
    connect(preset, &StudioChoiceRow::chosen, this, [this](int idx) {
        if (m_syncing)
            return;
        if (idx == int(GP::Custom)) {
            const QImage img =
                loadImageFile(this, tr("Load Grain Image"));
            if (img.isNull()) {
                syncAll(); // dialog cancelled: snap the row back
                return;
            }
            applyInstant([&img](::Brush &b) { b.setCustomGrain(img); },
                         false);
        } else {
            applyInstant(
                [idx](::Brush &b) { b.setGrainPreset(GP(idx)); }, false);
        }
    });
    m_syncers.append([this, preset] {
        preset->setCurrentIndex(int(scopeBrushConst().grainPreset()));
    });

    addSlider(l, QStringLiteral("Grain Scale"), 1.0, 2048.0,
              [](const ::Brush &b) { return b.grainScale(); },
              [](::Brush &b, double v) { b.setGrainScale(v); }, fmtPixels,
              false, 1.0, 2.5);
    addSlider(l, QStringLiteral("Grain Depth"), 0.0, 1.0,
              [](const ::Brush &b) { return b.grainDepth(); },
              [](::Brush &b, double v) { b.setGrainDepth(v); }, fmtPercent,
              false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.grainDepthPressureCurve();
              },
              false, ::Brush::DynamicProperty::GrainDepth);
    addSlider(l, QStringLiteral("Grain Contrast"), 0.0, 4.0,
              [](const ::Brush &b) { return b.grainContrast(); },
              [](::Brush &b, double v) { b.setGrainContrast(v); },
              fmtPercent, false);
    addSlider(l, QStringLiteral("Grain Rotation"), -180.0, 180.0,
              [](const ::Brush &b) { return b.grainRotation(); },
              [](::Brush &b, double v) { b.setGrainRotation(v); },
              fmtDegrees, false, 1.0);

    auto *mode = new StudioSegmentedRow(
        QStringLiteral("Grain Motion"),
        {QStringLiteral("Rolling"), QStringLiteral("Static")});
    l->addWidget(mode);
    connect(mode, &StudioSegmentedRow::chosen, this, [this](int idx) {
        if (m_syncing)
            return;
        applyInstant(
            [idx](::Brush &b) {
                b.setGrainMode(idx == 0
                                   ? ::Brush::GrainMode::Rolling
                                   : ::Brush::GrainMode::StaticCanvas);
            },
            false);
    });
    m_syncers.append([this, mode] {
        mode->setCurrentIndex(
            scopeBrushConst().grainMode() == ::Brush::GrainMode::Rolling ? 0
                                                                         : 1);
    });

    // Texture blend mode: nine options, so the dual-brush precedent (a
    // StudioChoiceRow list) fits where Grain Motion's two-way segmented
    // row cannot. Order matches Brush::TextureBlendMode exactly.
    auto *blendRow = new StudioChoiceRow(
        QStringLiteral("Texture Blend"),
        {QStringLiteral("Multiply"), QStringLiteral("Subtract"),
         QStringLiteral("Darken"), QStringLiteral("Overlay"),
         QStringLiteral("Colour Burn"), QStringLiteral("Linear Burn"),
         QStringLiteral("Hard Mix"), QStringLiteral("Height"),
         QStringLiteral("Linear Height")});
    l->addWidget(blendRow);
    connect(blendRow, &StudioChoiceRow::chosen, this, [this](int idx) {
        if (m_syncing)
            return;
        applyInstant(
            [idx](::Brush &b) {
                b.setTextureBlendMode(::Brush::TextureBlendMode(idx));
            },
            false);
    });
    m_syncers.append([this, blendRow] {
        blendRow->setCurrentIndex(
            int(scopeBrushConst().textureBlendMode()));
    });

    addToggle(l, QStringLiteral("Grain Affects Colour"),
              [](const ::Brush &b) { return b.grainAffectsColor(); },
              [](::Brush &b, bool on) { b.setGrainAffectsColor(on); },
              false);

    // Noise (Phase 6e): a bare STATIC row — no source, no minimum, no
    // curve chip, the wet-edges treatment — because the amount is a fixed
    // per-pixel property of the tip's falloff, not a per-stamp dynamic.
    // Texture hosts it: noise is per-pixel coverage modulation, grain's
    // nearest kin in the approved mapping table.
    addSlider(l, QStringLiteral("Noise"), 0.0, 1.0,
              [](const ::Brush &b) { return b.noise(); },
              [](::Brush &b, double v) { b.setNoise(v); }, fmtPercent,
              false);
    l->addWidget(makeCaption(QStringLiteral(
        "Noise roughens the soft edge of the tip into a grainy, dispersed "
        "border. It needs a soft tip to show — a hard tip has no falloff "
        "to roughen.")));
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildDualBrushSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    // A-scope only (selectSection blocks it in scope B): the secondary
    // payload is one level deep in the codec, so B has no dual controls.
    auto *enable = addToggle(
        l, QStringLiteral("Dual Brush"),
        [](const ::Brush &b) { return b.dualBrushEnabled(); },
        [](::Brush &b, bool on) {
            b.setDualBrushEnabled(on);
            if (on)
                (void)b.secondaryBrush(); // force the lazy allocation
        },
        false);
    Q_UNUSED(enable);
    // Combination semantics (Phase 6d). Combine keeps the engine's
    // original behaviour: B is a full second stroke, blended with A at
    // publication. Modulate is Photoshop's: B is a coverage pattern that
    // carves A's alpha and contributes no colour of its own.
    auto *mode = new StudioSegmentedRow(
        QStringLiteral("Mode"),
        {QStringLiteral("Combine"), QStringLiteral("Modulate")});
    l->addWidget(mode);
    connect(mode, &StudioSegmentedRow::chosen, this, [this](int idx) {
        if (m_syncing)
            return;
        applyInstant(
            [idx](::Brush &b) {
                b.setDualMode(idx == 1 ? ::Brush::DualMode::Modulate
                                       : ::Brush::DualMode::Composite);
            },
            false);
    });
    auto *blend = new StudioChoiceRow(
        QStringLiteral("Blend Mode"),
        {QStringLiteral("Normal Over"), QStringLiteral("Multiply"),
         QStringLiteral("Mask"), QStringLiteral("Subtract"),
         QStringLiteral("Screen"), QStringLiteral("Overlay"),
         QStringLiteral("Linear Burn")});
    l->addWidget(blend);
    auto *modulateNote = makeCaption(QStringLiteral(
        "In Modulate, brush B only shapes brush A's coverage — it adds no "
        "colour and never marks outside A. Normal Over and Screen have no "
        "coverage meaning there and act as Multiply."));
    l->addWidget(modulateNote);
    connect(blend, &StudioChoiceRow::chosen, this, [this](int idx) {
        if (m_syncing)
            return;
        applyInstant(
            [idx](::Brush &b) {
                b.setDualBlendMode(::Brush::DualBlendMode(idx));
            },
            false);
    });
    StudioSlider *master = addSlider(
        l, QStringLiteral("Master Opacity"), 0.0, 1.0,
        [](const ::Brush &b) { return b.dualMasterOpacity(); },
        [](::Brush &b, double v) { b.setDualMasterOpacity(v); }, fmtPercent,
        false);
    auto *editB = makeButton(QStringLiteral("Edit Brush B"), false);
    l->addWidget(editB);
    connect(editB, &QPushButton::clicked, this,
            [this] { setScope(true); });
    l->addWidget(makeCaption(QStringLiteral(
        "Brush B is a complete brush of its own — tip, texture, "
        "dynamics and pressure curves. Use the A | B switch above the "
        "properties list to come back.")));
    m_syncers.append([this, mode, blend, modulateNote, master, editB] {
        const bool on = m_session.dualBrushEnabled();
        mode->setEnabled(on);
        blend->setEnabled(on);
        master->setEnabled(on);
        editB->setEnabled(on);
        const bool modulate =
            m_session.dualMode() == ::Brush::DualMode::Modulate;
        modulateNote->setVisible(on && modulate);
        if (on) {
            mode->setCurrentIndex(modulate ? 1 : 0);
            blend->setCurrentIndex(int(m_session.dualBlendMode()));
        }
    });
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildMixingSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    auto *mode = new StudioSegmentedRow(
        QStringLiteral("Tool Mode"),
        {QStringLiteral("Paint"), QStringLiteral("Smudge")});
    l->addWidget(mode);
    connect(mode, &StudioSegmentedRow::chosen, this, [this](int idx) {
        if (m_syncing)
            return;
        applyInstant(
            [idx](::Brush &b) {
                b.setToolMode(idx == 0 ? ::Brush::ToolMode::Paint
                                       : ::Brush::ToolMode::Smudge);
            },
            false);
    });
    m_syncers.append([this, mode] {
        mode->setCurrentIndex(
            scopeBrushConst().toolMode() == ::Brush::ToolMode::Paint ? 0
                                                                     : 1);
    });
    // Wet Edges (Phase 6a): a bare row — no source, no minimum, no curve
    // chip — the same treatment the static tip fields received, which is
    // what visually signals "not a dynamic": it is a per-stroke transfer
    // at the publication boundary, so there is no per-stamp value a
    // control source could drive. Addendum entry to the approved mapping
    // table; Mixing hosts it because wetness is paint-liquidity behaviour,
    // beside the Paint/Smudge mode it is gated by.
    StudioSlider *wetRow = addSlider(
        l, QStringLiteral("Wet Edges"), 0.0, 1.0,
        [](const ::Brush &b) { return b.wetEdges(); },
        [](::Brush &b, double v) { b.setWetEdges(v); }, fmtPercent, false);
    l->addWidget(makeCaption(QStringLiteral(
        "Wet edges pools paint toward the stroke's rim, like watercolour. "
        "It needs a soft tip to show a ring, and is inert while smudging.")));
    m_syncers.append([this, wetRow] {
        wetRow->setEnabled(scopeBrushConst().toolMode()
                           == ::Brush::ToolMode::Paint);
    });

    StudioSlider *strength = addSlider(
        l, QStringLiteral("Smudge Strength"), 0.0, 1.0,
        [](const ::Brush &b) { return b.smudgeStrength(); },
        [](::Brush &b, double v) { b.setSmudgeStrength(v); }, fmtPercent,
        false, 0.0, 1.0,
        [](::Brush &b) -> PressureCurve & {
            return b.smudgePressureCurve();
        },
        false, ::Brush::DynamicProperty::Smudge, [this] {
            return scopeBrushConst().toolMode() == ::Brush::ToolMode::Smudge;
        });
    // Phase 1 fact made visible instead of a live-looking dead control:
    // smudgeStrength is INERT in Paint mode. The row's drawer (Source /
    // Minimum / curve editor) dims itself through the extraEnable passed
    // to addSlider above — only the row widget is handled here.
    auto *note = makeCaption(QStringLiteral(
        "Smudge Strength applies in Smudge mode only — in Paint mode "
        "the engine ignores it."));
    l->addWidget(note);
    m_syncers.append([this, strength] {
        strength->setEnabled(scopeBrushConst().toolMode()
                             == ::Brush::ToolMode::Smudge);
    });
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildColorSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    addSlider(l, QStringLiteral("Hue Jitter"), 0.0, 1.0,
              [](const ::Brush &b) { return b.hueJitter(); },
              [](::Brush &b, double v) { b.setHueJitter(v); }, fmtPercent,
              false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.hueJitterPressureCurve();
              },
              false, ::Brush::DynamicProperty::HueJitter);
    addSlider(l, QStringLiteral("Saturation Jitter"), 0.0, 1.0,
              [](const ::Brush &b) { return b.saturationJitter(); },
              [](::Brush &b, double v) { b.setSaturationJitter(v); },
              fmtPercent, false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.saturationJitterPressureCurve();
              },
              false, ::Brush::DynamicProperty::SaturationJitter);
    addSlider(l, QStringLiteral("Brightness Jitter"), 0.0, 1.0,
              [](const ::Brush &b) { return b.brightnessJitter(); },
              [](::Brush &b, double v) { b.setBrightnessJitter(v); },
              fmtPercent, false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.brightnessJitterPressureCurve();
              },
              false, ::Brush::DynamicProperty::BrightnessJitter);

    // Colour dynamics (Phase 6c) — addendum to the approved mapping table.
    // Color Shift is a DYNAMIC (per-stamp, drivable: it joins the response
    // -drawer convention); Background Colour, Purity and Apply Per Tip are
    // static and take the bare treatment.
    addSlider(l, QStringLiteral("Color Shift"), 0.0, 1.0,
              [](const ::Brush &b) { return b.fgBgJitter(); },
              [](::Brush &b, double v) { b.setFgBgJitter(v); }, fmtPercent,
              false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.fgBgJitterPressureCurve();
              },
              false, ::Brush::DynamicProperty::ForegroundBackground);
    auto *bgRow = new StudioColorRow(
        QStringLiteral("Background Colour"),
        [this] {
            const QColor current = scopeBrushConst().backgroundColor();
            const QColor picked = QColorDialog::getColor(
                current, this, tr("Background Colour"));
            if (!picked.isValid())
                return;
            applyInstant(
                [picked](::Brush &b) { b.setBackgroundColor(picked); },
                false);
        },
        nullptr, /*appColourWhenBlack=*/false);
    l->addWidget(bgRow);
    m_syncers.append([this, bgRow] {
        bgRow->setColor(scopeBrushConst().backgroundColor());
    });
    l->addWidget(makeCaption(QStringLiteral(
        "Color Shift blends each stamp from the brush colour toward the "
        "background colour. Unlike the identity colour, the background is "
        "always the preset's own — it is never adopted from or into the "
        "app colour.")));
    addSlider(l, QStringLiteral("Purity"), -1.0, 1.0,
              [](const ::Brush &b) { return b.purity(); },
              [](::Brush &b, double v) { b.setPurity(v); }, fmtPercent,
              false);
    addToggle(l, QStringLiteral("Apply Per Tip"),
              [](const ::Brush &b) { return b.colorDynamicsPerTip(); },
              [](::Brush &b, bool on) { b.setColorDynamicsPerTip(on); },
              false);
    l->addWidget(makeCaption(QStringLiteral(
        "Purity pulls saturation toward grey or full chroma after the "
        "jitters. With Apply Per Tip off, one colour is chosen for the "
        "whole stroke.")));
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildDynamicsSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    // The headline pressure responses whose base values live in General.
    // Size and hardness curves change the stamp shape per dab, so their
    // edits are tip-invalidating for the scratch debounce.
    // Fade Distance: the Fade source's decay span (dynamics Phase 3).
    // Exponent 3.0 — the established wide-range response (Size, Spacing).
    StudioSlider *fadeRow = addSlider(
        l, QStringLiteral("Fade Distance"), 1.0, 8192.0,
        [](const ::Brush &b) { return b.fadeDistance(); },
        [](::Brush &b, double v) { b.setFadeDistance(v); }, fmtPixels,
        false, 1.0, 3.0);
    l->addWidget(makeCaption(
        QStringLiteral("Applies when a property's source is Fade.")));
    m_syncers.append([this, fadeRow] {
        const ::Brush &b = scopeBrushConst();
        bool anyFade = false;
        for (int i = 0; i < ::Brush::kDynamicPropertyCount; ++i)
            anyFade = anyFade
                || b.controlSource(::Brush::DynamicProperty(i))
                    == ::Brush::ControlSource::Fade;
        fadeRow->setEnabled(anyFade);
    });

    addCurveRow(l, QStringLiteral("Size Dynamics"),
                [](::Brush &b) -> PressureCurve & {
                    return b.sizePressureCurve();
                },
                true, ::Brush::DynamicProperty::Size);
    addCurveRow(l, QStringLiteral("Opacity Dynamics"),
                [](::Brush &b) -> PressureCurve & {
                    return b.opacityPressureCurve();
                },
                false, ::Brush::DynamicProperty::Opacity);
    addCurveRow(l, QStringLiteral("Hardness Dynamics"),
                [](::Brush &b) -> PressureCurve & {
                    return b.hardnessPressureCurve();
                },
                true, ::Brush::DynamicProperty::Hardness);
    addCurveRow(l, QStringLiteral("Flow Dynamics"),
                [](::Brush &b) -> PressureCurve & {
                    return b.flowPressureCurve();
                },
                false, ::Brush::DynamicProperty::Flow);
    addSlider(l, QStringLiteral("Size Jitter"), 0.0, 1.0,
              [](const ::Brush &b) { return b.sizeJitter(); },
              [](::Brush &b, double v) { b.setSizeJitter(v); }, fmtPercent,
              false, 0.0, 1.0,
              [](::Brush &b) -> PressureCurve & {
                  return b.sizeJitterPressureCurve();
              },
              true, ::Brush::DynamicProperty::SizeJitter);
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildGeneralSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    addSlider(l, QStringLiteral("Size"), 1.0, 5000.0,
              [](const ::Brush &b) { return double(b.size()); },
              [](::Brush &b, double v) { b.setSize(qRound(v)); }, fmtPixels,
              true, 1.0, 3.0);
    addSlider(l, QStringLiteral("Opacity"), 0.0, 1.0,
              [](const ::Brush &b) { return b.opacity(); },
              [](::Brush &b, double v) { b.setOpacity(v); }, fmtPercent,
              false);
    addSlider(l, QStringLiteral("Flow"), 0.0, 1.0,
              [](const ::Brush &b) { return b.flow(); },
              [](::Brush &b, double v) { b.setFlow(v); }, fmtPercent, false);

    auto *colorRow = new StudioColorRow(
        QStringLiteral("Identity Colour"), [this] {
            const QColor current = scopeBrushConst().color();
            const QColor picked =
                QColorDialog::getColor(current, this,
                                       tr("Identity Colour"));
            if (!picked.isValid())
                return;
            applyInstant([picked](::Brush &b) { b.setColor(picked); },
                         false);
        });
    l->addWidget(colorRow);
    m_syncers.append([this, colorRow] {
        colorRow->setColor(scopeBrushConst().color());
    });
    l->addWidget(makeCaption(QStringLiteral(
        "Black follows the app's colour panel; any other colour is adopted "
        "when this brush is selected (Phase 3 identity-colour semantics).")));
    l->addStretch(1);
    return page;
}

QWidget *BrushSettingsStudio::buildPreviewSection()
{
    QVBoxLayout *l = nullptr;
    QWidget *page = sectionPage(&l);
    // Approved addition: a deterministic sample stroke (same path, same
    // seed, every press) whose curved run exceeds the default fade
    // distance and turns through 360 degrees — the path-dependent
    // dynamics (Direction, Fade) are invisible on a short dab.
    auto *sample = makeButton(QStringLiteral("Sample Stroke"), false);
    l->addWidget(sample);
    connect(sample, &QPushButton::clicked, this,
            [this] { m_scratch->laySampleStroke(); });
    auto *clear = makeButton(QStringLiteral("Clear Drawing Pad"), false);
    l->addWidget(clear);
    connect(clear, &QPushButton::clicked, this,
            [this] { m_scratch->clearStrokes(); });
    l->addWidget(makeCaption(QStringLiteral(
        "Draw on the pad to try the brush exactly as edited. Smudge "
        "brushes preview over colour bands automatically — a smudge "
        "over emptiness moves nothing.")));
    l->addStretch(1);
    return page;
}

// ------------------------------------------------------ session control ---

void BrushSettingsStudio::loadSession(const BrushPreset &preset)
{
    m_presetId = preset.id;
    m_presetName = preset.name;
    m_presetCategory = preset.category;
    m_presetBuiltin = preset.builtin;
    m_original = preset.brush;
    m_session = preset.brush;
    m_scopeB = false;
    m_undo.clear();
    m_gestureBefore.reset();
    pushToScratch(true);
    syncAll();
}

void BrushSettingsStudio::openForPreset(const QString &presetId)
{
    const BrushPreset *p = m_model->preset(presetId);
    if (!p)
        return;
    if (isVisible() && presetId != m_presetId && sessionDirty()) {
        const auto answer = QMessageBox::question(
            this, tr("Brush Settings"),
            tr("Discard unsaved changes to \"%1\"?").arg(m_presetName),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            raise();
            return;
        }
    }
    if (presetId != m_presetId || !isVisible())
        loadSession(*p);
    openAtDefault();
}

void BrushSettingsStudio::doneClicked()
{
    if (m_presetId.isEmpty())
        return;
    // FAILURE IS NOT SILENT (phase 5 defect D4): Done closes only on a
    // confirmed write. On any failure the studio stays open with the
    // session intact, and the message names what actually went wrong.
    if (m_model->updateBrush(m_presetId, m_session)) {
        emit presetCommitted(m_presetId);
        setVisible(false);
        return;
    }
    if (!m_model->preset(m_presetId)) {
        // The preset was deleted from the library behind this session. The
        // edits still exist HERE — Save Variation is the recovery path.
        QMessageBox box(QMessageBox::Warning, tr("Brush Settings"),
                        tr("\"%1\" no longer exists in the library — it was "
                           "deleted while this editor was open.\n\nYour "
                           "edits are still here. Save them as a new brush?")
                            .arg(m_presetName),
                        QMessageBox::NoButton, this);
        QPushButton *saveBtn = box.addButton(tr("Save Variation"),
                                             QMessageBox::AcceptRole);
        box.addButton(tr("Keep Editing"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == saveBtn)
            saveVariationClicked();
        return;
    }
    QMessageBox::warning(
        this, tr("Brush Settings"),
        tr("Could not save \"%1\" — the preset file could not be written "
           "(the folder may be read-only or the disk full).\n\nYour edits "
           "are still here; nothing was saved.")
            .arg(m_presetName));
}

void BrushSettingsStudio::saveVariationClicked()
{
    if (m_presetId.isEmpty())
        return;
    BrushPreset variation;
    variation.name = m_presetName + QStringLiteral(" Variation");
    variation.category = m_presetCategory;
    variation.brush = m_session;
    const QString newId = m_model->addUserPreset(std::move(variation));
    if (newId.isEmpty()) {
        // Same contract as Done (D4): a failed write never closes the
        // studio and never pretends it saved.
        QMessageBox::warning(
            this, tr("Brush Settings"),
            tr("Could not save the variation — the preset file could not "
               "be written (the folder may be read-only or the disk "
               "full).\n\nYour edits are still here; nothing was saved."));
        return;
    }
    emit variationSaved(newId);
    setVisible(false);
}

// ------------------------------------------- hosting: chrome + geometry ---

void BrushSettingsStudio::paintEvent(QPaintEvent *)
{
    // Painted chrome (the Phase 3 rule: QSS backgrounds do not render on a
    // translucent TOP-LEVEL, children are fine): rounded window, the three
    // Figma column fills, and the two column separators.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Column fills as PATH INTERSECTIONS, never painter clipping: on this
    // translucent top-level the raster backing store drops painting done
    // under QPainter::setClipPath (the seam's red-probe run showed clipped
    // fills vanishing while unclipped ones composed) — fillPath with
    // pre-intersected geometry avoids painter clip state entirely.
    QPainterPath shape;
    shape.addRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10,
                         10);
    auto column = [&](const QRect &r, const QColor &c) {
        QPainterPath colPath;
        colPath.addRect(QRectF(r));
        p.fillPath(shape.intersected(colPath), c);
    };
    column(rect(), kWindowBg); // properties column (base fill)
    column(QRect(0, 0, 1 + kSidebarW, height()), kSidebarBg);
    column(QRect(1 + kSidebarW, 0,
                 width() - 2 - kSidebarW - kPropsW, height()),
           kCanvasBg);
    p.setPen(QPen(kBorder, 1));
    p.drawLine(1 + kSidebarW, 0, 1 + kSidebarW, height());
    p.drawLine(width() - 1 - kPropsW, 0, width() - 1 - kPropsW, height());
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
}

void BrushSettingsStudio::openAtDefault()
{
    // Modal-style surface: centred in the application WINDOW's client area
    // on EVERY open, not just the first. The persisted geometry contributes
    // its SIZE only — a user resize is honoured for the session and across
    // sessions, a user move lasts until the next open, when the studio
    // re-centres. The bound is the window's client rect (not the canvas
    // marginRect): at small windows the studio is larger than the canvas
    // viewport, and the client area is the honest bound — the same 4 px
    // kMargin applies to it, so the studio never touches a window edge.
    // When even the client area cannot hold the kMinW x kMinH floor, the
    // top-left stays inside the margin and the excess runs off right and
    // bottom — the resizable edges, so the studio remains recoverable.
    QWidget *host = anchorWidget() ? anchorWidget()->window() : nullptr;
    const QRect client = host
        ? QRect(host->mapToGlobal(QPoint(0, 0)), host->size())
              .adjusted(kMargin, kMargin, -kMargin, -kMargin)
        : marginRect();
    QSize size = savedSize();
    if (!size.isValid())
        size = QSize(1376, 1032);
    size = QSize(qMax(qMin(size.width(), client.width()), kMinW),
                 qMax(qMin(size.height(), client.height()), kMinH));
    resize(size);
    QPoint at = client.center() - QPoint(width() / 2, height() / 2);
    at.setX(qMax(at.x(), client.left()));
    at.setY(qMax(at.y(), client.top()));
    move(at);
    m_anchorOffset = pos() - anchorWidget()->mapToGlobal(QPoint(0, 0));
    m_restored = true;
    setVisible(true);
    raise(); // above the Brush Library, which stays open underneath
}

void BrushSettingsStudio::saveGeometryState() const
{
    QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"))
        .setValue(kGeoKey, geometry());
}

void BrushSettingsStudio::reposition()
{
    // The base clamp confines to the CANVAS marginRect. The studio is a
    // centred modal-style surface that may legitimately exceed the canvas
    // viewport at small windows, so it keeps its window-relative spot
    // (anchor origin + m_anchorOffset — the anchor moves with the window)
    // and clamps against the WINDOW's client area with the same 4 px
    // margin. Left/top win when the window is smaller than the studio, so
    // the resizable right/bottom edges are what run off.
    QWidget *host = anchorWidget() ? anchorWidget()->window() : nullptr;
    if (!host) {
        FloatingToolWindow::reposition();
        return;
    }
    const QRect client = QRect(host->mapToGlobal(QPoint(0, 0)), host->size())
                             .adjusted(kMargin, kMargin, -kMargin, -kMargin);
    QPoint at = anchorWidget()->mapToGlobal(QPoint(0, 0)) + m_anchorOffset;
    at.setX(qMin(at.x(), client.right() - width() + 1));
    at.setY(qMin(at.y(), client.bottom() - height() + 1));
    at.setX(qMax(at.x(), client.left()));
    at.setY(qMax(at.y(), client.top()));
    move(at);
}

QSize BrushSettingsStudio::savedSize() const
{
    // The persisted geometry's SIZE, validated against the minimums; the
    // stored position is deliberately ignored — every open re-centres
    // (openAtDefault). Invalid or absent state returns an invalid QSize.
    const QSettings s(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const QRect geo = s.value(kGeoKey).toRect();
    if (!geo.isValid() || geo.width() < kMinW || geo.height() < kMinH)
        return QSize();
    return geo.size();
}

BrushSettingsStudio::Edge BrushSettingsStudio::edgeAt(const QPoint &pos) const
{
    const bool right = pos.x() >= width() - kEdgeBand;
    const bool bottom = pos.y() >= height() - kEdgeBand;
    if (right && bottom)
        return Edge::Corner;
    if (right)
        return Edge::Right;
    if (bottom)
        return Edge::Bottom;
    return Edge::None;
}

void BrushSettingsStudio::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint at = event->position().toPoint();
    m_resizing = edgeAt(at);
    if (m_resizing != Edge::None) {
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_dragStartSize = size();
        return;
    }
    if (!childAt(at)) { // background drags the studio
        m_pressed = true;
        m_dragging = false;
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_dragStartPos = pos();
    }
}

void BrushSettingsStudio::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint at = event->position().toPoint();
    if (m_resizing != Edge::None && (event->buttons() & Qt::LeftButton)) {
        const QPoint d =
            event->globalPosition().toPoint() - m_dragStartGlobal;
        int w = m_dragStartSize.width();
        int h = m_dragStartSize.height();
        if (m_resizing != Edge::Bottom)
            w = qMax(kMinW, m_dragStartSize.width() + d.x());
        if (m_resizing != Edge::Right)
            h = qMax(kMinH, m_dragStartSize.height() + d.y());
        resize(w, h);
        return;
    }
    if (m_pressed && (event->buttons() & Qt::LeftButton)) {
        const QPoint g = event->globalPosition().toPoint();
        if (!m_dragging) {
            if ((g - m_dragStartGlobal).manhattanLength()
                < QApplication::startDragDistance())
                return;
            m_dragging = true;
        }
        move(clampedPos(m_dragStartPos + (g - m_dragStartGlobal)));
        return;
    }
    switch (edgeAt(at)) {
    case Edge::Corner: setCursor(Qt::SizeFDiagCursor); break;
    case Edge::Right: setCursor(Qt::SizeHorCursor); break;
    case Edge::Bottom: setCursor(Qt::SizeVerCursor); break;
    case Edge::None:
        if (!childAt(at))
            setCursor(Qt::SizeAllCursor);
        else
            unsetCursor();
        break;
    }
}

void BrushSettingsStudio::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_resizing != Edge::None || m_dragging) {
        m_anchorOffset = pos() - anchorWidget()->mapToGlobal(QPoint(0, 0));
        saveGeometryState();
    }
    m_resizing = Edge::None;
    m_pressed = false;
    m_dragging = false;
}

void BrushSettingsStudio::leaveEvent(QEvent *)
{
    if (!m_dragging && m_resizing == Edge::None)
        unsetCursor();
}

void BrushSettingsStudio::showEvent(QShowEvent *event)
{
    FloatingToolWindow::showEvent(event);
    saveGeometryState();
    emit visibilityChanged(true);
}

void BrushSettingsStudio::hideEvent(QHideEvent *event)
{
    FloatingToolWindow::hideEvent(event);
    saveGeometryState();
    emit visibilityChanged(false);
}

void BrushSettingsStudio::moveEvent(QMoveEvent *event)
{
    FloatingToolWindow::moveEvent(event);
    if (m_restored)
        saveGeometryState();
}

void BrushSettingsStudio::resizeEvent(QResizeEvent *event)
{
    FloatingToolWindow::resizeEvent(event);
    if (m_restored)
        saveGeometryState();
}

} // namespace brushlib
