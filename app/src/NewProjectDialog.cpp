#include "NewProjectDialog.h"
#include "RecentProjects.h"
#include "SankoTheme.h"
#include "brushlib/StudioControls.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTemporaryFile>

using brushlib::StudioDropdown;
using brushlib::StudioTextField;
namespace studio = brushlib::studio;

namespace {
// One column. The vertical rhythm is Figma 350:24's form column unchanged
// (header at 18, first field at 53, 51 px pitch, 25 px boxes); the width is
// what that column becomes when it is the whole window: 18 px of padding
// either side of a 304 px content strip.
constexpr int kDialogW = NewProjectDialog::kWidth;   // 340
constexpr int kDialogH = NewProjectDialog::kHeight;  // 385
constexpr int kLeftX = 18;         // content x
constexpr int kLeftW = kDialogW - 2 * kLeftX; // content width (304)
constexpr int kHeaderY = 18;       // section header
constexpr int kFormY = 53;         // first field top
constexpr int kFieldPitch = 51;    // 41-tall field + 10 gap
constexpr int kBoxH = 25;
constexpr int kBrowseW = 65;       // the design's Browse button
constexpr int kBrowseGap = 6;
constexpr int kDimGap = 32;        // between Width and Height (holds the x)
constexpr int kDimW = (kLeftW - kDimGap) / 2; // 136
// The last field ends at 298; the validation line sits under it and the
// buttons under that, leaving the same 18 px at the bottom as at the sides.
constexpr int kFooterH = 33;
constexpr int kFooterY = kDialogH - kLeftX - kFooterH; // 334
constexpr int kCancelW = 96;
constexpr int kFooterGap = 8;

const QColor kDialogBg(0x11, 0x11, 0x11);
const QColor kHeaderText(0xcc, 0xcc, 0xcc);
const QColor kNoteText(0x66, 0x66, 0x66);

const char *kPresetNames[] = {"HDTV 1080p", "2K", "4K", "Custom"};
constexpr int kPresetDims[][2] = {{1920, 1080}, {2048, 1080}, {3840, 2160}};
constexpr int kCustomPreset = 3;
constexpr int kFpsValues[] = {24, 25, 30, 60};
constexpr int kDimMin = 64, kDimMax = 8192;

// The folder the dialog offers by default: our own directory under
// Documents. It is CREATED here rather than merely proposed, because Browse
// opens at whatever this says and a file dialog rooted at a path that does
// not exist is its own small mess. If it cannot be created, fall back to
// Documents itself (which exists by definition — it is writableLocation),
// and to home if even that is unavailable.
QString g_defaultLocationForTest; // verification: see the header

QString defaultSaveLocation()
{
    if (!g_defaultLocationForTest.isEmpty())
        return g_defaultLocationForTest;
    const QString documents =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!documents.isEmpty()) {
        const QString ours = documents + QStringLiteral("/SankoTV");
        if (QDir(ours).exists() || QDir().mkpath(ours))
            return ours;
        if (QDir(documents).exists())
            return documents;
    }
    return QDir::homePath();
}

QFont headerFont()
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    return f;
}

// (The Recent Projects list that was painted here - its rows, its film
// glyph, its thumbnail cache - is RecentProjectsView and RecentThumbnails
// now, on the start window.)
} // namespace

// ---------------------------------------------------------------------------
// NewProjectDialog
// ---------------------------------------------------------------------------

NewProjectDialog::NewProjectDialog(QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
{
    setModal(true);
    setFixedSize(kDialogW, kDialogH);
    setAttribute(Qt::WA_TranslucentBackground); // rounded corners

    auto fieldY = [](int index) { return kFormY + index * kFieldPitch + 16; };

    m_name = new StudioTextField(this);
    m_name->setGeometry(kLeftX, fieldY(0), kLeftW, kBoxH);
    m_name->setText(QStringLiteral("Untitled_Storyboard"));

    m_location = new StudioTextField(this);
    m_location->setGeometry(kLeftX, fieldY(1),
                            kLeftW - kBrowseW - kBrowseGap, kBoxH);
    m_location->setText(QDir::toNativeSeparators(defaultSaveLocation()));

    m_browse = new QPushButton(QStringLiteral("Browse..."), this);
    m_browse->setGeometry(kLeftX + kLeftW - kBrowseW, fieldY(1), kBrowseW,
                          kBoxH);
    m_browse->setCursor(Qt::PointingHandCursor);
    m_browse->setStyleSheet(QStringLiteral(
        "QPushButton { background:#1c1c1c; color:#999999; border:1px solid "
        "#333333; border-radius:3px; font-family:Inter; font-size:10px; "
        "font-weight:500; }"
        "QPushButton:hover { border-color:#4a4a4a; color:#cccccc; }"));
    connect(m_browse, &QPushButton::clicked, this, &NewProjectDialog::browse);

    m_preset = new StudioDropdown(
        {kPresetNames[0], kPresetNames[1], kPresetNames[2], kPresetNames[3]},
        this);
    m_preset->setGeometry(kLeftX, fieldY(2), kLeftW, kBoxH);
    connect(m_preset, &StudioDropdown::chosen, this,
            &NewProjectDialog::applyPreset);

    m_width = new StudioTextField(this);
    m_width->setGeometry(kLeftX, fieldY(3), kDimW, kBoxH);
    m_width->setNumericMode(1, 99999); // range enforced by validate()
    m_height = new StudioTextField(this);
    m_height->setGeometry(kLeftX + kDimW + kDimGap, fieldY(3), kDimW, kBoxH);
    m_height->setNumericMode(1, 99999);

    m_fps = new StudioDropdown({QStringLiteral("24 fps"),
                                QStringLiteral("25 fps"),
                                QStringLiteral("30 fps"),
                                QStringLiteral("60 fps")},
                               this);
    m_fps->setGeometry(kLeftX, fieldY(4), kLeftW, kBoxH);

    // Footer: Cancel | Create Project. Create is the filled accent button
    // and takes the width Cancel leaves.
    m_create = new QPushButton(QStringLiteral("Create Project"), this);
    m_create->setGeometry(kLeftX + kCancelW + kFooterGap, kFooterY,
                          kLeftW - kCancelW - kFooterGap, kFooterH);
    m_create->setCursor(Qt::PointingHandCursor);
    // White label on the kAccent fill (3.87:1): the recorded filled-button
    // exemption — matches the studio's Done button (see SankoTheme.h).
    m_create->setStyleSheet(SankoTheme::themed(QStringLiteral(
        "QPushButton { background:%ACCENT%; color:#ffffff; border:none; "
        "border-radius:3px; font-family:Inter; font-size:11px; "
        "font-weight:600; }"
        "QPushButton:hover { background:%ACCENT_HOVER%; }"
        "QPushButton:disabled { background:%ACCENT_DISABLED%; "
        "color:%ACCENT_DISABLED_TEXT%; }")
        .toUtf8().constData()));
    connect(m_create, &QPushButton::clicked, this,
            &NewProjectDialog::attemptCreate);

    // Cancel: the secondary button, in the style Open Project had here.
    // Until it existed the only way out of this window was Escape, which a
    // frameless dialog gives no hint of.
    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setGeometry(kLeftX, kFooterY, kCancelW, kFooterH);
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setStyleSheet(QStringLiteral(
        "QPushButton { background:#1c1c1c; color:#cccccc; border:1px solid "
        "#333333; border-radius:3px; font-family:Inter; font-size:11px; "
        "font-weight:500; }"
        "QPushButton:hover { border-color:#4a4a4a; color:#ffffff; }"));
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);

    // Live validation + Enter-submits from any field.
    for (StudioTextField *f : {m_name, m_location, m_width, m_height}) {
        connect(f, &StudioTextField::textEdited, this,
                [this] { revalidate(); });
        connect(f, &StudioTextField::submitted, this,
                [this] { attemptCreate(); });
    }
    connect(m_preset, &StudioDropdown::chosen, this,
            [this] { revalidate(); });

    // Tab order: form top-to-bottom, then the two buttons.
    setTabOrder(m_name, m_location);
    setTabOrder(m_location, m_browse);
    setTabOrder(m_browse, m_preset);
    setTabOrder(m_preset, m_width);
    setTabOrder(m_width, m_height);
    setTabOrder(m_height, m_fps);
    setTabOrder(m_fps, m_create);
    setTabOrder(m_create, m_cancel);

    applyPreset(0); // HDTV 1080p default: 1920x1080, dims locked
    revalidate();
}

void NewProjectDialog::setDefaultLocationForTest(const QString &dir)
{
    g_defaultLocationForTest = dir;
}

QString NewProjectDialog::projectName() const
{
    return m_name->text().trimmed();
}
int NewProjectDialog::fps() const { return kFpsValues[m_fps->currentIndex()]; }
int NewProjectDialog::canvasWidth() const { return m_width->intValue(); }
int NewProjectDialog::canvasHeight() const { return m_height->intValue(); }

void NewProjectDialog::applyPreset(int index)
{
    if (index < kCustomPreset) {
        m_width->setText(QString::number(kPresetDims[index][0]));
        m_height->setText(QString::number(kPresetDims[index][1]));
        // Blocked VISIBLY, not silently: the fields dim and refuse input
        // while a preset owns them (the "allow manual editing when Custom
        // is selected" rule, made legible).
        m_width->setFieldEnabled(false);
        m_height->setFieldEnabled(false);
    } else {
        m_width->setFieldEnabled(true);
        m_height->setFieldEnabled(true);
    }
    revalidate();
}

// Writability is probed by actually creating a temp file — isWritable()
// lies on Windows. Memoized per path string.
static bool probeWritable(const QString &dir)
{
    static QString lastDir;
    static bool lastResult = false;
    if (dir == lastDir)
        return lastResult;
    QTemporaryFile probe(dir + QStringLiteral("/.sanko_probe_XXXXXX"));
    lastDir = dir;
    lastResult = probe.open();
    return lastResult;
}

QString NewProjectDialog::validate() const
{
    const QString name = projectName();
    if (name.isEmpty())
        return QStringLiteral("Project name is empty.");
    static const QString illegal = QStringLiteral("<>:\"/\\|?*");
    for (const QChar &c : name)
        if (illegal.contains(c))
            return QStringLiteral("Name cannot contain  < > : \" / \\ | ? *");
    const QString loc = m_location->text().trimmed();
    if (loc.isEmpty())
        return QStringLiteral("Choose a save location.");
    // A location that does not exist yet is NOT an error: Create already
    // calls mkpath, which builds the whole chain including this folder. The
    // dialog used to refuse here and so blocked itself on work it was about
    // to do anyway — with the default location missing, Create stayed
    // disabled for the dialog's entire life and nothing the artist typed
    // could help. Writability is probed on the nearest ancestor that DOES
    // exist, because probing a folder that is not there can only fail.
    QDir probe(loc);
    while (!probe.exists() && !probe.isRoot() && probe.cdUp()) {
    }
    if (!probe.exists())
        return QStringLiteral("That drive or folder is not available. Use "
                              "Browse to choose one.");
    if (!probeWritable(probe.absolutePath()))
        return QStringLiteral("This location is not writable: %1")
            .arg(QDir::toNativeSeparators(probe.absolutePath()));
    if (QFileInfo::exists(QDir(loc).filePath(name)))
        return QStringLiteral("A project with this name already exists "
                              "there.");
    const int w = m_width->intValue(), h = m_height->intValue();
    if (w < kDimMin || w > kDimMax || h < kDimMin || h > kDimMax)
        return QStringLiteral("Width and Height must be %1-%2.")
            .arg(kDimMin)
            .arg(kDimMax);
    return QString();
}

void NewProjectDialog::revalidate()
{
    m_reason = validate();
    m_create->setEnabled(m_reason.isEmpty());
    update();
}

void NewProjectDialog::attemptCreate()
{
    revalidate();
    if (!m_reason.isEmpty())
        return;

    // The disk can change between validation and now (the race): re-check
    // the collision at the moment of creation, and leave NOTHING behind on
    // any failure — the only thing we create is the project folder, so
    // cleanup is removing it if and only if we made it.
    const QString name = projectName();
    const QDir loc(m_location->text().trimmed());
    const QString folder = loc.filePath(name);
    if (QFileInfo::exists(folder)) {
        QMessageBox::warning(this, QStringLiteral("Create Project"),
                             QStringLiteral("A project with this name "
                                            "appeared at:\n%1")
                                 .arg(folder));
        revalidate();
        return;
    }
    if (!QDir().mkpath(folder)) {
        QMessageBox::warning(this, QStringLiteral("Create Project"),
                             QStringLiteral("Could not create:\n%1")
                                 .arg(folder));
        return;
    }

    // The skeleton project, shaped exactly as loadFromPath reads it. fps
    // is applied (animatic timing); canvasWidth/Height are stored,
    // forward-compatible metadata — the canvas renders 960x540 in this
    // build, and the dialog SAYS so (the note under the dimension fields).
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("projectName")] = name;
    root[QStringLiteral("fps")] = fps();
    root[QStringLiteral("canvasWidth")] = canvasWidth();
    root[QStringLiteral("canvasHeight")] = canvasHeight();
    root[QStringLiteral("scenes")] = QJsonArray();
    root[QStringLiteral("consistencyBoard")] = QJsonArray();
    root[QStringLiteral("audioPath")] = QString();
    root[QStringLiteral("perspective")] = QJsonObject();

    const QString file =
        folder + QStringLiteral("/") + name + QStringLiteral(".sankotv");
    QSaveFile out(file);
    bool ok = out.open(QIODevice::WriteOnly);
    if (ok) {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        ok = out.commit();
    }
    if (!ok) {
        QDir(folder).removeRecursively(); // we created it above
        QMessageBox::warning(this, QStringLiteral("Create Project"),
                             QStringLiteral("Could not write:\n%1").arg(file));
        return;
    }

    RecentProjects::record(file);
    m_createdFile = file;
    m_mode = Mode::Created;
    accept();
}

void NewProjectDialog::browse()
{
    // Native dialog, deliberately (the standing decision for file dialogs).
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose Save Location"), m_location->text());
    if (dir.isEmpty())
        return; // cancelled: path unchanged
    m_location->setText(QDir::toNativeSeparators(dir));
    revalidate();
}

void NewProjectDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (QWidget *host = parentWidget())
        move(host->window()->frameGeometry().center()
             - QPoint(width() / 2, height() / 2));
}

void NewProjectDialog::keyPressEvent(QKeyEvent *event)
{
    // Enter creates when valid; Escape cancels (QDialog's default reject).
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        attemptCreate();
        return;
    }
    QDialog::keyPressEvent(event);
}

QVariantMap NewProjectDialog::devrecState() const
{
    QVariantMap s;
    s.insert(QStringLiteral("projectName"), projectName());
    s.insert(QStringLiteral("location"), m_location->text());
    s.insert(QStringLiteral("preset"), m_preset->currentText());
    s.insert(QStringLiteral("canvasW"), canvasWidth());
    s.insert(QStringLiteral("canvasH"), canvasHeight());
    s.insert(QStringLiteral("fps"), fps());
    s.insert(QStringLiteral("validationReason"), m_reason);
    s.insert(QStringLiteral("createEnabled"), m_create && m_create->isEnabled());
    return s;
}

QRect NewProjectDialog::dragHeaderBand() const
{
    // Above the first field: the "Create New Project" header and its rule.
    // The first control sits at kFormY + 16, so the band never overlaps a
    // control.
    return QRect(0, 0, width(), kFormY);
}

void NewProjectDialog::mousePressEvent(QMouseEvent *event)
{
    if (m_drag.press(this, event, dragHeaderBand())) {
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void NewProjectDialog::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag.move(this, event)) {
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void NewProjectDialog::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drag.release()) {
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

void NewProjectDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Chrome: #111 rounded surface, #333 border.
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath silhouette;
    silhouette.addRoundedRect(r, 4, 4);
    p.fillPath(silhouette, kDialogBg);
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 4, 4);

    // The header with its underline rule.
    p.setFont(headerFont());
    p.setPen(kHeaderText);
    p.drawText(QRect(kLeftX, kHeaderY, kLeftW, 14), Qt::AlignLeft,
               QStringLiteral("Create New Project"));
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.drawLine(kLeftX, kHeaderY + 20, kLeftX + kLeftW, kHeaderY + 20);

    // Field labels.
    p.setFont(studio::fieldLabelFont());
    p.setPen(studio::kFieldLabel);
    const char *labels[] = {"Project Name", "Save Location", "Canvas Preset",
                            nullptr, "Frame Rate"};
    for (int i = 0; i < 5; ++i)
        if (labels[i])
            p.drawText(QRect(kLeftX, kFormY + i * kFieldPitch, kLeftW, 12),
                       Qt::AlignLeft, QLatin1String(labels[i]));
    p.drawText(QRect(kLeftX, kFormY + 3 * kFieldPitch, kDimW, 12),
               Qt::AlignLeft, QStringLiteral("Width"));
    p.drawText(QRect(kLeftX + kDimW + kDimGap, kFormY + 3 * kFieldPitch,
                     kDimW, 12),
               Qt::AlignLeft, QStringLiteral("Height"));
    // The x between the dimension fields (design 350:51, #666).
    p.setFont(studio::fieldFont());
    p.setPen(kNoteText);
    p.drawText(QRect(kLeftX + kDimW, kFormY + 3 * kFieldPitch + 16, kDimGap,
                     kBoxH),
               Qt::AlignCenter, QStringLiteral("\xC3\x97"));

    // (The stored-not-applied note is gone: since the resolution epic the
    // chosen dimensions ARE the project's real canvas size.)

    // The one-line validation reason, above the buttons. Elided at the
    // content width: the longer reasons name a path, and an unclipped one
    // ran past the window's edge.
    if (!m_reason.isEmpty()) {
        p.setFont(studio::fieldLabelFont());
        p.setPen(studio::kFieldLabel);
        p.drawText(QRect(kLeftX, kFooterY - 16, kLeftW, 12), Qt::AlignLeft,
                   QFontMetrics(studio::fieldLabelFont())
                       .elidedText(m_reason, Qt::ElideMiddle, kLeftW));
    }
}
