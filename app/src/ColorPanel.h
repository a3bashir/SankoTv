#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class QLabel;
class QStackedLayout;

namespace ColorPanelParts {
class SegmentedTabs;
class WheelPicker;
class ClassicPicker;
class ChannelSlider;
class SwatchPair;
class SwatchStrip;
}

// Procreate-inspired color picker in the SankoTV dark language: a hue ring +
// saturation/value disc (Wheel tab) or a saturation/value square + hue strip
// (Classic tab), with live-gradient H/S/B sliders, current/previous swatches,
// an automatic color history, and a persistent user palette. One HSV state
// drives every control, so the tabs and sliders always agree and switching
// tabs preserves the exact color. Designed as the BODY of a floating panel
// (StoryboardPage::createFloatingPanel supplies the header/drag/close).
class ColorPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ColorPanel(QWidget *parent = nullptr);

    QColor color() const;

public slots:
    // External sync (e.g. panel opened with the brush's current color).
    // Does not emit colorChanged.
    void setColor(const QColor &color);

signals:
    // Live: fires on every change (drags included) so the brush follows.
    void colorChanged(const QColor &color);

private:
    // The single source of truth. fromUser: emit + sync; commit: a chosen
    // color (release/click) that feeds Previous + History.
    void applyHsv(qreal h, qreal s, qreal v, bool fromUser);
    void commitColor();
    void syncControls();
    void loadPalette();
    void savePalette();

    qreal m_h = 0.0; // 0..1 (fraction of the hue circle)
    qreal m_s = 0.0; // 0..1
    qreal m_v = 0.0; // 0..1
    QColor m_previous;   // last committed color before the current one
    QVector<QColor> m_history;
    QVector<QColor> m_palette;

    ColorPanelParts::SegmentedTabs *m_tabs = nullptr;
    QStackedLayout *m_stack = nullptr;
    ColorPanelParts::WheelPicker *m_wheel = nullptr;
    ColorPanelParts::ClassicPicker *m_classic = nullptr;
    ColorPanelParts::ChannelSlider *m_hSlider = nullptr;
    ColorPanelParts::ChannelSlider *m_sSlider = nullptr;
    ColorPanelParts::ChannelSlider *m_bSlider = nullptr;
    ColorPanelParts::SwatchPair *m_swatches = nullptr;
    ColorPanelParts::SwatchStrip *m_historyStrip = nullptr;
    ColorPanelParts::SwatchStrip *m_paletteStrip = nullptr;
};
