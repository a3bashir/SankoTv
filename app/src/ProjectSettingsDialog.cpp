#include "ProjectSettingsDialog.h"
#include "SankoTheme.h"
#include "brushlib/StudioControls.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <numeric>

using brushlib::StudioDropdown;
using brushlib::StudioTextField;
namespace studio = brushlib::studio;

namespace {
// Geometry in the New Project window's idiom: one column, headers with
// underline rules, 41-tall fields at a 51 pitch.
constexpr int kDialogW = 380;
constexpr int kDialogH = 352;
constexpr int kX = 18;
constexpr int kW = kDialogW - 2 * kX;
constexpr int kGeneralHeaderY = 18;
constexpr int kFormY = 53;
constexpr int kFieldPitch = 51;
constexpr int kBoxH = 25;
constexpr int kCanvasHeaderY = 166;
constexpr int kCanvasInfoY = 201;
constexpr int kResizeY = 246;
constexpr int kFooterY = 301;
constexpr int kFooterH = 33;
constexpr int kFooterGap = 8;

const QColor kDialogBg(0x11, 0x11, 0x11);
const QColor kHeaderText(0xcc, 0xcc, 0xcc);
const QColor kInfoText(0xb3, 0xb3, 0xb3);

// The New Project window's frame-rate choices; the current project rate is
// always present even if it is not one of them (a legacy file could carry
// any integer), so opening the dialog never silently changes it.
constexpr int kStandardFps[] = {24, 25, 30, 60};

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
    "QPushButton:disabled { color:#5a5a5a; border-color:#262626; "
    "background:#161616; }";
} // namespace

ProjectSettingsDialog::ProjectSettingsDialog(const QString &projectName,
                                             int fps, const QSize &canvasSize,
                                             QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint),
      m_canvasSize(canvasSize)
{
    setModal(true);
    setFixedSize(kDialogW, kDialogH);
    setAttribute(Qt::WA_TranslucentBackground); // rounded corners
    setWindowTitle(QStringLiteral("Project Settings"));

    auto fieldY = [](int index) { return kFormY + index * kFieldPitch + 16; };

    m_name = new StudioTextField(this);
    m_name->setGeometry(kX, fieldY(0), kW, kBoxH);
    m_name->setText(projectName);
    m_name->setPlaceholder(QStringLiteral("Project name"));

    QStringList fpsLabels;
    for (int v : kStandardFps)
        m_fpsValues.append(v);
    if (!m_fpsValues.contains(fps)) {
        m_fpsValues.prepend(fps); // the project's own rate, always offered
    }
    for (int v : m_fpsValues)
        fpsLabels << QStringLiteral("%1 fps").arg(v);
    m_fps = new StudioDropdown(fpsLabels, this);
    m_fps->setGeometry(kX, fieldY(1), kW, kBoxH);
    m_fps->setCurrentIndex(m_fpsValues.indexOf(fps)); // silent: no mutation

    // Canvas: informational. Resize is a later workflow; the control is
    // shown disabled so the feature is discoverable but never pretends.
    m_resize = new QPushButton(QStringLiteral("Resize Project..."), this);
    m_resize->setGeometry(kX, kResizeY, 150, kBoxH);
    m_resize->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    m_resize->setEnabled(false);
    m_resize->setToolTip(QStringLiteral("Project resizing is not yet available."));

    // Footer: Cancel | Apply | OK, right-aligned; OK is the filled accent
    // button under the recorded filled-button label exemption (SankoTheme.h).
    const int buttonW = (kW - 2 * kFooterGap) / 3;
    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setGeometry(kX, kFooterY, buttonW, kFooterH);
    m_cancel->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    m_cancel->setCursor(Qt::PointingHandCursor);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);

    m_apply = new QPushButton(QStringLiteral("Apply"), this);
    m_apply->setGeometry(kX + buttonW + kFooterGap, kFooterY, buttonW, kFooterH);
    m_apply->setStyleSheet(QLatin1String(kSecondaryButtonQss));
    m_apply->setCursor(Qt::PointingHandCursor);
    connect(m_apply, &QPushButton::clicked, this, [this] { applyPending(); });

    m_ok = new QPushButton(QStringLiteral("OK"), this);
    m_ok->setGeometry(kX + 2 * (buttonW + kFooterGap), kFooterY, buttonW,
                      kFooterH);
    m_ok->setCursor(Qt::PointingHandCursor);
    m_ok->setStyleSheet(SankoTheme::themed(
        "QPushButton { background:%ACCENT%; color:#ffffff; border:none; "
        "border-radius:3px; font-family:Inter; font-size:11px; "
        "font-weight:600; }"
        "QPushButton:hover { background:#8d80f8; }"
        "QPushButton:disabled { background:#3d3766; color:#8a86a8; }"));
    connect(m_ok, &QPushButton::clicked, this, [this] {
        if (applyPending())
            accept();
    });

    connect(m_name, &StudioTextField::textEdited, this,
            [this] { revalidate(); });
    connect(m_name, &StudioTextField::submitted, this, [this] {
        if (applyPending())
            accept();
    });

    setTabOrder(m_name, m_fps);
    setTabOrder(m_fps, m_cancel);
    setTabOrder(m_cancel, m_apply);
    setTabOrder(m_apply, m_ok);
    revalidate();
}

QString ProjectSettingsDialog::projectName() const
{
    return m_name->text().trimmed();
}

int ProjectSettingsDialog::fps() const
{
    const int index = m_fps->currentIndex();
    return (index >= 0 && index < m_fpsValues.size()) ? m_fpsValues.at(index)
                                                      : 24;
}

QString ProjectSettingsDialog::resolutionText(const QSize &size)
{
    return QStringLiteral("%1 \xC3\x97 %2").arg(size.width()).arg(size.height());
}

QString ProjectSettingsDialog::aspectRatioText(const QSize &size)
{
    if (size.width() <= 0 || size.height() <= 0)
        return QStringLiteral("\xE2\x80\x94");
    const int g = std::gcd(size.width(), size.height());
    const int rw = size.width() / g, rh = size.height() / g;
    const double ratio = double(size.width()) / double(size.height());
    // Familiar ratios read as "16:9"; odd canvases (777x1013 reduces to
    // itself) read better as the decimal with the raw ratio beside it.
    if (rw <= 64 && rh <= 64)
        return QStringLiteral("%1:%2  (%3)").arg(rw).arg(rh)
            .arg(ratio, 0, 'f', 2);
    return QStringLiteral("%1:%2  (%3)").arg(size.width()).arg(size.height())
        .arg(ratio, 0, 'f', 2);
}

QString ProjectSettingsDialog::validate() const
{
    const QString name = projectName();
    if (name.isEmpty())
        return QStringLiteral("Project name is empty.");
    static const QString illegal = QStringLiteral("<>:\"/\\|?*");
    for (const QChar &c : name)
        if (illegal.contains(c))
            return QStringLiteral("Name cannot contain  < > : \" / \\ | ? *");
    return QString();
}

void ProjectSettingsDialog::revalidate()
{
    m_reason = validate();
    m_apply->setEnabled(m_reason.isEmpty());
    m_ok->setEnabled(m_reason.isEmpty());
    update();
}

bool ProjectSettingsDialog::applyPending()
{
    revalidate();
    if (!m_reason.isEmpty())
        return false;
    emit applied(projectName(), fps());
    return true;
}

QRect ProjectSettingsDialog::dragHeaderBand() const
{
    // Everything above the first field: the "General" header and its rule.
    // The first control starts at kFormY + 16, so this band never overlaps
    // a control — presses there go to the control, never to a drag.
    return QRect(0, 0, width(), kFormY);
}

void ProjectSettingsDialog::mousePressEvent(QMouseEvent *event)
{
    if (m_drag.press(this, event, dragHeaderBand())) {
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void ProjectSettingsDialog::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag.move(this, event)) {
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void ProjectSettingsDialog::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drag.release()) {
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

void ProjectSettingsDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        reject(); // Cancel: pending edits are discarded
        return;
    }
    QDialog::keyPressEvent(event);
}

void ProjectSettingsDialog::paintEvent(QPaintEvent *)
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

    // Section headers with underline rules.
    p.setFont(headerFont());
    p.setPen(kHeaderText);
    p.drawText(QRect(kX, kGeneralHeaderY, kW, 14), Qt::AlignLeft,
               QStringLiteral("General"));
    p.drawText(QRect(kX, kCanvasHeaderY, kW, 14), Qt::AlignLeft,
               QStringLiteral("Canvas"));
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.drawLine(kX, kGeneralHeaderY + 20, kX + kW, kGeneralHeaderY + 20);
    p.drawLine(kX, kCanvasHeaderY + 20, kX + kW, kCanvasHeaderY + 20);

    // Field labels.
    p.setFont(studio::fieldLabelFont());
    p.setPen(studio::kFieldLabel);
    p.drawText(QRect(kX, kFormY, kW, 12), Qt::AlignLeft,
               QStringLiteral("Project Name"));
    p.drawText(QRect(kX, kFormY + kFieldPitch, kW, 12), Qt::AlignLeft,
               QStringLiteral("Frame Rate"));

    // Canvas facts (read-only in this part).
    p.drawText(QRect(kX, kCanvasInfoY, 120, 12), Qt::AlignLeft,
               QStringLiteral("Current resolution"));
    p.drawText(QRect(kX, kCanvasInfoY + 20, 120, 12), Qt::AlignLeft,
               QStringLiteral("Aspect ratio"));
    p.setFont(studio::fieldFont());
    p.setPen(kInfoText);
    p.drawText(QRect(kX + 128, kCanvasInfoY - 1, kW - 128, 14), Qt::AlignLeft,
               resolutionText(m_canvasSize));
    p.drawText(QRect(kX + 128, kCanvasInfoY + 19, kW - 128, 14), Qt::AlignLeft,
               aspectRatioText(m_canvasSize));

    // Validation reason, above the footer.
    if (!m_reason.isEmpty()) {
        p.setFont(studio::fieldLabelFont());
        p.setPen(studio::kFieldLabel);
        p.drawText(QRect(kX, kFooterY - 16, kW, 12), Qt::AlignLeft, m_reason);
    }
}
