#pragma once

#include "RecentProjects.h"

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class RecentThumbnails;

// The start window's recent projects: the three most recent as CARDS, the
// rest of the list (up to RecentProjects::kCap) as compact ROWS beneath, in
// three columns aligned to the cards.
//
// The cards replace three hardcoded placeholders; the rows are the New
// Project dialog's Recent Projects list, moved here when that dialog became
// a form for creating a project and nothing else. Everything that list did
// came with it: the first-panel thumbnail, the "Last opened" date, the
// MIDDLE-elided name (version-suffixed families keep their distinguishing
// tail), the missing project shown dimmed with an offer to remove it, and
// the decode-once thumbnail cache.
//
// One painted widget rather than a widget per item: hover, the keyboard
// cursor and the thumbnails repaint together, and the gate can ask it what
// it is showing (the *At accessors return what paintEvent draws, computed
// by the same code).
//
// A project opens on a single click (or Enter on the keyboard cursor). The
// dialog's select-then-Open made sense beside a form with its own primary
// button; on a page whose whole purpose is choosing a project it is a click
// with nothing to say.
class RecentProjectsView : public QWidget
{
    Q_OBJECT

public:
    // Card geometry is the placeholder cards', unchanged: 320 wide, a 16:9
    // thumbnail inside 16 px of padding.
    static constexpr int kCardSlots = 3;
    static constexpr int kCardW = 320;
    static constexpr int kCardGap = 24;
    static constexpr int kCardPad = 16;
    static constexpr int kThumbW = kCardW - 2 * kCardPad;  // 288
    static constexpr int kThumbH = kThumbW * 9 / 16;       // 162
    static constexpr int kCardH = kCardPad + kThumbH + 12 + 20 + 6 + 16
        + kCardPad;                                        // 248
    // Row geometry is the dialog's list, unchanged but for the width.
    static constexpr int kRowH = 44;
    static constexpr int kRowPitch = 50;
    static constexpr int kRowThumbW = 48;
    static constexpr int kRowThumbH = 32;
    static constexpr int kSectionGap = 28; // cards -> rows
    static constexpr int kViewW =
        kCardSlots * kCardW + (kCardSlots - 1) * kCardGap; // 1008

    explicit RecentProjectsView(RecentThumbnails *thumbnails,
                                QWidget *parent = nullptr);

    // Re-read the store, re-check which files exist, and ask for any
    // thumbnail that is missing or out of date.
    void reload();

    int count() const { return int(m_items.size()); }
    bool isEmpty() const { return m_items.isEmpty(); }

    // --- what is on screen, for the gate ---------------------------------
    enum class Kind { Card, Row };
    Kind kindAt(int index) const;
    QRect rectAt(int index) const;
    QRect thumbRectAt(int index) const;
    QString pathAt(int index) const;
    QString shownNameAt(int index) const; // elided exactly as painted
    QString shownDateAt(int index) const;
    bool missingAt(int index) const;
    bool hasThumbnailAt(int index) const;
    // Is the "no picture" film glyph drawn in this item's well? Only for a
    // project known to have none - not for one still waiting to be decoded.
    bool showsGlyphAt(int index) const;
    // The device-pixel size a thumbnail is requested and decoded at.
    QSize thumbPixelSize(Kind kind) const;

    // THE click path: opens the project, or - when its file is gone - asks
    // whether to drop it from the list.
    void activate(int index);
    // Replaces the "remove it?" modal (verification cannot click one).
    // Returns true to remove. Empty restores the modal.
    void setRemovePrompt(std::function<bool(const QString &path)> prompt);

signals:
    void openRequested(const QString &path);
    // The list was re-read; the page shows or hides its empty state on it.
    void reloaded();

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    struct Item
    {
        RecentProjects::Entry entry;
        bool missing = false;
    };
    int indexAt(const QPoint &pos) const;
    void paintCard(QPainter &p, int index) const;
    void paintRow(QPainter &p, int index) const;
    void paintThumb(QPainter &p, int index, const QRectF &well,
                    qreal radius, qreal glyph) const;
    QRect nameRectAt(int index) const;

    RecentThumbnails *m_thumbnails;
    QVector<Item> m_items;
    std::function<bool(const QString &)> m_removePrompt;
    int m_hover = -1;
    int m_pressed = -1;
    int m_current = -1; // the keyboard cursor
};
