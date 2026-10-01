#pragma once

#include "FramelessDialogDrag.h"

#include <QDateTime>
#include <QVariantMap>
#include <QDialog>
#include <QVector>

class QPushButton;
namespace brushlib {
class StudioDropdown;
class StudioTextField;
}

// The New Project window: a form for creating and configuring a project,
// and nothing else. 340x385, one column.
//
// It was built to Figma 350:24 ("new-project-dialog", 680x460) as two
// columns - this form on the left, a Recent Projects list with an Open
// Project button on the right. The right column moved to the start window
// (RecentProjectsView), where recent projects are what the page is FOR, and
// the form kept the design's controls, fonts, row pitch and header. What
// changed with the width: the fields grew from 229 to 304 px (Save Location
// from 158 to 233 - it clipped the default path), the height dropped by the
// band that only existed to match the list, and a Cancel button took the
// footer space Open Project left - the window could previously be dismissed
// only with Escape.
//
// Frameless application-modal QDialog with painted chrome. Hosted modally
// (exec) so Enter/Escape and focus recovery come from Qt — unlike the Brush
// Settings studio's unmanaged FloatingToolWindow, which exists for
// canvas-level reasons (toolbar suppression) this window does not have.
//
// Create writes <SaveLocation>/<Name>/<Name>.sankotv IMMEDIATELY — a folder
// per project, because saving scatters sibling PNGs (panel flattens, layer
// images, consistency thumbnails) and containing them is the point. This
// diverges from File > Save As, which still writes wherever it is pointed.
// The created project is recorded in the shared store (RecentProjects.h).
class NewProjectDialog : public QDialog
{
    Q_OBJECT
public:
    static constexpr int kWidth = 340;
    static constexpr int kHeight = 385;

    explicit NewProjectDialog(QWidget *parent = nullptr);

    // How the dialog was closed (Created only after exec() == Accepted).
    enum class Mode { Cancelled, Created };
    Mode mode() const { return m_mode; }

    // Created-project results.
    QString projectFilePath() const { return m_createdFile; }
    QString projectName() const;
    int fps() const;
    int canvasWidth() const;
    int canvasHeight() const;

    // Exposed for the verification seam.
    brushlib::StudioTextField *nameField() const { return m_name; }
    brushlib::StudioTextField *locationField() const { return m_location; }
    brushlib::StudioTextField *widthField() const { return m_width; }
    brushlib::StudioTextField *heightField() const { return m_height; }
    brushlib::StudioDropdown *presetDropdown() const { return m_preset; }
    brushlib::StudioDropdown *fpsDropdown() const { return m_fps; }
    // Drag-by-header (behaviour only): the band above the first field,
    // across the painted header. Exposed for tests.
    QRect dragHeaderBand() const;

    // Dev Recorder opt-in (see devrecorder/DevRecorder.h): the form's
    // current values, so a recording carries what the dialog held rather
    // than only that it was open. Invoked by name — no dependency either way.
    Q_INVOKABLE QVariantMap devrecState() const;
    QPushButton *createButton() const { return m_create; }
    QPushButton *cancelButton() const { return m_cancel; }
    QString validationReason() const { return m_reason; }
    void attemptCreate(); // the Create click path (re-validates, writes)

    // Verification only: the folder a new dialog offers as its Save
    // Location (empty = the real one, "SankoTV" under Documents). Merely
    // CONSTRUCTING this dialog creates that folder and probes it for
    // writability, so a gate that opens the dialog must point it at scratch
    // first or it writes into the user's Documents.
    static void setDefaultLocationForTest(const QString &dir);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void applyPreset(int index);
    void revalidate();       // live: sets m_reason + Create enabled state
    QString validate() const; // empty = valid
    void browse();

    brushlib::StudioTextField *m_name = nullptr;
    brushlib::StudioTextField *m_location = nullptr;
    brushlib::StudioTextField *m_width = nullptr;
    brushlib::StudioTextField *m_height = nullptr;
    brushlib::StudioDropdown *m_preset = nullptr;
    brushlib::StudioDropdown *m_fps = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_create = nullptr;
    QPushButton *m_cancel = nullptr;

    Mode m_mode = Mode::Cancelled;
    QString m_createdFile;
    QString m_reason;
    HeaderDrag m_drag;
};
