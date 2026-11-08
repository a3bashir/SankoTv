#include "NewProjectDialog.h"
#include "RecentProjects.h"
#include "SankoTheme.h"
#include "StageMarginDialog.h" // the stage margin's shared presets and note rules
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
// The last field ends at 400 (298 before Phase 2A's two margin rows); the
// validation line sits under it and the buttons under that, leaving the
// same 18 px at the bottom as at the sides.
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

    // THE STAGE MARGIN (Phase 2A, Figma 2A-1): a share of the shot on
    // every side, or Custom; the two fields below are the margin across and
    // down, px EACH SIDE, dimmed and locked while a preset owns them - the
    // Width and Height rule.
    QStringList marginLabels;
    for (int i = 0; i < stagemargin::kPresetCount; ++i)
        marginLabels << stagemargin::presetLabel(i);
    m_margin = new StudioDropdown(marginLabels, this);
    m_margin->setObjectName(QStringLiteral("newProjectStageMargin"));
    m_margin->setGeometry(kLeftX, fieldY(4), kLeftW, kBoxH);
    connect(m_margin, &StudioDropdown::chosen, this, &NewProjectDialog::applyMarginPreset);
    m_across = new StudioTextField(this);
    m_across->setGeometry(kLeftX, fieldY(5), kDimW, kBoxH);
    m_across->setNumericMode(0, 99999); // range enforced by validate()
    m_down = new StudioTextField(this);
    m_down->setGeometry(kLeftX + kDimW + kDimGap, fieldY(5), kDimW, kBoxH);
    m_down->setNumericMode(0, 99999);

    m_fps = new StudioDropdown({QStringLiteral("24 fps"),
                                QStringLiteral("25 fps"),
                                QStringLiteral("30 fps"),
                                QStringLiteral("60 fps")},
                               this);
    m_fps->setGeometry(kLeftX, fieldY(6), kLeftW, kBoxH);

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
    for (StudioTextField *f : {m_name, m_location, m_width, m_height, m_across, m_down}) {
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
    setTabOrder(m_height, m_margin);
    setTabOrder(m_margin, m_across);
    setTabOrder(m_across, m_down);
    setTabOrder(m_down, m_fps);
    setTabOrder(m_fps, m_create);
    setTabOrder(m_create, m_cancel);

    applyMarginPreset(0); // None: the project every older build made
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
int NewProjectDialog::canvasWidth() const
{
    return m_width->intValue() + 2 * qMax(0, m_across->intValue());
}
int NewProjectDialog::canvasHeight() const
{
    return m_height->intValue() + 2 * qMax(0, m_down->intValue());
}
QSize NewProjectDialog::stageMargin() const
{
    return QSize(qMax(0, m_across->intValue()), qMax(0, m_down->intValue()));
}

void NewProjectDialog::applyMarginPreset(int index)
{
    const bool custom = index == stagemargin::kCustom;
    m_across->setFieldEnabled(custom);
    m_down->setFieldEnabled(custom);
    if (!custom) {
        const QSize m = stagemargin::presetMargin(
            index, QSize(m_width->intValue(), m_height->intValue()));
        m_across->setText(QString::number(qMax(0, m.width())));
        m_down->setText(QString::number(qMax(0, m.height())));
    }
    revalidate();
    update(); // the note on the Stage Margin line
}

// The note on the Stage Margin line (Figma 2A-1): the stage and what one
// layer of it weighs; with none, that the stage is the shot.
QString NewProjectDialog::stageNote() const
{
    if (!m_across || stageMargin() == QSize(0, 0))
        return QStringLiteral("No margin: the stage is the frame");
    const QSize stage(canvasWidth(), canvasHeight());
    return QStringLiteral("Stage %1 × %2 · %3 MB a layer")
        .arg(stage.width())
        .arg(stage.height())
        .arg(stagemargin::layerMegabytes(stage), 0, 'f', 1);
}

bool NewProjectDialog::stageNoteHeavyForTest() const
{
    return stageMargin() != QSize(0, 0)
        && stagemargin::heavy(QSize(canvasWidth(), canvasHeight()));
}

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
    // A margin preset is a share of the shot: it follows the new size.
    if (m_margin && m_margin->currentIndex() != stagemargin::kCustom)
        applyMarginPreset(m_margin->currentIndex());
    revalidate();
    update(); // the 1080p camera hint comes and goes with the preset
}

// THE 1080p CAMERA HINT (the user's decision 6, Phase 1B): beside the
// default preset, one line saying a camera push-in will look soft there and
// 4K is recommended for camera work. The default does not change. Drawn on
// the Canvas Preset label's line, right-aligned in the note grey; the
// longest wording that fits the line is used.
QString NewProjectDialog::canvasHint() const
{
    if (!m_preset || m_preset->currentIndex() != 0)
        return QString();
    const QFontMetrics fm(studio::fieldLabelFont());
    const int room = kLeftW - fm.horizontalAdvance(QStringLiteral("Canvas Preset")) - 12;
    for (const QString &text : {
             QStringLiteral("Camera push-ins look soft here; 4K is recommended for camera work"),
             QStringLiteral("Camera push-ins look soft; 4K recommended for camera work"),
             QStringLiteral("Push-ins look soft; 4K recommended for camera work"),
             QStringLiteral("Push-ins look soft; 4K for camera work")})
        if (fm.horizontalAdvance(text) <= room)
            return text;
    return fm.elidedText(QStringLiteral("Push-ins look soft; 4K for camera work"),
                         Qt::ElideRight, room);
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
    // The stage (Phase 2A) is the canvas: the same cap holds for it.
    if (m_across->intValue() < 0 || m_down->intValue() < 0)
        return QStringLiteral("The margin cannot be less than 0.");
    if (canvasWidth() > kDimMax || canvasHeight() > kDimMax)
        return QStringLiteral("The stage can be at most %1 × %1; this would be "
                              "%2 × %3.")
            .arg(kDimMax)
            .arg(canvasWidth())
            .arg(canvasHeight());
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
    // The shot's size, only with a stage margin (Phase 2A): ProjectIO's rule.
    if (stageMargin() != QSize(0, 0)) {
        root[QStringLiteral("frameWidth")] = m_width->intValue();
        root[QStringLiteral("frameHeight")] = m_height->intValue();
    }
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
    s.insert(QStringLiteral("stageMargin"), m_margin->currentText());
    s.insert(QStringLiteral("marginW"), stageMargin().width());
    s.insert(QStringLiteral("marginH"), stageMargin().height());
    s.insert(QStringLiteral("stageNote"), stageNote());
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
                            nullptr, "Stage Margin (for new panels)", nullptr, "Frame Rate"};
    for (int i = 0; i < 7; ++i)
        if (labels[i])
            p.drawText(QRect(kLeftX, kFormY + i * kFieldPitch, kLeftW, 12),
                       Qt::AlignLeft, QLatin1String(labels[i]));
    p.drawText(QRect(kLeftX, kFormY + 3 * kFieldPitch, kDimW, 12),
               Qt::AlignLeft, QStringLiteral("Width"));
    p.drawText(QRect(kLeftX + kDimW + kDimGap, kFormY + 3 * kFieldPitch,
                     kDimW, 12),
               Qt::AlignLeft, QStringLiteral("Height"));
    // The margin, each side (Phase 2A, Figma 2A-1).
    p.drawText(QRect(kLeftX, kFormY + 5 * kFieldPitch, kDimW + kDimGap, 12),
               Qt::AlignLeft, QStringLiteral("Across, each side (px)"));
    p.drawText(QRect(kLeftX + kDimW + kDimGap, kFormY + 5 * kFieldPitch, kDimW, 12),
               Qt::AlignLeft, QStringLiteral("Down, each side (px)"));
    // The note on the Stage Margin line: the stage and one layer's weight,
    // amber past a 4K layer (the user's Q1).
    p.setPen(stageNoteHeavyForTest() ? QColor(0xe0, 0xa8, 0x60) : kNoteText);
    p.drawText(QRect(kLeftX, kFormY + 4 * kFieldPitch, kLeftW, 12), Qt::AlignRight,
               stageNote());
    p.setPen(studio::kFieldLabel);
    // The 1080p camera hint, right-aligned on the Canvas Preset line.
    const QString hint = canvasHint();
    if (!hint.isEmpty()) {
        p.setPen(kNoteText);
        p.drawText(QRect(kLeftX, kFormY + 2 * kFieldPitch, kLeftW, 12),
                   Qt::AlignRight, hint);
    }
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
