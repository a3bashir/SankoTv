#pragma once

#include "FramelessDialogDrag.h"

#include <QColor>
#include <QDialog>
#include <QPoint>
#include <QRect>

#include <functional>

class ColorPanel;
class QPushButton;
class QTimer;
class QToolButton;

// THE SANKOTV COLOUR PICKER (COLOUR-1, Figma 634:702; the user's choice, 2026-11-18: picture 2,
// 634:793, without the History and Palette rows). Every Custom... colour in the app opens it in
// place of the Windows picker (QColorDialog): Scene Colour (the SCENES row's menu and the scene's
// card), Stage Background (View and the canvas's menu), Grid Colour and Gutter Background (the
// canvas's menu), a layer's colour (Change Layer Color), Brush Settings Studio's Identity Colour
// and Background Colour, and Panel Strip Background (STRIP-1).
//
// It is the Colors panel: the Brush Library's container (a #1E1E20 ground, a 1 px #2D2D35 border,
// 10 px corners) and header (69 px, the title alone in Inter Extra Bold 22 - what it sets), the
// panel's own body (Wheel / Classic, the wheel, H / S / B, current | previous) with its
// Eyedropper beside the wheel, then a divider and Cancel / OK in SankoTV's buttons. It opens on
// the colour in use - "previous" keeps it while "current" shows the new one - beside the point
// that opened it, never over it. OK or Enter sets the colour; Cancel or Esc leaves everything as
// it was. Nothing changes while it is open: the caller sets the colour it returns (one undo step
// where that setting has undo).
//
// THE EYEDROPPER takes one colour from anywhere on screen - the canvas included - as the Windows
// picker's "Pick Screen Color" does: while it waits (the button in SankoTV's active purple),
// "current" shows the colour under the pointer as it moves, a click takes it and the picker is
// back; Esc puts back the colour it had. It reads what the screen shows (a tinted stage reads
// tinted).
class SankoColorPicker : public QDialog
{
    Q_OBJECT

public:
    // As QColorDialog::getColor: the colour chosen, or an invalid colour on Cancel / Esc. It opens
    // beside the pointer - where the Custom... that opened it was clicked.
    static QColor getColor(const QColor &initial, QWidget *parent, const QString &title);

    SankoColorPicker(const QColor &initial, const QString &title, QWidget *parent = nullptr);
    ~SankoColorPicker() override;

    QColor color() const;         // "current"
    QColor previousColor() const { return m_previous; } // the colour it opened on
    void setColor(const QColor &color);
    QString title() const { return m_title; }
    // Right of `anchor` (global) where the screen has room, else left of it; level with it, kept
    // inside the screen's free area; never over it.
    void placeBeside(const QRect &anchor);

    // The Eyedropper.
    void startScreenPick();
    bool screenPickActive() const { return m_picking; }
    // What the screen shows at a global point: the pick's sample.
    static QColor screenColorAt(const QPoint &global);

    // Tests: called with the picker shown, in place of exec(); the hook ends it with the
    // picker's own OK / Cancel / Esc, and getColor returns what exec() would have.
    static void setOpenHookForTest(std::function<void(SankoColorPicker *)> hook);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

public slots:
    void done(int result) override; // a pick still waiting ends first

private:
    void followPointer(const QPoint &global);
    void endScreenPick(bool keep);

    QString m_title;
    QColor m_previous;
    bool m_changed = false; // a colour was chosen (else color() is exactly the one it opened on)
    ColorPanel *m_body = nullptr;
    QToolButton *m_pickButton = nullptr;
    QPushButton *m_ok = nullptr;
    QPushButton *m_cancel = nullptr;
    HeaderDrag m_drag;
    bool m_picking = false;
    QColor m_beforePick;
    bool m_changedBeforePick = false;
    QPoint m_lastPointer{-100000, -100000};
    QTimer *m_pickTimer = nullptr;
    // Under the pointer while a pick waits: a click over another program's window lands on it,
    // not on that program (as the Windows picker's own catcher).
    QWidget *m_pickCatcher = nullptr;
};
