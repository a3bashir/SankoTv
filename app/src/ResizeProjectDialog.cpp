#include "ResizeProjectDialog.h"
#include "SankoTheme.h"
#include "brushlib/StudioControls.h"

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

namespace {
// Project Settings' idiom: one column, headers with underline rules,
// 25-tall fields at a 51 pitch.
constexpr int kDialogW = 380;
constexpr int kDialogH = 358;
constexpr int kX = 18;
constexpr int kW = kDialogW - 2 * kX;
constexpr int kHeaderY = 18;
constexpr int kFormY = 53;
constexpr int kFieldPitch = 51;
constexpr int kBoxH = 25;
constexpr int kHalfW = (kW - 10) / 2;
constexpr int kEffectY = 214;
constexpr int kFooterY = 307;
constexpr int kFooterH = 33;
constexpr int kFooterGap = 8;

const QColor kDialogBg(0x11, 0x11, 0x11);
const QColor kHeaderText(0xcc, 0xcc, 0xcc);
const QColor kInfoText(0xb3, 0xb3, 0xb3);
const QColor kWarnText(0xe0, 0xa8, 0x60); // cropping: a caution, not an error

constexpr int kMinDim = 64;
constexpr int kMaxDim = 8192;

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
} // namespace

ResizeProjectDialog::ResizeProjectDialog(const QSize &currentSize,
                                         QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint),
      m_current(currentSize)
{
    setModal(true);
    setFixedSize(kDialogW, kDialogH);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(QStringLiteral("Resize Project"));

    auto fieldY = [](int index) { return kFormY + index * kFieldPitch + 16; };

    // Presets, plus the project's CURRENT size so reopening never proposes a
    // change nobody asked for, and Custom.
    struct Preset { const char *label; int w, h; };
    const Preset presets[] = {{"1280 x 720", 1280, 720},
                              {"1920 x 1080", 1920, 1080},
                              {"2560 x 1440", 2560, 1440},
                              {"3840 x 2160", 3840, 2160}};
    QStringList labels;
    for (const Preset &preset : presets) {
        labels << QString::fromLatin1(preset.label);
        m_presetSizes.append(QSize(preset.w, preset.h));
    }
    labels << QStringLiteral("Custom");
    m_presetSizes.append(QSize()); // Custom sentinel

    m_preset = new StudioDropdown(labels, this);
    m_preset->setGeometry(kX, fieldY(0), kW, kBoxH);

    m_width = new StudioTextField(this);
    m_width->setGeometry(kX, fieldY(1), kHalfW, kBoxH);
    m_width->setText(QString::number(currentSize.width()));
    m_height = new StudioTextField(this);
    m_height->setGeometry(kX + kHalfW + 10, fieldY(1), kHalfW, kBoxH);
    m_height->setText(QString::number(currentSize.height()));

    m_lockAspect = new QPushButton(QStringLiteral("Lock aspect ratio"), this);
    m_lockAspect->setGeometry(kX, fieldY(1) + kBoxH + 10, 150, 22);
    m_lockAspect->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    m_lockAspect->setCheckable(true);
    m_lockAspect->setChecked(true);

    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    m_resize = new QPushButton(QStringLiteral("Resize Project"), this);
    m_resize->setStyleSheet(QLatin1String(kPrimaryButtonQss));
    const int buttonW = (kW - kFooterGap) / 2;
    m_cancel->setGeometry(kX, kFooterY, buttonW, kFooterH);
    m_resize->setGeometry(kX + buttonW + kFooterGap, kFooterY, buttonW,
                          kFooterH);

    // Start on the preset that matches the current size, else Custom.
    int startIndex = m_presetSizes.size() - 1;
    for (int i = 0; i < m_presetSizes.size(); ++i)
        if (m_presetSizes.at(i) == currentSize)
            startIndex = i;
    m_preset->setCurrentIndex(startIndex);

    connect(m_preset, &StudioDropdown::chosen, this,
            [this](int index) { applyPreset(index); });
    connect(m_width->edit(), &QLineEdit::textEdited, this, [this] {
        syncAspect(true);
        revalidate();
    });
    connect(m_height->edit(), &QLineEdit::textEdited, this, [this] {
        syncAspect(false);
        revalidate();
    });
    connect(m_lockAspect, &QPushButton::toggled, this, [this] { revalidate(); });
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_resize, &QPushButton::clicked, this, [this] {
        if (m_reason.isEmpty())
            accept();
    });

    revalidate();
}

QSize ResizeProjectDialog::chosenSize() const
{
    return QSize(m_width->intValue(), m_height->intValue());
}

void ResizeProjectDialog::applyPreset(int index)
{
    if (index < 0 || index >= m_presetSizes.size())
        return;
    const QSize size = m_presetSizes.at(index);
    if (!size.isValid() || size.isEmpty())
        return; // Custom: leave whatever is typed
    m_syncing = true;
    m_width->setText(QString::number(size.width()));
    m_height->setText(QString::number(size.height()));
    m_syncing = false;
    revalidate();
}

// With the ratio locked, the other field follows the one being typed. The
// ratio is the CURRENT project's, so locking preserves the shape the artist
// is already working in.
void ResizeProjectDialog::syncAspect(bool fromWidth)
{
    if (m_syncing || !m_lockAspect->isChecked() || m_current.isEmpty())
        return;
    m_syncing = true;
    const double ratio =
        double(m_current.width()) / double(m_current.height());
    if (fromWidth) {
        const int w = m_width->intValue();
        if (w > 0)
            m_height->setText(QString::number(qRound(double(w) / ratio)));
    } else {
        const int h = m_height->intValue();
        if (h > 0)
            m_width->setText(QString::number(qRound(double(h) * ratio)));
    }
    m_syncing = false;
    // Typing a size by hand means Custom, unless it lands on a preset.
    int index = m_presetSizes.size() - 1;
    for (int i = 0; i < m_presetSizes.size(); ++i)
        if (m_presetSizes.at(i) == chosenSize())
            index = i;
    m_preset->setCurrentIndex(index);
}

QString ResizeProjectDialog::validate() const
{
    const QSize size = chosenSize();
    if (size.width() < kMinDim || size.height() < kMinDim)
        return QStringLiteral("Minimum size is %1 \xC3\x97 %1 pixels.")
            .arg(kMinDim);
    if (size.width() > kMaxDim || size.height() > kMaxDim)
        return QStringLiteral("Maximum size is %1 \xC3\x97 %1 pixels.")
            .arg(kMaxDim);
    if (size == m_current)
        return QStringLiteral("This is already the project's size.");
    return QString();
}

// The live truth about the chosen size. Growing and cropping are stated
// differently because they are different promises: growing cannot lose
// anything, cropping permanently can, and a warning shown for a harmless
// expansion teaches people to ignore warnings.
void ResizeProjectDialog::revalidate()
{
    m_reason = validate();
    const QSize size = chosenSize();
    m_effect.clear();
    if (m_reason.isEmpty()) {
        const bool cropsX = size.width() < m_current.width();
        const bool cropsY = size.height() < m_current.height();
        if (cropsX || cropsY) {
            const int lostX = qMax(0, m_current.width() - size.width());
            const int lostY = qMax(0, m_current.height() - size.height());
            m_effect = QStringLiteral(
                           "Artwork keeps its size, centred. %1 will be "
                           "cropped away and cannot be recovered.")
                           .arg(cropsX && cropsY
                                    ? QStringLiteral("%1 px across and %2 px "
                                                     "down").arg(lostX).arg(lostY)
                                    : (cropsX
                                           ? QStringLiteral("%1 px across")
                                                 .arg(lostX)
                                           : QStringLiteral("%1 px down")
                                                 .arg(lostY)));
        } else {
            m_effect = QStringLiteral(
                "Artwork keeps its size and position, centred on the larger "
                "canvas. Nothing is scaled or lost.");
        }
    }
    m_resize->setEnabled(m_reason.isEmpty());
    update();
}

QRect ResizeProjectDialog::dragHeaderBand() const
{
    return QRect(0, 0, width(), kFormY);
}

QVariantMap ResizeProjectDialog::devrecState() const
{
    QVariantMap s;
    s.insert(QStringLiteral("currentW"), m_current.width());
    s.insert(QStringLiteral("currentH"), m_current.height());
    s.insert(QStringLiteral("chosenW"), chosenSize().width());
    s.insert(QStringLiteral("chosenH"), chosenSize().height());
    s.insert(QStringLiteral("lockAspect"), m_lockAspect->isChecked());
    s.insert(QStringLiteral("preset"), m_preset->currentText());
    s.insert(QStringLiteral("validationReason"), m_reason);
    s.insert(QStringLiteral("effectText"), m_effect);
    return s;
}

void ResizeProjectDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_reason.isEmpty())
            accept();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ResizeProjectDialog::mousePressEvent(QMouseEvent *event)
{
    if (m_drag.press(this, event, dragHeaderBand())) {
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void ResizeProjectDialog::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag.move(this, event)) {
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void ResizeProjectDialog::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drag.release()) {
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

void ResizeProjectDialog::paintEvent(QPaintEvent *)
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
    p.drawText(QRect(kX, kHeaderY, kW, 14), Qt::AlignLeft,
               QStringLiteral("Resize Project"));
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.drawLine(kX, kHeaderY + 20, kX + kW, kHeaderY + 20);

    p.setFont(studio::fieldLabelFont());
    p.setPen(studio::kFieldLabel);
    p.drawText(QRect(kX, kFormY, kW, 12), Qt::AlignLeft,
               QStringLiteral("Preset"));
    p.drawText(QRect(kX, kFormY + kFieldPitch, kW, 12), Qt::AlignLeft,
               QStringLiteral("Width"));
    p.drawText(QRect(kX + kHalfW + 10, kFormY + kFieldPitch, kW, 12),
               Qt::AlignLeft, QStringLiteral("Height"));

    // Current size, then what the chosen one would do.
    p.setPen(kInfoText);
    p.drawText(QRect(kX, kEffectY, kW, 12), Qt::AlignLeft,
               QStringLiteral("Current: %1 \xC3\x97 %2")
                   .arg(m_current.width())
                   .arg(m_current.height()));

    const QString message = m_reason.isEmpty() ? m_effect : m_reason;
    if (!message.isEmpty()) {
        p.setPen(m_reason.isEmpty()
                     && m_effect.contains(QStringLiteral("cropped"))
                 ? kWarnText
                 : (m_reason.isEmpty() ? kInfoText : studio::kFieldLabel));
        p.drawText(QRect(kX, kEffectY + 20, kW, 56),
                   Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, message);
    }
}
