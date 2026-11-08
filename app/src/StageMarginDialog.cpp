#include "StageMarginDialog.h"
#include "ProjectResize.h" // formatBytes, the precheck's own units
#include "SankoTheme.h"
#include "brushlib/StudioControls.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVariantMap>

using brushlib::StudioDropdown;
using brushlib::StudioTextField;
namespace studio = brushlib::studio;

// --- The shared rules ---------------------------------------------------------

namespace stagemargin {

namespace {
constexpr double kShares[] = {0.0, 0.10, 0.25, 0.50};
// One 4K layer, 3840 x 2160 x 4 bytes: past it the note is amber (Q1).
constexpr double kHeavyBytes = 3840.0 * 2160.0 * 4.0;
} // namespace

QString presetLabel(int index)
{
    switch (index) {
    case 0: return QStringLiteral("None");
    case 1: return QStringLiteral("10 %");
    case 2: return QStringLiteral("25 %");
    case 3: return QStringLiteral("50 %");
    default: return QStringLiteral("Custom");
    }
}

QSize presetMargin(int index, const QSize &frame)
{
    if (index < 0 || index >= kCustom)
        return QSize();
    return QSize(qRound(frame.width() * kShares[index]),
                 qRound(frame.height() * kShares[index]));
}

int presetOf(const QSize &margin, const QSize &frame)
{
    for (int i = 0; i < kCustom; ++i)
        if (presetMargin(i, frame) == margin)
            return i;
    return kCustom;
}

QSize stageSize(const QSize &frame, const QSize &margin)
{
    return QSize(frame.width() + 2 * margin.width(), frame.height() + 2 * margin.height());
}

double layerMegabytes(const QSize &stage)
{
    return double(stage.width()) * stage.height() * 4.0 / 1e6;
}

bool heavy(const QSize &stage)
{
    return double(stage.width()) * stage.height() * 4.0 > kHeavyBytes;
}

} // namespace stagemargin

// --- The dialog ----------------------------------------------------------------

namespace {
// Resize Project's idiom and size (Figma 2A-3).
constexpr int kDialogW = 380;
constexpr int kDialogH = 376;
constexpr int kX = 18;
constexpr int kW = kDialogW - 2 * kX;
constexpr int kHeaderY = 18;
constexpr int kFormY = 53;
constexpr int kFieldPitch = 51;
constexpr int kBoxH = 25;
constexpr int kHalfW = (kW - 10) / 2;
constexpr int kEffectY = 214;
constexpr int kFooterY = 325;
constexpr int kFooterH = 33;
constexpr int kFooterGap = 8;

const QColor kDialogBg(0x11, 0x11, 0x11);
const QColor kHeaderText(0xcc, 0xcc, 0xcc);
const QColor kInfoText(0xb3, 0xb3, 0xb3);
const QColor kWarnText(0xe0, 0xa8, 0x60); // art cut: a caution, not an error

QFont headerFont()
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(12);
    f.setWeight(QFont::DemiBold);
    return f;
}

const char *kSecondaryButtonQss =
    "QPushButton { background:#1c1c1c; color:#cccccc; border:1px solid "
    "#333333; border-radius:3px; font-family:Inter; font-size:11px; "
    "font-weight:500; }"
    "QPushButton:hover { border-color:#4a4a4a; color:#ffffff; }"
    "QPushButton:checked { background:#2a2456; border-color:#7c6ef6; "
    "color:#ffffff; }"
    "QPushButton:disabled { color:#5a5a5a; border-color:#262626; "
    "background:#161616; }";

const char *kPrimaryButtonQss =
    "QPushButton { background:#7c6ef6; color:#ffffff; border:none; "
    "border-radius:3px; font-family:Inter; font-size:11px; font-weight:600; }"
    "QPushButton:hover { background:#8b7ef8; }"
    "QPushButton:disabled { background:#33306a; color:#8a86a8; }";

QString sizeText(const QSize &s)
{
    return QStringLiteral("%1 × %2").arg(s.width()).arg(s.height());
}
} // namespace

StageMarginDialog::StageMarginDialog(const QSize &frame, const QSize &currentMargin,
                                     BandProbe band, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint),
      m_frame(frame), m_current(currentMargin), m_band(std::move(band))
{
    setModal(true);
    setFixedSize(kDialogW, kDialogH);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(QStringLiteral("Stage Margin"));

    auto fieldY = [](int index) { return kFormY + index * kFieldPitch + 16; };

    QStringList labels;
    for (int i = 0; i < stagemargin::kPresetCount; ++i)
        labels << stagemargin::presetLabel(i);
    m_preset = new StudioDropdown(labels, this);
    m_preset->setGeometry(kX, fieldY(0), kW, kBoxH);

    m_across = new StudioTextField(this);
    m_across->setGeometry(kX, fieldY(1), kHalfW, kBoxH);
    m_down = new StudioTextField(this);
    m_down->setGeometry(kX + kHalfW + 10, fieldY(1), kHalfW, kBoxH);

    // On: the stage keeps the shot's proportions (the user's Q4).
    m_sameShare = new QPushButton(QStringLiteral("Same share both ways"), this);
    m_sameShare->setGeometry(kX, fieldY(1) + kBoxH + 10, 150, 22);
    m_sameShare->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    m_sameShare->setCheckable(true);
    m_sameShare->setChecked(true);

    // Every panel too (batch 6c, Q10): the panels already made keep theirs otherwise.
    m_applyAll = new QPushButton(QStringLiteral("Apply to All Panels..."), this);
    m_applyAll->setObjectName(QStringLiteral("stageMarginApplyAll"));
    m_applyAll->setGeometry(kX + 160, fieldY(1) + kBoxH + 10, kW - 160, 22);
    m_applyAll->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    connect(m_applyAll, &QPushButton::clicked, this, [this] {
        m_applyAllChosen = true;
        accept();
    });

    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    m_set = new QPushButton(QStringLiteral("Set for New Panels"), this);
    m_set->setObjectName(QStringLiteral("stageMarginSet"));
    m_set->setStyleSheet(QLatin1String(kPrimaryButtonQss));
    const int buttonW = (kW - kFooterGap) / 2;
    m_cancel->setGeometry(kX, kFooterY, buttonW, kFooterH);
    m_set->setGeometry(kX + buttonW + kFooterGap, kFooterY, buttonW, kFooterH);

    // Start on the margin the project has.
    m_across->setText(QString::number(qMax(0, m_current.width())));
    m_down->setText(QString::number(qMax(0, m_current.height())));
    const int start = stagemargin::presetOf(m_current, m_frame);
    m_preset->setCurrentIndex(start);
    m_across->setFieldEnabled(start == stagemargin::kCustom);
    m_down->setFieldEnabled(start == stagemargin::kCustom);

    connect(m_preset, &StudioDropdown::chosen, this, [this](int index) { applyPreset(index); });
    connect(m_across->edit(), &QLineEdit::textEdited, this, [this] {
        syncShare(true);
        revalidate();
    });
    connect(m_down->edit(), &QLineEdit::textEdited, this, [this] {
        syncShare(false);
        revalidate();
    });
    connect(m_sameShare, &QPushButton::toggled, this, [this] { revalidate(); });
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_set, &QPushButton::clicked, this, [this] {
        if (m_reason.isEmpty())
            accept();
    });
    revalidate();
}

QSize StageMarginDialog::chosenMargin() const
{
    return QSize(m_across->intValue(), m_down->intValue());
}

void StageMarginDialog::applyPreset(int index)
{
    // Like Width and Height in New Project: a preset owns the fields
    // (dimmed, locked); Custom hands them over.
    const bool custom = index == stagemargin::kCustom;
    m_across->setFieldEnabled(custom);
    m_down->setFieldEnabled(custom);
    if (!custom) {
        const QSize m = stagemargin::presetMargin(index, m_frame);
        m_syncing = true;
        m_across->setText(QString::number(m.width()));
        m_down->setText(QString::number(m.height()));
        m_syncing = false;
    }
    revalidate();
}

// With the same share both ways, the other field follows the one typed:
// the margin keeps the shot's proportions.
void StageMarginDialog::syncShare(bool fromAcross)
{
    if (m_syncing || !m_sameShare->isChecked() || m_frame.isEmpty())
        return;
    m_syncing = true;
    if (fromAcross)
        m_down->setText(QString::number(
            qRound(double(m_across->intValue()) * m_frame.height() / m_frame.width())));
    else
        m_across->setText(QString::number(
            qRound(double(m_down->intValue()) * m_frame.width() / m_frame.height())));
    m_syncing = false;
}

QString StageMarginDialog::currentText() const
{
    const QSize stage = stagemargin::stageSize(m_frame, m_current);
    const int preset = stagemargin::presetOf(m_current, m_frame);
    const QString which = preset == 0 ? QStringLiteral("no margin")
        : preset == stagemargin::kCustom
            ? QStringLiteral("%1 / %2 px").arg(m_current.width()).arg(m_current.height())
            : stagemargin::presetLabel(preset);
    // "current stage", the user's words (2026-10-31).
    return QStringLiteral("Frame %1 · current stage %2 (%3)")
        .arg(sizeText(m_frame), sizeText(stage), which);
}

QString StageMarginDialog::validate() const
{
    const QSize m = chosenMargin();
    if (m.width() < 0 || m.height() < 0)
        return QStringLiteral("The margin cannot be less than 0.");
    const QSize stage = stagemargin::stageSize(m_frame, m);
    if (stage.width() > stagemargin::kMaxSide || stage.height() > stagemargin::kMaxSide)
        return QStringLiteral("The stage can be at most %1 × %1 pixels; this would be %2.")
            .arg(stagemargin::kMaxSide).arg(sizeText(stage));
    if (m == m_current)
        return QStringLiteral("This is already the project's stage margin.");
    return QString();
}

void StageMarginDialog::revalidate()
{
    m_reason = validate();
    m_effect.clear();
    m_warn = false;
    if (m_reason.isEmpty()) {
        const QSize m = chosenMargin();
        const QSize stage = stagemargin::stageSize(m_frame, m);
        const int lostX = qMax(0, m_current.width() - m.width());
        const int lostY = qMax(0, m_current.height() - m.height());
        if (lostX == 0 && lostY == 0) {
            m_effect = QStringLiteral(
                           "The stage grows to %1 around the same frame. Artwork keeps its "
                           "size and place; the shot and every camera key are unchanged. "
                           "Nothing is scaled or lost.")
                           .arg(sizeText(stage));
        } else {
            // The part that goes, each side.
            const QString part = lostX > 0 && lostY > 0
                ? QStringLiteral("%1 px each side, %2 px top and bottom").arg(lostX).arg(lostY)
                : lostX > 0 ? QStringLiteral("%1 px each side").arg(lostX)
                            : QStringLiteral("%1 px top and bottom").arg(lostY);
            const bool grows = m.width() > m_current.width() || m.height() > m_current.height();
            const QString lead = QStringLiteral("The stage %1 to %2.")
                                     .arg(grows ? QStringLiteral("changes")
                                                : QStringLiteral("shrinks"),
                                          sizeText(stage));
            const Band band = m_band ? m_band(m) : Band();
            const QString keys = band.keysInBand <= 0 ? QString()
                : band.keysInBand == 1
                    ? QStringLiteral(" 1 camera key frames part of it.")
                    : QStringLiteral(" %1 camera keys frame part of it.").arg(band.keysInBand);
            if (band.panelsWithArt > 0) {
                // Nothing is cut (batch 6b-V, Q13): File > Trim Hidden Art frees it.
                m_effect = QStringLiteral("%1 Art on %2 lies outside the new stage (%3). It is kept, "
                                          "hidden, and comes back if the stage grows again.%4")
                               .arg(lead,
                                    band.panelsWithArt == 1
                                        ? QStringLiteral("1 panel")
                                        : QStringLiteral("%1 panels").arg(band.panelsWithArt),
                                    part, keys);
            } else {
                m_effect = QStringLiteral("%1 Nothing is drawn in the part that goes (%2), so "
                                          "nothing is lost. The shot is unchanged.%3")
                               .arg(lead, part, keys);
            }
            m_warn = band.panelsWithArt > 0 || band.keysInBand > 0;
        }
    }
    // The whole project at the chosen stage (the user's item, 2026-10-31).
    m_memText.clear();
    m_memWarn = false;
    if (m_layers > 0) {
        // A margin is a rectangle (batch 6b-V): the layers hold only what is
        // drawn, whatever the stage, so the choice here costs no memory.
        m_memText = QStringLiteral("A margin costs no memory until something is drawn in it");
        if (m_memKnown)
            m_memText += QStringLiteral(" · %1 free now")
                             .arg(ProjectResize::formatBytes(m_availableBytes));
    }
    m_set->setEnabled(m_reason.isEmpty());
    // The project's margin as it is: nothing for new panels to change, and still
    // something to give the panels with their own (batch 6c).
    m_applyAll->setEnabled(m_reason.isEmpty() || chosenMargin() == m_current);
    update();
}

void StageMarginDialog::setProjectMemory(int layers, int panels, qint64 currentBytes,
                                         qint64 availableBytes, bool known)
{
    m_layers = layers;
    m_panels = panels;
    m_currentBytes = currentBytes;
    m_availableBytes = availableBytes;
    m_memKnown = known;
    revalidate();
}

QRect StageMarginDialog::dragHeaderBand() const
{
    return QRect(0, 0, width(), kFormY);
}

QVariantMap StageMarginDialog::devrecState() const
{
    QVariantMap s;
    s.insert(QStringLiteral("frameW"), m_frame.width());
    s.insert(QStringLiteral("frameH"), m_frame.height());
    s.insert(QStringLiteral("currentW"), m_current.width());
    s.insert(QStringLiteral("currentH"), m_current.height());
    s.insert(QStringLiteral("chosenW"), chosenMargin().width());
    s.insert(QStringLiteral("chosenH"), chosenMargin().height());
    s.insert(QStringLiteral("preset"), m_preset->currentText());
    s.insert(QStringLiteral("sameShare"), m_sameShare->isChecked());
    s.insert(QStringLiteral("validationReason"), m_reason);
    s.insert(QStringLiteral("effectText"), m_effect);
    s.insert(QStringLiteral("warns"), m_warn);
    s.insert(QStringLiteral("memoryText"), m_memText);
    s.insert(QStringLiteral("memoryWarns"), m_memWarn);
    return s;
}

void StageMarginDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath silhouette;
    silhouette.addRoundedRect(r, 4, 4);
    p.fillPath(silhouette, kDialogBg);
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 4, 4);

    p.setFont(headerFont());
    p.setPen(kHeaderText);
    p.drawText(QRect(kX, kHeaderY, kW, 14), Qt::AlignLeft, QStringLiteral("Stage Margin for New Panels"));
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.drawLine(kX, kHeaderY + 20, kX + kW, kHeaderY + 20);

    p.setFont(studio::fieldLabelFont());
    p.setPen(studio::kFieldLabel);
    p.drawText(QRect(kX, kFormY, kW, 12), Qt::AlignLeft, QStringLiteral("Margin"));
    p.drawText(QRect(kX, kFormY + kFieldPitch, kHalfW, 12), Qt::AlignLeft,
               QStringLiteral("Across, each side (px)"));
    p.drawText(QRect(kX + kHalfW + 10, kFormY + kFieldPitch, kHalfW, 12), Qt::AlignLeft,
               QStringLiteral("Down, each side (px)"));

    // The whole project's memory, under Same share both ways; amber when it
    // nears what the PC has.
    if (!m_memText.isEmpty()) {
        p.setPen(m_memWarn ? kWarnText : kInfoText);
        p.drawText(QRect(kX, kEffectY - 24, kW, 12), Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(m_memText, Qt::ElideRight, kW));
    }
    p.setPen(kInfoText);
    p.drawText(QRect(kX, kEffectY, kW, 12), Qt::AlignLeft, currentText());
    if (!m_reason.isEmpty()) {
        p.setPen(studio::kFieldLabel);
        p.drawText(QRect(kX, kEffectY + 20, kW, 88), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, m_reason);
    } else {
        // Set touches no panel made so far; what follows is what Apply to All does.
        p.setPen(kInfoText);
        p.drawText(QRect(kX, kEffectY + 18, kW, 12), Qt::AlignLeft,
                   QStringLiteral("New panels get this stage; the panels already made keep theirs."));
        if (!m_effect.isEmpty()) {
            p.setPen(m_warn ? kWarnText : kInfoText);
            p.drawText(QRect(kX, kEffectY + 36, kW, 72), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                       QStringLiteral("To all panels: ") + m_effect);
        }
    }
}

void StageMarginDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_reason.isEmpty())
            accept();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void StageMarginDialog::mousePressEvent(QMouseEvent *event)
{
    if (m_drag.press(this, event, dragHeaderBand())) {
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void StageMarginDialog::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag.move(this, event)) {
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void StageMarginDialog::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drag.release()) {
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}
