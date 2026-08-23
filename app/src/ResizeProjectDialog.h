#pragma once

#include "FramelessDialogDrag.h"

#include <QDialog>
#include <QSize>
#include <QVector>

class QPushButton;
namespace brushlib {
class StudioDropdown;
class StudioTextField;
}

// Resize Project — the size prompt for a CANVAS-ONLY resize (V1). Artwork
// keeps its pixel size and is re-anchored at the CENTRE of the new canvas:
// nothing is ever scaled or resampled here, so growing adds margin and
// shrinking crops.
//
// The dialog only COLLECTS a size. It performs no resize, prompts no save,
// and warns about nothing irreversible: MainWindow owns that workflow (the
// memory precheck, the save prompt, the confirm, the swap), because the
// operation reaches far past this window — it clears the undo stack and the
// panel clipboard. What the dialog does own is telling the truth about what
// the chosen size WOULD do, live, while it is being chosen: whether it
// grows or crops, and by how much.
//
// Built from the same studio controls and painted chrome as New Project and
// Project Settings.
class ResizeProjectDialog : public QDialog
{
    Q_OBJECT

public:
    ResizeProjectDialog(const QSize &currentSize, QWidget *parent = nullptr);

    QSize chosenSize() const;          // valid only after exec() == Accepted
    QString validationReason() const { return m_reason; } // empty = valid

    // Exposed for the gate, in the accessor idiom the other dialogs use.
    brushlib::StudioTextField *widthField() const { return m_width; }
    brushlib::StudioTextField *heightField() const { return m_height; }
    brushlib::StudioDropdown *presetDropdown() const { return m_preset; }
    QPushButton *lockAspectButton() const { return m_lockAspect; }
    QPushButton *cancelButton() const { return m_cancel; }
    QPushButton *resizeButton() const { return m_resize; }
    QString effectText() const { return m_effect; } // the live "what happens"
    QRect dragHeaderBand() const;

    Q_INVOKABLE QVariantMap devrecState() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void applyPreset(int index);
    void revalidate();          // reason + effect text + button state
    QString validate() const;
    void syncAspect(bool fromWidth);

    HeaderDrag m_drag;
    QSize m_current;
    bool m_syncing = false; // re-entry guard while mirroring the locked ratio

    brushlib::StudioTextField *m_width = nullptr;
    brushlib::StudioTextField *m_height = nullptr;
    brushlib::StudioDropdown *m_preset = nullptr;
    QPushButton *m_lockAspect = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_resize = nullptr;
    QVector<QSize> m_presetSizes; // index -> size; (0,0) = Custom
    QString m_reason;
    QString m_effect;
};
