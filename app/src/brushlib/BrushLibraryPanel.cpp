#include "BrushLibraryPanel.h"

#include "BrushPresetCodec.h"
#include "SankoScrollBarStyle.h"

#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QVBoxLayout>

namespace brushlib {
namespace {

// Figma 245:23 tokens.
const QColor kPanelBg(0x1e, 0x1e, 0x20);
const QColor kPanelBorder(0x2d, 0x2d, 0x35);
const QColor kRowIdle(0x24, 0x24, 0x2b);
// Brush rows carry an always-on card background ~10% lighter than kRowIdle
// (0x24..2b x1.1) so neighbouring rows read as distinct cards against the
// #1e1e20 panel; hover steps ~10% above that so the affordance survives.
// DOCUMENTED FIGMA DIVERGENCE (approved, like the favourite dot and the
// size label): the design draws idle rows with no background at all.
const QColor kRowBg(0x28, 0x28, 0x2f);
const QColor kRowHover(0x2e, 0x2e, 0x36);
const QColor kAccent(0x7c, 0x6e, 0xf6);
const QColor kTextDim(0xcc, 0xcc, 0xcc);
const QColor kFavourite(0xE8, 0xA3, 0x3D); // approved favourite amber
constexpr int kSidebarW = 154;
constexpr int kMinW = 360;
constexpr int kMinH = 420;
constexpr int kEdgeBand = 8; // resize hit band, subclass-only

} // namespace

// --- Category row (Figma rows 245:33..45 geometry: 38h r6 px12 py10) -------
// Label-only since the icon removal: the text starts at x=12 where the icon
// sat, matching the Studio sidebar's iconless rows — same visual language,
// same height and radius.
class CategoryRow : public QWidget
{
public:
    CategoryRow(const QString &name, QWidget *parent)
        : QWidget(parent)
        , m_name(name)
    {
        setFixedHeight(38);
        setCursor(Qt::PointingHandCursor);
    }
    QString name() const { return m_name; }
    void setActive(bool active)
    {
        if (m_active == active)
            return;
        m_active = active;
        update();
    }
    std::function<void()> onClicked;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m_active ? kAccent : kRowIdle);
        p.drawRoundedRect(rect(), 6, 6);
        QFont f = font();
        f.setFamily(QStringLiteral("Inter"));
        f.setPixelSize(12);
        f.setWeight(m_active ? QFont::Bold : QFont::Medium);
        p.setFont(f);
        p.setPen(m_active ? Qt::white : kTextDim);
        const QRect textRect(12, 0, width() - 24, height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(f).elidedText(m_name, Qt::ElideRight,
                                              textRect.width()));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onClicked)
            onClicked();
    }

private:
    QString m_name;
    bool m_active = false;
};

// --- Brush row (Figma rows 245:86..119: r10 px16 py12 gap8; name Bold 13
// white; swatch 26h). Divergences, both approved/reported: an amber
// favourite dot (the design has no favourite affordance) and a right-aligned
// size label (previews normalise size away, so the row states it) ----------
class BrushRow : public QWidget
{
public:
    BrushRow(const BrushPreset *preset, QWidget *parent)
        : QWidget(parent)
        , m_id(preset->id)
        , m_name(preset->name)
        , m_sizePx(preset->brush.size())
        , m_smudge(preset->brush.smudgeActive())
    {
        setFixedHeight(74);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }
    QString presetId() const { return m_id; }
    void setPreset(const BrushPreset *preset)
    {
        m_id = preset->id;
        m_name = preset->name;
        m_sizePx = preset->brush.size();
        m_smudge = preset->brush.smudgeActive();
        m_swatch = QPixmap();
        update();
    }
    void setSelected(bool on)
    {
        if (m_selected != on) {
            m_selected = on;
            update();
        }
    }
    void setFavourite(bool on)
    {
        if (m_favourite != on) {
            m_favourite = on;
            update();
        }
    }
    // Phase 4 dirty-state affordance: the working brush has been edited away
    // from this (selected) preset. White dot after the name — the row's own
    // background IS the accent, so an accent dot would vanish — plus a
    // "Reset" chip that restores the pristine preset (the discoverable form
    // of the re-select gesture).
    void setDirty(bool on)
    {
        if (m_dirty != on) {
            m_dirty = on;
            update();
        }
    }
    void setSwatch(const QImage &image)
    {
        m_swatch = QPixmap::fromImage(image);
        update();
    }
    std::function<void()> onClicked;
    std::function<void()> onDoubleClicked;
    std::function<void()> onResetClicked;
    std::function<void(const QPoint &)> onContextMenu;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m_selected ? kAccent : (m_hover ? kRowHover : kRowBg));
        p.drawRoundedRect(rect(), 10, 10);
        QFont f = font();
        f.setFamily(QStringLiteral("Inter"));
        f.setPixelSize(13);
        f.setWeight(QFont::Bold);
        p.setFont(f);
        p.setPen(Qt::white);
        const int favW = m_favourite ? 14 : 0;
        const int sizeW = 44;
        const int resetW = m_dirty ? kResetChipW + 8 : 0;
        const int nameW = width() - 32 - sizeW - favW - resetW
            - (m_dirty ? 10 : 0);
        const QString elided =
            QFontMetrics(f).elidedText(m_name, Qt::ElideRight, nameW);
        p.drawText(QRect(16, 12, nameW, 16),
                   Qt::AlignVCenter | Qt::AlignLeft, elided);
        if (m_dirty) {
            // White dot right after the name: "you are no longer on stock".
            const int textW = QFontMetrics(f).horizontalAdvance(elided);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(Qt::white));
            p.drawEllipse(QRectF(16 + textW + 6, 17, 6, 6));
            // Reset chip.
            const QRect chip = resetChipRect();
            p.setBrush(QColor(255, 255, 255, m_selected ? 46 : 28));
            p.drawRoundedRect(chip, chip.height() / 2.0,
                              chip.height() / 2.0);
            QFont cf = f;
            cf.setPixelSize(10);
            cf.setWeight(QFont::DemiBold);
            p.setFont(cf);
            p.setPen(Qt::white);
            p.drawText(chip, Qt::AlignCenter, QStringLiteral("Reset"));
            p.setFont(f);
        }
        // Size label: previews render at a normalised size, so the row
        // carries the real one.
        QFont sf = f;
        sf.setPixelSize(11);
        sf.setWeight(QFont::Medium);
        p.setFont(sf);
        p.setPen(m_selected ? QColor(255, 255, 255, 200)
                            : QColor(0x99, 0x99, 0x99));
        p.drawText(QRect(width() - 16 - sizeW - favW, 12, sizeW, 16),
                   Qt::AlignVCenter | Qt::AlignRight,
                   QStringLiteral("%1 px").arg(m_sizePx));
        // Favourite dot: amber reads against BOTH the idle row and the
        // accent selection (distinct hues, verified in the seam).
        if (m_favourite) {
            p.setPen(Qt::NoPen);
            p.setBrush(kFavourite);
            p.drawEllipse(QRectF(width() - 16 - 8, 15, 8, 8));
        }
        // Swatch area (222x26 design; stretches with the row width). NO
        // backing of any kind is drawn: the preview is white ink on
        // TRANSPARENT pixels and composites straight onto the row's own card,
        // so there is no box, border, shadow or dark area behind it.
        // DOCUMENTED FIGMA DIVERGENCE, and a deliberate trade recorded with
        // the others: a dark #16161a bed here did give the white stroke its
        // best contrast — most of all on the SELECTED row, where white now
        // sits on the #7C6EF6 accent instead. The clean look was chosen over
        // that contrast; the numbers are in the task report. Any future
        // attempt to "fix" faint selected-row previews must not reinstate a
        // backing rect. A swatch whose worker render has not arrived yet now
        // shows nothing rather than an empty bed — a sub-second transient.
        // (The smudge brushes' colour-band background is NOT this container:
        // it lives inside the rendered QImage, is required because a smudge
        // over transparency is a no-op, and is untouched here.)
        if (!m_swatch.isNull())
            p.drawPixmap(swatchRect(), m_swatch);
    }
    void enterEvent(QEnterEvent *) override
    {
        m_hover = true;
        update();
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = false;
        update();
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            if (m_dirty && resetChipRect().contains(e->position().toPoint())
                && onResetClicked) {
                onResetClicked();
                return;
            }
            if (onClicked)
                onClicked();
        } else if (e->button() == Qt::RightButton && onContextMenu) {
            onContextMenu(e->globalPosition().toPoint());
        }
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onDoubleClicked)
            onDoubleClicked();
    }

private:
    static constexpr int kResetChipW = 42;
    QRect swatchRect() const { return QRect(16, 36, width() - 32, 26); }
    QRect resetChipRect() const
    {
        const int favW = m_favourite ? 14 : 0;
        return QRect(width() - 16 - 44 - favW - kResetChipW - 8, 11,
                     kResetChipW, 18);
    }

    QString m_id;
    QString m_name;
    int m_sizePx;
    bool m_smudge;
    bool m_selected = false;
    bool m_favourite = false;
    bool m_dirty = false;
    bool m_hover = false;
    QPixmap m_swatch;
};

// --- Panel ------------------------------------------------------------------

BrushLibraryPanel::BrushLibraryPanel(BrushLibraryModel *model,
                                     QWidget *anchor, QWidget *parent)
    : FloatingToolWindow(anchor, QString(), parent)
    , m_model(model)
{
    setMouseTracking(true);
    // Cooperate with the manager's anchor-follow: on window move/resize the
    // base calls reposition(), which re-applies defaultOffset() through the
    // margined clamp. Serving OUR last placed offset makes the panel follow
    // the window and re-clamp into the 4px margin, instead of snapping to
    // the anchor origin.
    setDefaultOffsetProvider([this] { return m_anchorOffset; });
    buildUi();
    connect(&m_previews, &BrushPreviewRenderer::previewReady, this,
            [this](const QString &presetId, const QImage &image) {
                for (BrushRow *row : m_brushRows)
                    if (row->presetId() == presetId)
                        row->setSwatch(image);
            });
    connect(m_model, &BrushLibraryModel::changed, this, [this] {
        updateImportedRow();
        rebuildBrushList();
    });
    updateImportedRow();
    selectCategory(QStringLiteral("Recent")); // MRU is the default view
}

void BrushLibraryPanel::buildUi()
{
    const QString icons = QStringLiteral(":/icons/brushlib/");
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1); // inside the 1px painted border
    root->setSpacing(0);

    // Header (245:24): pad 24/20/24/16, bottom border drawn in paintEvent.
    m_headerWidget = new QWidget(this);
    m_headerWidget->setObjectName(QStringLiteral("brushLibHeader"));
    m_headerWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_headerWidget->setStyleSheet(QStringLiteral(
        "QWidget#brushLibHeader { background: transparent;"
        " border-bottom: 1px solid #2d2d35; }"));
    m_headerWidget->setFixedHeight(69);
    auto *header = new QHBoxLayout(m_headerWidget);
    header->setContentsMargins(24, 20, 24, 16);
    header->setSpacing(8);
    m_titleLabel = new QLabel(m_model->libraryName(), m_headerWidget);
    QFont titleFont(QStringLiteral("Inter"));
    titleFont.setPixelSize(22);
    titleFont.setWeight(QFont::ExtraBold);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setStyleSheet(QStringLiteral("color: white;"));
    header->addWidget(m_titleLabel);
    auto *chevron = new QPushButton(m_headerWidget);
    chevron->setFixedSize(18, 18);
    chevron->setCursor(Qt::PointingHandCursor);
    chevron->setIcon(QIcon(icons + QStringLiteral("chevron-down.svg")));
    chevron->setIconSize(QSize(8, 8));
    chevron->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2a2a33; border: none; "
        "border-radius: 9px; }"
        "QPushButton:hover { background: #34343f; }"));
    connect(chevron, &QPushButton::clicked, this, [this, chevron] {
        showChevronMenu(chevron->mapToGlobal(QPoint(0, chevron->height())));
    });
    header->addWidget(chevron);
    header->addStretch(1);
    auto *importBtn = new QPushButton(QStringLiteral("Import"),
                                      m_headerWidget);
    importBtn->setCursor(Qt::PointingHandCursor);
    QFont importFont(QStringLiteral("Inter"));
    importFont.setPixelSize(14);
    importFont.setWeight(QFont::DemiBold);
    importBtn->setFont(importFont);
    importBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: #7c6ef6; color: #ccc; border: none; "
        "border-radius: 6px; padding: 8px 16px; }"
        "QPushButton:hover { background: #8b7ef8; }"));
    connect(importBtn, &QPushButton::clicked, this,
            [this] { importBrushes(); });
    header->addWidget(importBtn);
    root->addWidget(m_headerWidget);

    // Columns: sidebar (154w, bg #16161a) + scrolling brush list.
    auto *columns = new QHBoxLayout;
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    m_sidebarWidget = new QWidget(this);
    m_sidebarWidget->setObjectName(QStringLiteral("brushLibSidebar"));
    m_sidebarWidget->setFixedWidth(kSidebarW);
    m_sidebarWidget->setAttribute(Qt::WA_StyledBackground, true);
    // The bottom-left radius matters: the frame's rounded corner is painted
    // by paintEvent, but this sidebar is a SQUARE child sitting on top of it
    // all the way down to the panel's bottom edge — without its own radius,
    // its QSS background overpaints the frame's arc and squares the panel's
    // bottom-left corner (the header above is transparent, which is why the
    // top-left never had the problem). 9px = the frame's 10px outer radius
    // minus the 1px border the child sits inside.
    m_sidebarWidget->setStyleSheet(QStringLiteral(
        "QWidget#brushLibSidebar { background-color: #16161a;"
        " border-right: 1px solid #2d2d35;"
        " border-bottom-left-radius: 9px; }"));
    m_sidebarLayout = new QVBoxLayout(m_sidebarWidget);
    m_sidebarLayout->setContentsMargins(16, 16, 12, 16);
    m_sidebarLayout->setSpacing(6);
    // Label-only rows; Recent + the model's five categories (Watercolor is
    // gone — its brushes were redistributed into Painting, ids unchanged),
    // plus "Imported" — the home of foreign-format (.abr) imports — which
    // stays hidden until at least one imported brush exists.
    const QStringList cats = QStringList()
        << QStringLiteral("Recent") << BrushLibraryModel::categories()
        << QStringLiteral("Imported");
    for (const QString &name : cats) {
        auto *row = new CategoryRow(name, m_sidebarWidget);
        row->onClicked = [this, row] { selectCategory(row->name()); };
        m_sidebarLayout->addWidget(row);
        m_categoryRows.append(row);
        if (name == QStringLiteral("Imported"))
            m_importedRow = row;
    }
    m_sidebarLayout->addStretch(1);
    columns->addWidget(m_sidebarWidget);

    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    // The bottom-RIGHT counterpart of the sidebar fix above had a different
    // mechanism: StoryboardPage's page/column stylesheets used BARE
    // background-color declarations, which cascade into every descendant —
    // this scroll area, the panel's one child with no styling of its own,
    // inherited #0a0a0a and painted it over the panel chrome, squaring the
    // frame's bottom-right corner (13 paper px in the corner probe vs 23 at
    // the other three). Those page sheets are now SCOPED by objectName; the
    // scrollbar was ALSO the reference design the app-wide style in
    // SankoScrollBarStyle.h was lifted from. It keeps one site-specific
    // parameter — the 12px bottom margin that clears the panel's rounded
    // arc — through the same shared builder, so a future style change
    // still lands in one place.
    m_scroll->verticalScrollBar()->setStyleSheet(sankoScrollBarStyle(12));
    m_listWidget = new QWidget;
    m_listWidget->setAutoFillBackground(false);
    m_listLayout = new QVBoxLayout(m_listWidget);
    m_listLayout->setContentsMargins(16, 16, 16, 16);
    m_listLayout->setSpacing(12);
    m_listLayout->addStretch(1);
    m_scroll->setWidget(m_listWidget);
    columns->addWidget(m_scroll, 1);
    root->addLayout(columns, 1);
}

void BrushLibraryPanel::selectCategory(const QString &category)
{
    m_category = category;
    for (CategoryRow *row : m_categoryRows)
        row->setActive(row->name() == category);
    m_previews.cancelAll(); // obsolete requests die with the old category
    rebuildBrushList();
}

void BrushLibraryPanel::rebuildBrushList()
{
    const QVector<const BrushPreset *> presets =
        m_category == QStringLiteral("Recent")
        ? m_model->recentPresets()
        : m_model->presetsIn(m_category);
    // Row POOL: rows are reused in place; only the count difference is
    // created or destroyed. Rebuilding 30 rows per category switch cost
    // ~20ms per row of UI-thread widget churn.
    while (m_brushRows.size() > presets.size()) {
        m_brushRows.last()->deleteLater();
        m_brushRows.removeLast();
    }
    while (m_brushRows.size() < presets.size()) {
        auto *row = new BrushRow(presets.at(m_brushRows.size()),
                                 m_listWidget);
        row->onClicked = [this, row] {
            m_selectedId = row->presetId();
            for (BrushRow *r : m_brushRows)
                r->setSelected(r->presetId() == m_selectedId);
            m_model->recordUsage(m_selectedId);
            emit brushActivated(m_selectedId);
        };
        row->onDoubleClicked = [this, row] {
            emit brushSettingsRequested(row->presetId()); // Phase 4 studio
        };
        row->onResetClicked = [this, row] {
            // Reset = the discoverable form of the re-select gesture: the
            // pristine preset replaces the edited working brush.
            emit brushActivated(row->presetId());
        };
        row->onContextMenu = [this, row](const QPoint &globalPos) {
            showRowMenu(row, globalPos);
        };
        m_listLayout->insertWidget(m_brushRows.size(), row);
        row->show();
        m_brushRows.append(row);
    }
    for (int i = 0; i < presets.size(); ++i) {
        const BrushPreset *preset = presets.at(i);
        BrushRow *row = m_brushRows.at(i);
        row->setPreset(preset);
        row->setSelected(preset->id == m_selectedId);
        row->setFavourite(m_model->isFavourite(preset->id));
        row->setDirty(preset->id == m_dirtyId);
        m_previews.requestPreview(preset->id, preset->brush);
    }
}

void BrushLibraryPanel::setActiveDirty(const QString &presetId, bool dirty)
{
    const QString id = dirty ? presetId : QString();
    if (id == m_dirtyId)
        return;
    m_dirtyId = id;
    for (BrushRow *row : m_brushRows)
        row->setDirty(row->presetId() == m_dirtyId);
}

void BrushLibraryPanel::showRowMenu(BrushRow *row, const QPoint &globalPos)
{
    const BrushPreset *preset = m_model->preset(row->presetId());
    if (!preset)
        return;
    const QString id = preset->id;
    QMenu menu(this);
    menu.addAction(tr("Rename Brush"), [this, id] {
        const BrushPreset *p = m_model->preset(id);
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename Brush"), tr("Name:"), QLineEdit::Normal,
            p ? p->name : QString(), &ok);
        if (ok)
            m_model->renamePreset(id, name);
    });
    menu.addAction(tr("Duplicate Brush"),
                   [this, id] { m_model->duplicatePreset(id); });
    QAction *fav = menu.addAction(m_model->isFavourite(id)
                                      ? tr("Unfavorite Brush")
                                      : tr("Favorite Brush"));
    connect(fav, &QAction::triggered, this, [this, id] {
        m_model->setFavourite(id, !m_model->isFavourite(id));
    });
    menu.addAction(tr("Delete Brush"), [this, id] {
        const BrushPreset *p = m_model->preset(id);
        if (!p)
            return;
        if (p->builtin) {
            // Built-in delete = hide; Restore Default Brushes brings the
            // stock roster back. No confirmation: nothing is destroyed.
            m_model->setBuiltinHidden(id, true);
        } else if (QMessageBox::question(
                       this, tr("Delete Brush"),
                       tr("Delete \"%1\" permanently?").arg(p->name))
                   == QMessageBox::Yes) {
            m_model->removeUserPreset(id);
        }
    });
    menu.exec(globalPos);
}

void BrushLibraryPanel::showChevronMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    menu.addAction(tr("Export Brushes..."), [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Brushes"),
            m_model->libraryName() + QStringLiteral(".sankobrushset"),
            tr("SankoTV Brush Set (*.sankobrushset)"));
        if (!path.isEmpty())
            m_model->exportLibrary(path);
    });
    menu.addAction(tr("Rename Brush Library..."), [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename Brush Library"), tr("Name:"),
            QLineEdit::Normal, m_model->libraryName(), &ok);
        if (ok) {
            m_model->setLibraryName(name);
            m_titleLabel->setText(m_model->libraryName());
        }
    });
    menu.addSeparator();
    menu.addAction(tr("Restore Default Brushes"),
                   [this] { m_model->restoreDefaultBrushes(); });
    menu.exec(globalPos);
}

void BrushLibraryPanel::importBrushes()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Brushes"), QString(),
        tr("All Supported Brushes (*.sankobrush *.sankobrushset *.abr);;"
           "SankoTV Brushes (*.sankobrush *.sankobrushset);;"
           "Photoshop Brushes (*.abr)"));
    if (path.isEmpty())
        return;
    QString report;
    BrushLibraryModel::ImportSummary summary;
    m_model->importFile(path, &report, &summary);
    if (!report.isEmpty()) {
        // A lossy importer (.abr) accounts for every brush: mapped,
        // approximated, dropped — and importFile has already appended the
        // per-brush APPLICATION outcomes and a summary tallied from the
        // same records, so this dialog never contradicts itself. Nothing
        // to add here.
        showImportReport(report);
        return;
    }
    // Native imports write no report; word the outcome from the summary
    // counts. "Nothing was importable" and "everything was already
    // present" are opposite outcomes — never conflate them.
    if (!summary.claimed || summary.parsed == 0) {
        QMessageBox::warning(
            this, tr("Import Brushes"),
            tr("Nothing could be imported from this file — it was not "
               "recognised as a brush file, or it contains no readable "
               "brushes."));
    } else if (summary.failed > 0) {
        QMessageBox::warning(
            this, tr("Import Brushes"),
            tr("%1 brush(es) could not be written to the library. Check "
               "disk space and permissions.").arg(summary.failed));
    } else if (summary.added + summary.upgraded == 0) {
        // Identity-aware import (J8) working as designed: a SUCCESS.
        QMessageBox::information(
            this, tr("Import Brushes"),
            tr("Every brush in this file is already in the library — "
               "nothing needed to change."));
    }
}

void BrushLibraryPanel::showImportReport(const QString &report)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Import Report"));
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background-color: #1e1e20; }"
        "QPlainTextEdit { background-color: #16161a; color: #cccccc;"
        " border: 1px solid #2d2d35; border-radius: 6px; padding: 8px; }"
        "QPushButton { background-color: #28282f; color: #e8e8ea;"
        " border: 1px solid #2d2d35; border-radius: 6px;"
        " padding: 6px 18px; }"
        "QPushButton:hover { background-color: #2e2e36; }"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setPlainText(report);
    layout->addWidget(text, 1);
    auto *close = new QPushButton(tr("Close"), &dialog);
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close, 0, Qt::AlignRight);
    dialog.resize(560, 480);
    dialog.exec();
}

void BrushLibraryPanel::updateImportedRow()
{
    if (!m_importedRow)
        return;
    const bool hasImports =
        !m_model->presetsIn(QStringLiteral("Imported")).isEmpty();
    m_importedRow->setVisible(hasImports);
    // If the category empties out from under the user (delete of the last
    // imported brush), fall back to the default view instead of showing an
    // empty list for a row that just vanished.
    if (!hasImports && m_category == QStringLiteral("Imported"))
        selectCategory(QStringLiteral("Recent"));
}

// --- Hosting: default size, persistence, move + subclass-only resize -------

void BrushLibraryPanel::openAtDefault()
{
    if (!m_restored) {
        // SIZE persists; POSITION is derived from the Brush-button anchor
        // every time, so a previously saved position is deliberately
        // ignored (its size component is still honoured).
        const QSettings s(QStringLiteral("SankoTV"),
                          QStringLiteral("SankoTV"));
        const QRect geo =
            s.value(QStringLiteral("storyboard/brushLibrary/v1/geo"))
                .toRect();
        const QRect margin = marginRect();
        int w = 440, h = qMin(824, margin.height());
        if (geo.isValid() && geo.width() >= kMinW && geo.height() >= kMinH) {
            w = geo.width();
            h = geo.height();
        }
        resize(w, h);
        m_restored = true;
    }
    reanchor();
    setVisible(true);
    raise();
}

void BrushLibraryPanel::setAnchorProvider(
    std::function<QPair<QRect, QRect>()> provider)
{
    m_anchorProvider = std::move(provider);
}

void BrushLibraryPanel::reanchor()
{
    const QRect margin = marginRect();
    QRect buttonRect, toolbarRect;
    if (m_anchorProvider) {
        const auto rects = m_anchorProvider();
        buttonRect = rects.first;
        toolbarRect = rects.second;
    }
    if (!buttonRect.isValid() || !toolbarRect.isValid()) {
        // No anchor available (toolbar hidden by collision fallback):
        // the legacy right-centre placement.
        move(clampedPos(QPoint(
            margin.right() + 1 - width(),
            margin.top() + (margin.height() - height()) / 2)));
        m_anchorOffset = pos() - anchorWidget()->mapToGlobal(QPoint(0, 0));
        return;
    }
    // Below the toolbar when it sits in the top half of the canvas, above
    // it when in the bottom half — the same +1 that gives the Modifier
    // toolbars their exact 4px visual gap.
    const bool toolbarOnTop =
        toolbarRect.center().y() < margin.center().y();
    // The panel may never overlap the toolbar, so its height is capped to
    // the space on the chosen side (floored at the resize minimum — below
    // that the margined clamp takes over, as everywhere else).
    const int available = toolbarOnTop
        ? margin.bottom() - (toolbarRect.bottom() + 1 + kMargin) + 1
        : (toolbarRect.top() - kMargin) - margin.top();
    const int h = qMax(kMinH, qMin(height(), available));
    if (h != height())
        resize(width(), h);
    const int y = toolbarOnTop ? toolbarRect.bottom() + 1 + kMargin
                               : toolbarRect.top() - kMargin - height();
    // Left edge on the BUTTON's left edge; the position-only clamp shifts
    // it inward at the right margin instead of clipping.
    move(clampedPos(QPoint(buttonRect.left(), y)));
    m_anchorOffset = pos() - anchorWidget()->mapToGlobal(QPoint(0, 0));
}

void BrushLibraryPanel::autoHide()
{
    // Bypasses the intent-recording setVisible() override (D1): drawing
    // dismissed the panel, the user did not choose "hidden".
    FloatingToolWindow::setVisible(false);
}

void BrushLibraryPanel::setVisible(bool visible)
{
    // Persist the INTENT here (D1) — never from hideEvent, where teardown
    // has already made isVisible() false regardless of what the user wants.
    QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"))
        .setValue(QStringLiteral("storyboard/brushLibrary/v1/visible"),
                  visible);
    FloatingToolWindow::setVisible(visible);
}

void BrushLibraryPanel::saveGeometry() const
{
    // Geometry only. The `visible` key is written exclusively by the
    // intent-recording setVisible() override above (D1).
    QSettings s(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    s.setValue(QStringLiteral("storyboard/brushLibrary/v1/geo"), geometry());
}

void BrushLibraryPanel::paintEvent(QPaintEvent *)
{
    // The panel chrome (Figma 245:23): #1e1e20 fill, 1px #2d2d35 border,
    // radius 10 — painted here like every FloatingToolWindow bar. (QSS
    // backgrounds work on the CHILD widgets but not on this translucent
    // top-level, so the separators are QSS on the header/sidebar and the
    // surface itself is painted.)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setPen(QPen(kPanelBorder, 1));
    p.setBrush(kPanelBg);
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10, 10);
}

BrushLibraryPanel::Edge BrushLibraryPanel::edgeAt(const QPoint &pos) const
{
    const bool right = pos.x() >= width() - kEdgeBand;
    const bool bottom = pos.y() >= height() - kEdgeBand;
    if (right && bottom)
        return Edge::Corner;
    if (right)
        return Edge::Right;
    if (bottom)
        return Edge::Bottom;
    return Edge::None;
}

void BrushLibraryPanel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint at = event->position().toPoint();
    m_resizing = edgeAt(at);
    if (m_resizing != Edge::None) {
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_dragStartSize = size();
        return;
    }
    if (!childAt(at)) { // background (incl. the header strip) drags the panel
        m_pressed = true;
        m_dragging = false;
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_dragStartPos = pos();
    }
}

void BrushLibraryPanel::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint at = event->position().toPoint();
    if (m_resizing != Edge::None && (event->buttons() & Qt::LeftButton)) {
        const QPoint d = event->globalPosition().toPoint() - m_dragStartGlobal;
        int w = m_dragStartSize.width();
        int h = m_dragStartSize.height();
        if (m_resizing != Edge::Bottom)
            w = qMax(kMinW, m_dragStartSize.width() + d.x());
        if (m_resizing != Edge::Right)
            h = qMax(kMinH, m_dragStartSize.height() + d.y());
        resize(w, h);
        return;
    }
    if (m_pressed && (event->buttons() & Qt::LeftButton)) {
        const QPoint g = event->globalPosition().toPoint();
        if (!m_dragging) {
            if ((g - m_dragStartGlobal).manhattanLength()
                < QApplication::startDragDistance())
                return;
            m_dragging = true;
        }
        move(clampedPos(m_dragStartPos + (g - m_dragStartGlobal)));
        return;
    }
    switch (edgeAt(at)) { // live resize affordance
    case Edge::Corner: setCursor(Qt::SizeFDiagCursor); break;
    case Edge::Right: setCursor(Qt::SizeHorCursor); break;
    case Edge::Bottom: setCursor(Qt::SizeVerCursor); break;
    case Edge::None:
        if (!childAt(at))
            setCursor(Qt::SizeAllCursor);
        else
            unsetCursor();
        break;
    }
}

void BrushLibraryPanel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_resizing != Edge::None || m_dragging) {
        m_anchorOffset =
            pos() - anchorWidget()->mapToGlobal(QPoint(0, 0));
        saveGeometry();
    }
    m_resizing = Edge::None;
    m_pressed = false;
    m_dragging = false;
}

void BrushLibraryPanel::leaveEvent(QEvent *)
{
    if (!m_dragging && m_resizing == Edge::None)
        unsetCursor();
}

void BrushLibraryPanel::showEvent(QShowEvent *event)
{
    FloatingToolWindow::showEvent(event);
    saveGeometry();
    emit visibilityChanged(true);
    // Kick previews for the visible category (cheap: cache-warm rows are
    // 2ms; cold rows fill progressively as the worker delivers).
    rebuildBrushList();
}

void BrushLibraryPanel::hideEvent(QHideEvent *event)
{
    FloatingToolWindow::hideEvent(event);
    saveGeometry();
    emit visibilityChanged(false);
}

void BrushLibraryPanel::moveEvent(QMoveEvent *event)
{
    FloatingToolWindow::moveEvent(event);
    if (m_restored)
        saveGeometry();
}

void BrushLibraryPanel::resizeEvent(QResizeEvent *event)
{
    FloatingToolWindow::resizeEvent(event);
    if (m_restored)
        saveGeometry();
}

} // namespace brushlib
