#pragma once

#include "FloatingToolWindow.h"

#include "BrushLibraryModel.h"
#include "BrushPreviewRenderer.h"

#include <QPointer>

class QLabel;
class QMenu;
class QScrollArea;
class QVBoxLayout;

namespace brushlib {

class CategoryRow;
class BrushRow;

// The Brush Library panel (Figma 245:23): an UNMANAGED FloatingToolWindow —
// like SizeCtlBar it takes no part in managed snapping/collision for itself
// and is automatically an obstacle for the three managed toolbars; its drag
// and restore clamps derive from the base's marginRect(), so the 4px window
// margin holds for free. Resizable in THIS subclass only (right/bottom/
// corner edge drag, min 360x420) so the managed toolbars cannot regress.
// Geometry and visibility persist under storyboard/brushLibrary/v1/*.
class BrushLibraryPanel : public FloatingToolWindow
{
    Q_OBJECT
public:
    BrushLibraryPanel(BrushLibraryModel *model, QWidget *anchor,
                      QWidget *parent);

    // First show: default height min(824, marginRect().height()). On a
    // window too small for the minimum (e.g. 1280x720), the base clamp keeps
    // the top/left on-canvas and the panel overflows bottom/right — it is
    // never hidden.
    void openAtDefault();
    void toggleOpen();

signals:
    // A brush was selected: the canvas applies preset.brush wholesale.
    void brushActivated(const QString &presetId);
    // Double-click: the Phase 4 Brush Settings studio (unwired until then).
    void brushSettingsRequested(const QString &presetId);
    void visibilityChanged(bool visible); // View-menu checkbox sync

protected:
    void paintEvent(QPaintEvent *event) override; // panel chrome
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class Edge { None, Right, Bottom, Corner };
    Edge edgeAt(const QPoint &pos) const;
    void buildUi();
    void selectCategory(const QString &category);
    void rebuildBrushList();
    void showRowMenu(BrushRow *row, const QPoint &globalPos);
    void showChevronMenu(const QPoint &globalPos);
    void importBrushes();
    void saveGeometry() const;
    bool restoreGeometry();

    BrushLibraryModel *m_model;
    BrushPreviewRenderer m_previews;
    QString m_category = QStringLiteral("Recent");
    QString m_selectedId;

    QLabel *m_titleLabel = nullptr;
    QWidget *m_headerWidget = nullptr;
    QWidget *m_sidebarWidget = nullptr;
    QVBoxLayout *m_sidebarLayout = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_listWidget = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QVector<CategoryRow *> m_categoryRows;
    QVector<BrushRow *> m_brushRows;

    // Subclass-only move/resize state.
    bool m_pressed = false;
    bool m_dragging = false;
    Edge m_resizing = Edge::None;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartPos;
    QSize m_dragStartSize;
    QPoint m_anchorOffset; // last placed offset; served as defaultOffset()
    bool m_restored = false;
};

} // namespace brushlib
