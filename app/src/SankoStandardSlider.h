#pragma once

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QString>
#include <QWidget>

// THE STANDARD SANKO SLIDER (2026-11-27; the user's word after hand test I2-1 and again after
// I2-2: "every new slider in SankoTV uses the standard Sanko slider from the Zoom Toolbar").
// The Zoom Toolbar's Zoom and Rotate controls as one widget, drawn from the same numbers
// (ZoomToolbar.cpp, Figma 86:32; the Perspective bar's sliders, Figma 180:121, are the same
// design):
//   - the label at the top left (Inter Semi Bold, 9 px, #cccccc) and the value at the top
//     right (8 px), its right edge on the track's;
//   - 14 px below the top the track: 10 px high, #333333, radius 2;
//   - the fill from the track's left to the dragger's middle: #4b4397 to the Sanko purple,
//     left to right, under a white sheen (10 % at the top to none at the bottom), radius 1;
//   - the dragger: 20 x 10, #b3b3b3, radius 1, its left at t x (track width - 20).
// A press or a drag on the widget puts the value at x / width of its range, as the toolbar does.
//
// Kind::Hue is the Perspective bar's "Hue Colors" track: the spectrum across the track, no
// fill, the same dragger; its value is a hue, 0 to 359. It may carry a label, and a small
// swatch where the value's text would be (setSwatch): the colour the choice gives.
//
// The toolbar's own two tracks and the Perspective bar's sliders stay as they are painted
// today (their pictures are pinned); every slider made from now on is this widget.
class SankoStandardSlider : public QWidget
{
    Q_OBJECT

public:
    enum class Kind { Value, Hue };

    static constexpr int kTrackTop = 14;   // below the label's line
    static constexpr int kTrackHeight = 10;
    static constexpr int kDraggerWidth = 20;
    static constexpr int kHeight = kTrackTop + kTrackHeight + 1;
    static constexpr int kStandardWidth = 160; // the toolbar's track

    explicit SankoStandardSlider(const QString &label, Kind kind = Kind::Value, QWidget *parent = nullptr);

    Kind kind() const { return m_kind; }
    QString label() const { return m_label; }
    int value() const { return m_value; }
    int minimum() const { return m_min; }
    int maximum() const { return m_max; }
    void setRange(int min, int max);
    // What follows the number in the value's text: "%", " px"...
    void setValueSuffix(const QString &suffix);
    QString valueText() const;
    // (Hue) The colour shown at the top right; an invalid colour shows none.
    void setSwatch(const QColor &color);
    QColor swatch() const { return m_swatch; }

    // Where things are painted, in the widget's own coordinates.
    QRectF trackRect() const;
    QRectF draggerRect() const;
    QRectF fillRect() const; // empty for Kind::Hue, and while the value is at its least
    // The x a press must land on to choose `value` (the middle of its span).
    int xForValue(int value) const;
    bool isDragging() const { return m_dragging; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    void setValue(int value); // kept in the range; valueChanged when it changes

signals:
    void valueChanged(int value);
    void dragStarted();  // the press that begins a drag, before its first change
    void dragFinished(); // the release

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    double fraction() const; // the value along its range, 0 to 1
    void setFromX(double x);

    QString m_label;
    Kind m_kind = Kind::Value;
    int m_min = 0;
    int m_max = 100;
    int m_value = 0;
    QString m_suffix;
    QColor m_swatch;
    bool m_dragging = false;
    QFont m_labelFont;
    QFont m_valueFont;
};
