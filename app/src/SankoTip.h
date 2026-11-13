#pragma once

#include <QProxyStyle>
#include <QString>
#include <QWidget>

class QLabel;
class QPropertyAnimation;

// ONE TOOLTIP LOOK (batch C, the user's rule of 2026-11-13; Figma C-1): every tooltip in
// SankoTV is the floating Brush toolbar's - #1a1a1a fill, a 1 px #2a2a2a border, 6 px
// corners drawn smooth, #cccccc text at 11 px in the app's font, 10 x 6 px of padding. It
// appears 0.35 s after the pointer stops, fades in over 0.12 s, and a hovered one leaves
// 0.12 s after the pointer leaves its widget.
//
// SankoTipFrame is that look and nothing else. The Brush bar's popup (StoryboardPage's
// SankoTipPopup, placed below the bar) is one; the window that stands in for Qt's own
// tooltip everywhere else (installSankoTips) is another.
class SankoTipFrame : public QWidget
{
public:
    explicit SankoTipFrame(QWidget *parent = nullptr);

    // The text in the label: one line, unless wider than kMaxWidth - then wrapped at it.
    void setTipText(const QString &text, Qt::TextFormat format = Qt::AutoText);
    QString tipText() const;
    // Shown fading in from transparent over kFadeMs; already shown, it stays opaque.
    void showFaded();

    static constexpr int kShowDelayMs = 350; // the Brush bar's hover delay
    static constexpr int kHideDelayMs = 120; // ...and its grace after the pointer leaves
    static constexpr int kFadeMs = 120;
    static constexpr int kMaxWidth = 480;    // a longer line wraps at this width

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QLabel *m_label = nullptr;
    QPropertyAnimation *m_fade = nullptr;
};

// THE ONE PATH FOR EVERY OTHER TOOLTIP. Qt still decides when a tooltip is due, what it
// says and where it goes - a widget's tooltip, a menu entry's, every QToolTip::showText -
// so every caller, and every check that reads QToolTip, is unchanged. What changes is
// what is drawn: Qt's own label (a QTipLabel) is made fully transparent, and one
// SankoTipFrame is drawn where it is, with its text. Installed once, from the MainWindow
// constructor (idempotent):
//  - SankoTipStyle as the application's style: the hover delay is 350 ms
//    (SH_ToolTip_WakeUpDelay), and Qt's label is made at opacity 0
//    (SH_ToolTipLabel_Opacity);
//  - an application event filter that mirrors the label (shown, moved, re-texted,
//    hidden) onto the frame; turns Qt's roll and fade off as each label is made (they
//    would draw the label's own pixels); and hides a HOVERED tip 0.12 s after the pointer
//    leaves the widget it came from. A note shown with showText keeps Qt's timing.
void installSankoTips();

// A hovered tooltip's end - the Brush bar's 0.12 s grace, then gone - for a widget that
// tells one hovered item from the next itself (the timeline's tracks: one widget, many
// keys and scenes), where no Leave comes. QToolTip::hideText() would wait Qt's 0.3 s.
// A new tooltip in the grace cancels it.
void sankoHideTipSoon();

// For the gate: the frame standing in for Qt's tooltip, and the Qt label it mirrors
// (null before the first tooltip).
SankoTipFrame *sankoTipFrameForTest();
QWidget *sankoQtTipLabelForTest();
// True: Qt's tooltip as it was before batch C - its own label drawn, nothing mirrored,
// Qt's delay. The "before" a check compares with. Off by default.
void setSankoTipsBypassedForTest(bool bypass);

class SankoTipStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override;
};
