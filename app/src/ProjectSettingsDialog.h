#pragma once

#include "FramelessDialogDrag.h"

#include <QDialog>
#include <QSize>
#include <QString>
#include <QVariantMap>

class QPushButton;
namespace brushlib {
class StudioDropdown;
class StudioTextField;
}

// File > Project Settings... — settings that belong to the CURRENTLY OPEN
// project (Edit > Preferences is application-wide; this is not that).
//
// Two sections. General: Project Name and Frame Rate, both PENDING while
// the dialog is open — Cancel discards, Apply commits and keeps the dialog
// open, OK commits and closes; nothing is mutated before Apply/OK, and the
// dialog itself owns no project state: it emits applied(name, fps) and the
// window applies it. Canvas: informational only in this part — the current
// resolution and aspect ratio, and a Resize Project... button that is
// visible but DISABLED (resizing is a separate workflow; a control that
// looks functional but does nothing is not shipped here).
//
// Built from the shared studio components (StudioTextField, StudioDropdown
// — the New Project window's controls), painted with the same chrome and
// SankoTheme tokens. No QComboBox, no unstyled QLineEdit.
class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    ProjectSettingsDialog(const QString &projectName, int fps,
                          const QSize &canvasSize, QWidget *parent = nullptr);

    // The PENDING values (what the fields show right now).
    QString projectName() const;
    int fps() const;
    QString validationReason() const { return m_reason; } // empty = valid

    // Informational strings the Canvas section paints — exposed so a test
    // can assert the same text the user reads.
    static QString resolutionText(const QSize &size);
    static QString aspectRatioText(const QSize &size);

    // Controls, for tests (same accessor pattern as NewProjectDialog).
    brushlib::StudioTextField *nameField() const { return m_name; }
    brushlib::StudioDropdown *fpsDropdown() const { return m_fps; }
    QPushButton *cancelButton() const { return m_cancel; }
    QPushButton *applyButton() const { return m_apply; }
    QPushButton *okButton() const { return m_ok; }
    QPushButton *resizeButton() const { return m_resize; }

    // Drag-by-header (behaviour only, no chrome): the band above the first
    // field — the painted "General" header and its rule. Exposed so a test
    // can press exactly inside / outside it.
    QRect dragHeaderBand() const;

    // Dev Recorder opt-in (see devrecorder/DevRecorder.h): what this dialog
    // was SEEDED with and what it would PAINT, so a recording of a "the
    // values look wrong/blank" report carries the answer instead of only
    // showing that the dialog opened. Invoked by name, so the recorder
    // needs no knowledge of this type and this class needs no dependency
    // on the recorder.
    Q_INVOKABLE QVariantMap devrecState() const;

signals:
    // Emitted by Apply and by OK (once each), never by Cancel and never by
    // editing a field.
    void applied(const QString &projectName, int fps);
    // "Resize Project..." was pressed. The window owns the workflow: the
    // size prompt, the memory precheck, the save prompt and the confirm.
    void resizeRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QString validate() const;
    void revalidate();
    bool applyPending(); // false if invalid (nothing emitted)

    HeaderDrag m_drag;
    QSize m_canvasSize;
    QVector<int> m_fpsValues; // dropdown index -> fps value
    brushlib::StudioTextField *m_name = nullptr;
    brushlib::StudioDropdown *m_fps = nullptr;
    QPushButton *m_resize = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_apply = nullptr;
    QPushButton *m_ok = nullptr;
    QString m_reason;
};
