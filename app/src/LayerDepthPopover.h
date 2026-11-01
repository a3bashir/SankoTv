#pragma once

// THE DEPTH POPOVER (Phase 2B, 2026-11-01; Figma 2B-2, the user's Q1-Q6).
// Opened from a layer's depth badge or from Depth... in its row menu, under
// the row: a slider 0 - 200 % with five named stops it snaps to within 3 %
// (Fixed to camera 0, Sky 10, Far 50, Picture plane 100, Near 150), the
// value field for an exact %, and one line saying what the depth does when
// the camera moves. It only reports: onLive while the slider moves (the
// workspace shows it at once), onCommit when the slider is let go or the
// field is entered (the workspace makes it ONE undo step, "Layer Depth").
// Escape or a click outside closes it (a Qt::Popup).

#include <QColor>
#include <QFrame>
#include <QString>

#include <functional>

class QLabel;
class QLineEdit;

namespace layerdepth {
// The words the Layers panel and the popover use, one place.
QString percentText(double parallax);  // "50 %"
QString badgeTooltip(double parallax); // the badge's hover
QString readout(double parallax);      // the popover's line
QString memberTooltip(const QString &folderName);
QString paperTooltip();
// A slider value snapped to a named stop within 3 % (the field never is).
int snapPercent(int percent);
} // namespace layerdepth

class LayerDepthSlider;

class LayerDepthPopover : public QFrame
{
    Q_OBJECT

public:
    LayerDepthPopover(const QString &layerName, double parallax, QWidget *parent = nullptr);

    std::function<void(double)> onLive;   // the slider moved: show it
    std::function<void(double)> onCommit; // let go / Enter: one undo step
    std::function<void()> onClosed;       // a real close (never the destructor)

    // Opens under `anchor` (global px), kept on its screen.
    void popupBelow(const QPoint &anchor);

    // Test hooks: the same paths the pointer and the keyboard take.
    int percentForTest() const;
    QString fieldTextForTest() const;
    QString readoutForTest() const;
    QString activeStopForTest() const; // the stop label lit, or empty
    QColor fieldSelectionForTest() const; // the value field's selected-text background
    void slideForTest(int percent, bool release);
    void typeForTest(const QString &text); // then Enter

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void showValue(int percent);
    void applyField();

    LayerDepthSlider *m_slider = nullptr;
    QLineEdit *m_field = nullptr;
    QLabel *m_readout = nullptr;
};
