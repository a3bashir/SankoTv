#include "NewProjectDialog.h"
#include "SankoScrollBarStyle.h"
#include "SankoTheme.h"
#include "brushlib/StudioControls.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTemporaryFile>

using brushlib::StudioDropdown;
using brushlib::StudioTextField;
namespace studio = brushlib::studio;

namespace {
// Geometry from Figma 350:24 (680x460): left form column, 1px divider,
// darker right column whose 224px content strip is right-anchored.
constexpr int kDialogW = 680;
constexpr int kDialogH = 460;
constexpr int kDividerX = 342;
constexpr int kLeftX = 18;         // left content x
constexpr int kLeftW = 229;        // left content width
constexpr int kRightX = 438;       // right content x (420 + 18 pad)
constexpr int kRightW = 224;       // right content width
constexpr int kHeaderY = 18;       // section headers
constexpr int kFormY = 53;         // first field top
constexpr int kFieldPitch = 51;    // 41-tall field + 10 gap
constexpr int kBoxH = 25;
constexpr int kFooterY = 409;      // Create / Open buttons top
constexpr int kFooterH = 33;

const QColor kDialogBg(0x11, 0x11, 0x11);
const QColor kRightBg(0x0a, 0x0a, 0x0a);
const QColor kHeaderText(0xcc, 0xcc, 0xcc);
const QColor kNoteText(0x66, 0x66, 0x66);

constexpr int kRecentCap = 10;

const char *kPresetNames[] = {"HDTV 1080p", "2K", "4K", "Custom"};
constexpr int kPresetDims[][2] = {{1920, 1080}, {2048, 1080}, {3840, 2160}};
constexpr int kCustomPreset = 3;
constexpr int kFpsValues[] = {24, 25, 30, 60};
constexpr int kDimMin = 64, kDimMax = 8192;

QString g_settingsOverride; // seam: scratch ini path

QSettings recentSettings()
{
    if (!g_settingsOverride.isEmpty())
        return QSettings(g_settingsOverride, QSettings::IniFormat);
    return QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
}

QFont headerFont()
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    return f;
}

// The 16px film-strip glyph in a recent entry's thumbnail well (design
// 350:109) — painted, no asset: frame outline + sprocket holes.
void paintFilmIcon(QPainter &p, const QRectF &r, const QColor &color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.6, 0.6, -0.6, -0.6), 2, 2);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    for (int i = 0; i < 3; ++i) {
        const qreal y = r.top() + 3.2 + i * (r.height() - 6.4) / 2.0;
        p.drawRect(QRectF(r.left() + 2.2, y - 0.9, 1.8, 1.8));
        p.drawRect(QRectF(r.right() - 4.0, y - 0.9, 1.8, 1.8));
    }
    p.restore();
}

// The first panel's flattened PNG, saved beside every project by
// saveToPath as panel_s0_p0.png — the natural thumbnail source.
QPixmap projectThumbnail(const QString &projectPath)
{
    const QString png = QFileInfo(projectPath).absolutePath()
        + QStringLiteral("/panel_s0_p0.png");
    return QFileInfo::exists(png) ? QPixmap(png) : QPixmap();
}
} // namespace

// ---------------------------------------------------------------------------
// Recent-projects persistence
// ---------------------------------------------------------------------------

void NewProjectDialog::setSettingsOverride(const QString &iniPath)
{
    g_settingsOverride = iniPath;
}

QVector<NewProjectDialog::RecentEntry> NewProjectDialog::recentProjects()
{
    QSettings s = recentSettings();
    QVector<RecentEntry> out;
    const int n = s.beginReadArray(QStringLiteral("recentProjects"));
    for (int i = 0; i < n && i < kRecentCap; ++i) {
        s.setArrayIndex(i);
        RecentEntry e;
        e.path = s.value(QStringLiteral("path")).toString();
        e.lastOpened = QDateTime::fromString(
            s.value(QStringLiteral("lastOpened")).toString(), Qt::ISODate);
        if (!e.path.isEmpty())
            out.append(e);
    }
    s.endArray();
    return out;
}

static void writeRecents(const QVector<NewProjectDialog::RecentEntry> &list)
{
    QSettings s = recentSettings();
    s.beginWriteArray(QStringLiteral("recentProjects"),
                      int(qMin(list.size(), qsizetype(kRecentCap))));
    for (int i = 0; i < list.size() && i < kRecentCap; ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("path"), list.at(i).path);
        s.setValue(QStringLiteral("lastOpened"),
                   list.at(i).lastOpened.toString(Qt::ISODate));
    }
    s.endArray();
}

void NewProjectDialog::recordRecentProject(const QString &path)
{
    QVector<RecentEntry> list = recentProjects();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list.at(i).path.compare(path, Qt::CaseInsensitive) == 0)
            list.removeAt(i);
    list.prepend({path, QDateTime::currentDateTime()});
    writeRecents(list);
}

void NewProjectDialog::removeRecentProject(const QString &path)
{
    QVector<RecentEntry> list = recentProjects();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list.at(i).path.compare(path, Qt::CaseInsensitive) == 0)
            list.removeAt(i);
    writeRecents(list);
}

// ---------------------------------------------------------------------------
// RecentList — the right column's entry list (custom painted rows)
// ---------------------------------------------------------------------------

class NewProjectDialog::RecentList : public QWidget
{
public:
    static constexpr int kRowH = 44;
    static constexpr int kRowPitch = 50;

    RecentList(NewProjectDialog *owner) : m_owner(owner)
    {
        reload();
        setMouseTracking(true);
    }
    void reload()
    {
        m_entries = recentProjects();
        m_selected = -1;
        setFixedSize(kRightW,
                     qMax(1, int(m_entries.size()) * kRowPitch - 6));
        update();
    }
    bool empty() const { return m_entries.isEmpty(); }
    QString selectedPath() const
    {
        return m_selected >= 0 ? m_entries.at(m_selected).path : QString();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < m_entries.size(); ++i) {
            const RecentEntry &e = m_entries.at(i);
            const QRect row(0, i * kRowPitch, width(), kRowH);
            const bool missing = !QFileInfo::exists(e.path);
            if (i == m_selected) {
                p.setPen(QPen(studio::kAccent, 1.0));
                p.setBrush(studio::kFieldBg);
                p.drawRoundedRect(QRectF(row).adjusted(0.5, 0.5, -0.5, -0.5),
                                  4, 4);
            } else if (i == m_hover) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 10));
                p.drawRoundedRect(row, 4, 4);
            }
            // A missing project stays listed but visibly dimmed at 50%.
            p.setOpacity(missing ? 0.5 : 1.0);
            const QRectF thumb(row.left() + 6, row.top() + 6, 48, 32);
            p.setPen(QPen(studio::kFieldBorder, 1.0));
            p.setBrush(studio::kFieldBg);
            p.drawRoundedRect(thumb.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
            const QPixmap px = missing ? QPixmap() : projectThumbnail(e.path);
            if (!px.isNull()) {
                p.save();
                QPainterPath clip;
                clip.addRoundedRect(thumb.adjusted(1, 1, -1, -1), 2, 2);
                p.setClipPath(clip);
                p.drawPixmap(thumb.adjusted(1, 1, -1, -1).toRect(),
                             px.scaled(46, 30, Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation));
                p.restore();
            } else {
                paintFilmIcon(p, QRectF(thumb.center().x() - 8,
                                        thumb.center().y() - 8, 16, 16),
                              studio::kFieldLabel);
            }
            // Title: MIDDLE-elided so version-suffixed families keep their
            // distinguishing tail (Cyberpunk_Alley_v1/v2/v3).
            QFont title(QStringLiteral("Inter"));
            title.setPixelSize(11);
            title.setWeight(QFont::Medium);
            p.setFont(title);
            p.setPen(studio::kFieldText);
            const QString name = QFileInfo(e.path).completeBaseName();
            const QRect titleR(row.left() + 64, row.top() + 8, 154, 14);
            p.drawText(titleR, Qt::AlignVCenter | Qt::AlignLeft,
                       QFontMetrics(title).elidedText(name, Qt::ElideMiddle,
                                                      titleR.width()));
            QFont date(QStringLiteral("Inter"));
            date.setPixelSize(10);
            p.setFont(date);
            p.setPen(studio::kFieldLabel);
            p.drawText(QRect(row.left() + 64, row.top() + 23, 154, 13),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       QStringLiteral("Last opened: ")
                           + e.lastOpened.toString(
                               QStringLiteral("MMM d, yyyy")));
            p.setOpacity(1.0);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int i = rowAt(event->position().toPoint());
        if (i != m_hover) {
            m_hover = i;
            update();
        }
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        const int i = rowAt(event->position().toPoint());
        if (i < 0)
            return;
        const RecentEntry e = m_entries.at(i);
        if (!QFileInfo::exists(e.path)) {
            // Never a silent failure: offer to drop the dead entry.
            const auto pick = QMessageBox::question(
                m_owner, QStringLiteral("Project Not Found"),
                QStringLiteral("The project file was not found:\n%1\n\n"
                               "Remove it from Recent Projects?")
                    .arg(e.path),
                QMessageBox::Yes | QMessageBox::No);
            if (pick == QMessageBox::Yes) {
                removeRecentProject(e.path);
                reload();
            }
            return;
        }
        m_selected = i;
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const int i = rowAt(event->position().toPoint());
        if (i >= 0 && QFileInfo::exists(m_entries.at(i).path)) {
            m_selected = i;
            m_owner->openSelectedOrDialog();
        }
    }

private:
    int rowAt(const QPoint &pos) const
    {
        const int i = pos.y() / kRowPitch;
        return (i >= 0 && i < m_entries.size()
                && pos.y() % kRowPitch < kRowH)
            ? i
            : -1;
    }
    NewProjectDialog *m_owner;
    QVector<RecentEntry> m_entries;
    int m_selected = -1;
    int m_hover = -1;
};

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
    m_location->setGeometry(kLeftX, fieldY(1), 158, kBoxH);
    m_location->setText(QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + QStringLiteral("/SankoTV")));

    m_browse = new QPushButton(QStringLiteral("Browse..."), this);
    m_browse->setGeometry(kLeftX + 164, fieldY(1), 65, kBoxH);
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
    m_width->setGeometry(kLeftX, fieldY(3), 98, kBoxH);
    m_width->setNumericMode(1, 99999); // range enforced by validate()
    m_height = new StudioTextField(this);
    m_height->setGeometry(kLeftX + 131, fieldY(3), 98, kBoxH);
    m_height->setNumericMode(1, 99999);

    m_fps = new StudioDropdown({QStringLiteral("24 fps"),
                                QStringLiteral("25 fps"),
                                QStringLiteral("30 fps"),
                                QStringLiteral("60 fps")},
                               this);
    m_fps->setGeometry(kLeftX, fieldY(4), kLeftW, kBoxH);

    m_create = new QPushButton(QStringLiteral("Create Project"), this);
    m_create->setGeometry(kLeftX, kFooterY, kLeftW, kFooterH);
    m_create->setCursor(Qt::PointingHandCursor);
    // White label on the kAccent fill (3.87:1): the recorded filled-button
    // exemption — matches the studio's Done button (see SankoTheme.h).
    m_create->setStyleSheet(SankoTheme::themed(QStringLiteral(
        "QPushButton { background:%ACCENT%; color:#ffffff; border:none; "
        "border-radius:3px; font-family:Inter; font-size:11px; "
        "font-weight:600; }"
        "QPushButton:hover { background:#8d80f8; }"
        "QPushButton:disabled { background:#3d3766; color:#8a86a8; }")
        .toUtf8().constData()));
    connect(m_create, &QPushButton::clicked, this,
            &NewProjectDialog::attemptCreate);

    m_recent = new RecentList(this);
    m_recentScroll = new QScrollArea(this);
    m_recentScroll->setGeometry(kRightX, kFormY, kRightW, 342);
    m_recentScroll->setWidget(m_recent);
    m_recentScroll->setFrameShape(QFrame::NoFrame);
    m_recentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_recentScroll->setStyleSheet(
        QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget "
                       "{ background: transparent; }")
        + sankoScrollBarStyle());

    m_open = new QPushButton(QStringLiteral("Open Project..."), this);
    m_open->setGeometry(kRightX, kFooterY, kRightW, kFooterH);
    m_open->setCursor(Qt::PointingHandCursor);
    m_open->setStyleSheet(QStringLiteral(
        "QPushButton { background:#1c1c1c; color:#cccccc; border:1px solid "
        "#333333; border-radius:3px; font-family:Inter; font-size:11px; "
        "font-weight:500; }"
        "QPushButton:hover { border-color:#4a4a4a; color:#ffffff; }"));
    connect(m_open, &QPushButton::clicked, this,
            &NewProjectDialog::openSelectedOrDialog);

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
    setTabOrder(m_create, m_open);

    applyPreset(0); // HDTV 1080p default: 1920x1080, dims locked
    revalidate();
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
    if (!QDir(loc).exists())
        return QStringLiteral("The save location does not exist.");
    if (!probeWritable(loc))
        return QStringLiteral("The save location is not writable.");
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

    recordRecentProject(file);
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

void NewProjectDialog::openSelectedOrDialog()
{
    QString path = m_recent->selectedPath();
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Project"), QDir::homePath(),
            QStringLiteral("SankoTV Project (*.sankotv)"));
        if (path.isEmpty())
            return;
    }
    m_openPath = path;
    m_mode = Mode::OpenExisting;
    accept();
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

void NewProjectDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Chrome: #111 rounded surface, #333 border; the right column sits on
    // the darker #0a0a0a, clipped to the same rounded silhouette.
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath silhouette;
    silhouette.addRoundedRect(r, 4, 4);
    p.fillPath(silhouette, kDialogBg);
    p.save();
    p.setClipPath(silhouette);
    p.fillRect(QRectF(kDividerX + 1, 0, width() - kDividerX - 1, height()),
               kRightBg);
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.drawLine(QPointF(kDividerX + 0.5, 0), QPointF(kDividerX + 0.5, height()));
    p.restore();
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 4, 4);

    // Section headers with their underline rules.
    p.setFont(headerFont());
    p.setPen(kHeaderText);
    p.drawText(QRect(kLeftX, kHeaderY, kLeftW, 14), Qt::AlignLeft,
               QStringLiteral("Create New Project"));
    p.drawText(QRect(kRightX, kHeaderY, kRightW, 14), Qt::AlignLeft,
               QStringLiteral("Recent Projects"));
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.drawLine(kLeftX, kHeaderY + 20, kLeftX + kLeftW, kHeaderY + 20);
    p.drawLine(kRightX, kHeaderY + 20, kRightX + kRightW, kHeaderY + 20);

    // Field labels.
    p.setFont(studio::fieldLabelFont());
    p.setPen(studio::kFieldLabel);
    const char *labels[] = {"Project Name", "Save Location", "Canvas Preset",
                            nullptr, "Frame Rate"};
    for (int i = 0; i < 5; ++i)
        if (labels[i])
            p.drawText(QRect(kLeftX, kFormY + i * kFieldPitch, kLeftW, 12),
                       Qt::AlignLeft, QLatin1String(labels[i]));
    p.drawText(QRect(kLeftX, kFormY + 3 * kFieldPitch, 98, 12), Qt::AlignLeft,
               QStringLiteral("Width"));
    p.drawText(QRect(kLeftX + 131, kFormY + 3 * kFieldPitch, 98, 12),
               Qt::AlignLeft, QStringLiteral("Height"));
    // The x between the dimension fields (design 350:51, #666).
    p.setFont(studio::fieldFont());
    p.setPen(kNoteText);
    p.drawText(QRect(kLeftX + 100, kFormY + 3 * kFieldPitch + 16, 29, kBoxH),
               Qt::AlignCenter, QStringLiteral("\xC3\x97"));

    // (The stored-not-applied note is gone: since the resolution epic the
    // chosen dimensions ARE the project's real canvas size.)

    // The one-line validation reason, above the Create button.
    if (!m_reason.isEmpty()) {
        p.setFont(studio::fieldLabelFont());
        p.setPen(studio::kFieldLabel);
        p.drawText(QRect(kLeftX, kFooterY - 16, kLeftW, 12),
                   Qt::AlignLeft, m_reason);
    }

    // Empty first-run state: a single dimmed centred line.
    if (m_recent->empty()) {
        p.setFont(studio::fieldFont());
        p.setPen(studio::kFieldLabel);
        p.drawText(QRect(kRightX, kFormY, kRightW, 342), Qt::AlignCenter,
                   QStringLiteral("No recent projects"));
    }
}
