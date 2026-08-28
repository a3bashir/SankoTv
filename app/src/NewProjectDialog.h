#pragma once

#include "FramelessDialogDrag.h"

#include <QDateTime>
#include <QVariantMap>
#include <QDialog>
#include <QVector>

class QPushButton;
class QScrollArea;
namespace brushlib {
class StudioDropdown;
class StudioTextField;
}

// The New Project window (Figma 350:24 "new-project-dialog", 680x460).
// Frameless application-modal QDialog with painted chrome: left column the
// creation form, right column Recent Projects, per the design. Hosted
// modally (exec) so Enter/Escape and focus recovery come from Qt — unlike
// the Brush Settings studio's unmanaged FloatingToolWindow, which exists
// for canvas-level reasons (toolbar suppression) this window does not have.
//
// Create writes <SaveLocation>/<Name>/<Name>.sankotv IMMEDIATELY — a folder
// per project, because saving scatters sibling PNGs (panel flattens, layer
// images, consistency thumbnails) and containing them is the point. This
// diverges from File > Save As, which still writes wherever it is pointed.
//
// Recent projects: sankoSettings() key "recentProjects",
// entries {path, lastOpenedIso}, capped at kRecentCap, recorded by
// MainWindow on every successful load/save. setSettingsOverride() points
// the storage at a scratch ini for verification.
class NewProjectDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewProjectDialog(QWidget *parent = nullptr);

    // How the dialog was closed (valid after exec() == Accepted).
    enum class Mode { Cancelled, Created, OpenExisting };
    Mode mode() const { return m_mode; }

    // Created-project results.
    QString projectFilePath() const { return m_createdFile; }
    QString projectName() const;
    int fps() const;
    int canvasWidth() const;
    int canvasHeight() const;

    // OpenExisting result: the chosen .sankotv path.
    QString openPath() const { return m_openPath; }

    // --- recent-projects persistence (shared with MainWindow) ------------
    struct RecentEntry
    {
        QString path;         // absolute .sankotv path
        QDateTime lastOpened;
    };
    static QVector<RecentEntry> recentProjects();
    static void recordRecentProject(const QString &path);
    static void removeRecentProject(const QString &path);
    // Verification only: route storage to a scratch ini (empty = real).
    static void setSettingsOverride(const QString &iniPath);

    // Exposed for the verification seam.
    brushlib::StudioTextField *nameField() const { return m_name; }
    brushlib::StudioTextField *locationField() const { return m_location; }
    brushlib::StudioTextField *widthField() const { return m_width; }
    brushlib::StudioTextField *heightField() const { return m_height; }
    brushlib::StudioDropdown *presetDropdown() const { return m_preset; }
    brushlib::StudioDropdown *fpsDropdown() const { return m_fps; }
    // Drag-by-header (behaviour only): the band above the first field,
    // spanning both columns' painted headers. Exposed for tests.
    QRect dragHeaderBand() const;

    // Dev Recorder opt-in (see devrecorder/DevRecorder.h): the form's
    // current values, so a recording carries what the dialog held rather
    // than only that it was open. Invoked by name — no dependency either way.
    Q_INVOKABLE QVariantMap devrecState() const;
    QPushButton *createButton() const { return m_create; }
    QPushButton *openButton() const { return m_open; }
    QString validationReason() const { return m_reason; }
    void attemptCreate(); // the Create click path (re-validates, writes)

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    class RecentList;
    void applyPreset(int index);
    void revalidate();       // live: sets m_reason + Create enabled state
    QString validate() const; // empty = valid
    void browse();
    void openSelectedOrDialog();

    brushlib::StudioTextField *m_name = nullptr;
    brushlib::StudioTextField *m_location = nullptr;
    brushlib::StudioTextField *m_width = nullptr;
    brushlib::StudioTextField *m_height = nullptr;
    brushlib::StudioDropdown *m_preset = nullptr;
    brushlib::StudioDropdown *m_fps = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_create = nullptr;
    QPushButton *m_open = nullptr;
    QScrollArea *m_recentScroll = nullptr;
    RecentList *m_recent = nullptr;

    Mode m_mode = Mode::Cancelled;
    QString m_createdFile;
    QString m_openPath;
    QString m_reason;
    HeaderDrag m_drag;
};
