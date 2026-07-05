#pragma once

#include <QString>
#include <QWidget>

// Custom-painted slider with a flat purple fill, horizontal or vertical,
// configurable track/handle sizes. The live value label is painted inside
// the widget (right of a horizontal track, below a vertical one), purple
// monospace. Drop-in for the QSlider API surface the app uses:
// value()/setValue()/setRange()/valueChanged(int)/setEnabled().
class SankoSlider : public QWidget
{
    Q_OBJECT

public:
    explicit SankoSlider(QWidget *parent = nullptr);

    int value() const { return m_value; }
    void setRange(int min, int max);
    void setOrientation(Qt::Orientation orientation); // default Horizontal
    void setTrackHeight(int px);          // when Vertical this is the track WIDTH
    void setHandleSize(int px);           // widget cross-size = handle + glow bleed
    void setValueSuffix(const QString &suffix); // "%" for opacity, "" for size

public slots:
    void setValue(int value); // clamped to range; emits valueChanged on change

signals:
    void valueChanged(int value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override; // repaint on enable/disable

private:
    int labelExtent() const;     // space reserved for the value text
    qreal trackLength() const;   // slider run available to the track
    int valueFromPos(qreal pos) const; // x (horizontal) or y (vertical)
    qreal handleCenterPos() const;     // along the slider run
    void refreshSizeConstraints();

    Qt::Orientation m_orientation = Qt::Horizontal;
    int m_min = 0;
    int m_max = 100;
    int m_value = 100;
    int m_trackH = 14;
    int m_handle = 16;
    QString m_suffix;
    bool m_hovered = false;
    bool m_dragging = false;
};
