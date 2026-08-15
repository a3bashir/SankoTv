#pragma once

#include "FloatingToolWindow.h"

#include "BrushLibraryModel.h"
#include "StudioControls.h"

#include <QUndoStack>

#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace brushlib {

class ScratchCanvas;
class StudioSectionRow;

// The Brush Settings studio (Figma 274:23): a COMPLETE editor for every
// parameter of a brush preset, opened by double-clicking a library row.
//
// Hosting: an unmanaged FloatingToolWindow exactly like the library panel —
// subclass-only resize (right/bottom/corner), the base's margined clamp,
// geometry persisted under storyboard/brushStudio/v1/*. Default size
// min(1376x1032, marginRect), centred; smaller windows keep the designed
// column widths (sidebar 292, properties 384) and let the canvas column
// absorb the loss while the sidebar/properties lists scroll — proportions
// are never scaled.
//
// SESSION MODEL: opening copies the preset's brush into m_session; every
// control edits that copy and the scratch canvas re-renders with it live.
//   Done          -> model->updateBrush() (built-ins become override files)
//   Save Variation-> a NEW user preset; the original is untouched
//   Cancel/close  -> the session is discarded
// Edits here are LIBRARY state, never document state: nothing touches the
// app QUndoStack, composite caches, Layer::image, or the document dirty
// flag. The studio has its OWN QUndoStack (full-session byte snapshots per
// committed gesture); the page routes Ctrl+Z here while the cursor is over
// the studio.
//
// A/B scope: with the dual brush enabled, the A|B switch in the properties
// header retargets every section at the secondary brush. The Dual Brush
// section itself is A-only (it hosts the enable/blend/master controls and
// the switch), matching the codec: the secondary payload is one level deep.
class BrushSettingsStudio : public FloatingToolWindow
{
    Q_OBJECT
public:
    BrushSettingsStudio(BrushLibraryModel *model, QWidget *anchor,
                        QWidget *parent);

    void openForPreset(const QString &presetId);
    QString sessionPresetId() const { return m_presetId; }
    bool sessionDirty() const;
    QUndoStack *undoStack() { return &m_undo; }
    ScratchCanvas *scratchCanvas() { return m_scratch; }

    // App-level stroke stabilization (the Smoothing section, option (c)):
    // QSettings-backed, applies to every brush, never part of a preset.
    static double storedStabilization();

signals:
    void visibilityChanged(bool visible);
    void presetCommitted(const QString &presetId);  // Done
    void variationSaved(const QString &newPresetId);
    void stabilizationChanged(double amount); // 0..1, live

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void reposition() override; // window-client clamp, not the canvas clamp
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class Edge { None, Right, Bottom, Corner };
    static constexpr int kMinW = 980;
    static constexpr int kMinH = 560;
    static constexpr int kSidebarW = 292;
    static constexpr int kPropsW = 384;

    using CurveAccess = std::function<PressureCurve &(::Brush &)>;

    Edge edgeAt(const QPoint &pos) const;
    void buildUi();
    QWidget *sectionPage(QVBoxLayout **outLayout) const;
    void addSection(const QString &name, QWidget *page);
    void selectSection(int index);

    // Section builders (each returns its page widget).
    QWidget *buildStrokeSection();
    QWidget *buildSmoothingSection();
    QWidget *buildTipSection();
    QWidget *buildTextureSection();
    QWidget *buildDualBrushSection();
    QWidget *buildMixingSection();
    QWidget *buildColorSection();
    QWidget *buildDynamicsSection();
    QWidget *buildGeneralSection();
    QWidget *buildPreviewSection();

    // Row factories: create the control, wire the edit path, register a
    // syncer that re-reads the scope brush (selection, undo, scope switch).
    StudioSlider *addSlider(QVBoxLayout *layout, const QString &label,
                            double min, double max,
                            std::function<double(const ::Brush &)> get,
                            std::function<void(::Brush &, double)> set,
                            std::function<QString(double)> fmt,
                            bool tipInvalidating, double step = 0.0,
                            double exponent = 1.0,
                            const CurveAccess &curve = nullptr,
                            bool curveInvalidatesTip = false,
                            std::optional<::Brush::DynamicProperty> property =
                                std::nullopt,
                            std::function<bool()> extraEnable = nullptr);
    StudioCurveRow *addCurveRow(QVBoxLayout *layout, const QString &label,
                                const CurveAccess &curve,
                                bool tipInvalidating = false,
                                std::optional<::Brush::DynamicProperty>
                                    property = std::nullopt,
                                std::function<bool()> extraEnable = nullptr);
    StudioCurveEditor *attachCurveEditor(QVBoxLayout *layout,
                                         const CurveAccess &curve,
                                         bool tipInvalidating);
    // The response drawer (dynamics Phases 1-3): Source choice + Minimum
    // slider + the curve editor, stacked in the chip's expansion. Returns
    // the hidden container the owner row's chip toggles. extraEnable ANDs
    // into the None-dimming (the Smudge row is also inert in Paint mode).
    QWidget *attachResponseDrawer(QVBoxLayout *layout,
                                  const CurveAccess &curve,
                                  bool curveInvalidatesTip,
                                  ::Brush::DynamicProperty property,
                                  StudioSlider *ownerSlider,
                                  StudioCurveRow *ownerRow,
                                  std::function<bool()> extraEnable);
    StudioToggleRow *addToggle(QVBoxLayout *layout, const QString &label,
                               std::function<bool(const ::Brush &)> get,
                               std::function<void(::Brush &, bool)> set,
                               bool tipInvalidating);
    QPushButton *makeButton(const QString &text, bool accent) const;
    QLabel *makeCaption(const QString &text) const;

    // Edit path. Sliders/curve drags: beginGesture (captures the session
    // bytes once) -> live edits -> commitGesture (one undo entry).
    // Instantaneous controls: applyInstant (capture, apply, push).
    ::Brush &scopeBrush();
    const ::Brush &scopeBrushConst() const;
    void setScope(bool scopeB);
    void beginGesture();
    void commitGesture();
    void applyInstant(const std::function<void(::Brush &)> &fn,
                      bool tipInvalidating);
    void pushToScratch(bool tipInvalidating);
    void restoreSessionBytes(const QByteArray &bytes); // undo/redo target
    void syncAll();
    void loadSession(const BrushPreset &preset);
    void openAtDefault();
    void saveGeometryState() const;
    QSize savedSize() const;
    void doneClicked();
    void saveVariationClicked();

    friend class BrushEditCommand;

    BrushLibraryModel *m_model;
    ScratchCanvas *m_scratch = nullptr;

    // Session.
    ::Brush m_session;
    ::Brush m_original;
    QString m_presetId;
    QString m_presetName;
    QString m_presetCategory;
    bool m_presetBuiltin = false;
    bool m_scopeB = false;
    QUndoStack m_undo;
    std::optional<QByteArray> m_gestureBefore;

    // UI.
    QVector<StudioSectionRow *> m_sectionRows;
    QVector<QWidget *> m_sectionPages;
    QStringList m_sectionNames;
    int m_dualSectionIndex = -1;
    int m_currentSection = 0;
    QLabel *m_brushNameLabel = nullptr;
    QLabel *m_propsTitle = nullptr;
    StudioSegmentedRow *m_scopeSwitch = nullptr;
    QScrollArea *m_propsScroll = nullptr;
    QStackedWidget *m_pages = nullptr;
    QVBoxLayout *m_sidebarRows = nullptr;
    StudioCurveEditor *m_lastCurveEditor = nullptr; // set by the factories
    QWidget *m_sidebarWidget = nullptr;
    QWidget *m_canvasWidget = nullptr;
    QWidget *m_propsWidget = nullptr;
    QVector<std::function<void()>> m_syncers;
    bool m_syncing = false; // silences control callbacks during syncAll

    // Move/resize state (the library panel's subclass-only pattern).
    bool m_pressed = false;
    bool m_dragging = false;
    Edge m_resizing = Edge::None;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartPos;
    QSize m_dragStartSize;
    QPoint m_anchorOffset;
    bool m_restored = false;
};

} // namespace brushlib
