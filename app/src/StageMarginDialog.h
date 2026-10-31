#pragma once

#include "FramelessDialogDrag.h"

#include <QDialog>
#include <QSize>

#include <functional>

class QPushButton;
namespace brushlib {
class StudioDropdown;
class StudioTextField;
}

// THE STAGE MARGIN's shared rules (Phase 2A, the camera's Phase 2 spec,
// Figma 2A-1 / 2A-3): the presets are a share of the FRAME on each side -
// 10 % of 1920 across and of 1080 down, each side - and a stage heavier per
// layer than a 4K layer (33.2 MB) is named in amber (the user's Q1).
namespace stagemargin {
constexpr int kPresetCount = 5; // None, 10 %, 25 %, 50 %, Custom
constexpr int kCustom = 4;
constexpr int kMaxSide = 8192;  // the canvas's own cap
QString presetLabel(int index);
// The margin preset `index` gives a frame (index 0: none; Custom: invalid).
QSize presetMargin(int index, const QSize &frame);
// The preset a margin is, else Custom (0 x 0 is None).
int presetOf(const QSize &margin, const QSize &frame);
QSize stageSize(const QSize &frame, const QSize &margin);
double layerMegabytes(const QSize &stage); // stage x 4 bytes, MB of 10^6
bool heavy(const QSize &stage);            // larger than a 4K layer
} // namespace stagemargin

// Project Settings > Stage Margin... (Figma 2A-3): the margin PROMPT. Like
// Resize Project it only collects a margin and tells the truth about it,
// live; MainWindow owns the workflow (the memory precheck, the save
// prompt, the confirm, the swap). Growing cannot lose anything and says so
// in the info grey. Shrinking asks `band` what lies in the part that goes:
// nothing drawn there, it says so plainly; art cut, the text is amber and
// names how many panels lose art and how many camera keys frame part of
// that band (the user's Q2).
class StageMarginDialog : public QDialog
{
    Q_OBJECT

public:
    struct Band
    {
        int panelsWithArt = 0; // panels whose art reaches into the part that goes
        int keysInBand = 0;    // camera keys whose frame reaches into it
    };
    using BandProbe = std::function<Band(const QSize &newMargin)>;

    StageMarginDialog(const QSize &frame, const QSize &currentMargin,
                      BandProbe band, QWidget *parent = nullptr);

    QSize chosenMargin() const;
    QString validationReason() const { return m_reason; } // empty = valid
    QString effectText() const { return m_effect; }       // the live "what happens"
    bool effectWarns() const { return m_warn; }           // amber: art is cut
    QString currentText() const;                          // the "Frame ... current stage" line

    brushlib::StudioDropdown *presetDropdown() const { return m_preset; }
    brushlib::StudioTextField *acrossField() const { return m_across; }
    brushlib::StudioTextField *downField() const { return m_down; }
    QPushButton *sameShareButton() const { return m_sameShare; }
    QPushButton *cancelButton() const { return m_cancel; }
    QPushButton *setButton() const { return m_set; }
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
    void syncShare(bool fromAcross);
    void revalidate();
    QString validate() const;

    HeaderDrag m_drag;
    QSize m_frame;
    QSize m_current;
    BandProbe m_band;
    bool m_syncing = false;

    brushlib::StudioDropdown *m_preset = nullptr;
    brushlib::StudioTextField *m_across = nullptr;
    brushlib::StudioTextField *m_down = nullptr;
    QPushButton *m_sameShare = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_set = nullptr;
    QString m_reason;
    QString m_effect;
    bool m_warn = false;
};
