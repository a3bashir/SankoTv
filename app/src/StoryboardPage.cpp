#include "StoryboardPage.h"

#include "DrawingCanvas.h"
#include "StoryboardModel.h"

#include <QButtonGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QKeySequence>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QVBoxLayout>
#include <Qt>

namespace {

constexpr int kThumbW = 160;
constexpr int kThumbH = 90; // 16:9

Panel *makePanel()
{
    return makeBlankPanel(); // one blank raster layer ("Layer 1"), 960x540
}

QPushButton *toolButton(const QString &text, const QString &tip)
{
    QPushButton *button = new QPushButton(text);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(34);
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        "  border-radius: 4px; font-size: 11px;"
        "}"
        "QPushButton:hover { background-color: #262626; }"
        "QPushButton:checked { background-color: #f5a623; color: #0a0a0a; border: none; font-weight: 600; }"));
    return button;
}

} // namespace

StoryboardPage::StoryboardPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    QHBoxLayout *content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(createLeftColumn());
    content->addWidget(createCenterColumn(), 1);
    content->addWidget(createLayerPanel()); // docked LAYERS column, right of the canvas
    content->addWidget(createRightColumn());

    root->addLayout(content, 1);
    root->addWidget(createBottomBar());

    // 'O' toggles onion skin.
    QShortcut *onionShortcut = new QShortcut(QKeySequence(Qt::Key_O), this);
    connect(onionShortcut, &QShortcut::activated, this, [this] {
        if (m_onionButton)
            m_onionButton->toggle(); // emits toggled -> updates canvas + ghost
    });

    // Ctrl+Left / Ctrl+Right move the current panel within its scene.
    QShortcut *movePanelLeft =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this);
    connect(movePanelLeft, &QShortcut::activated, this, [this] { movePanelBy(-1); });
    QShortcut *movePanelRight =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this);
    connect(movePanelRight, &QShortcut::activated, this, [this] { movePanelBy(1); });

    // Ctrl+D duplicates the current panel.
    QShortcut *duplicateShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this);
    connect(duplicateShortcut, &QShortcut::activated, this, [this] { duplicatePanel(); });

    // Ctrl+I imports an image onto the current panel.
    QShortcut *importShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_I), this);
    connect(importShortcut, &QShortcut::activated, this, [this] { importImageToPanel(); });

    // Ctrl+Z reverts the last canvas change (drawing or image import).
    QShortcut *undoShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), this);
    connect(undoShortcut, &QShortcut::activated, this, [this] {
        if (m_canvas)
            m_canvas->undo();
    });

    updateDuplicateButton(); // panel-action buttons disabled until a panel is selected
}

// --- Left column ----------------------------------------------------------

QWidget *StoryboardPage::createLeftColumn()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setFixedWidth(200);
    column->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(12, 14, 12, 12);
    layout->setSpacing(10);

    QLabel *header = new QLabel(QStringLiteral("Scenes"));
    header->setStyleSheet(QStringLiteral(
        "color: #ffffff; font-size: 13px; font-weight: 600; letter-spacing: 0.5px;"));
    layout->addWidget(header);

    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    QWidget *container = new QWidget;
    container->setStyleSheet(QStringLiteral("background: transparent;"));
    m_sceneListLayout = new QVBoxLayout(container);
    m_sceneListLayout->setContentsMargins(0, 0, 0, 0);
    m_sceneListLayout->setSpacing(8);
    m_sceneListLayout->addStretch(1);

    scroll->setWidget(container);
    layout->addWidget(scroll, 1);

    return column;
}

void StoryboardPage::rebuildSceneList()
{
    m_sceneCards.clear();
    // Remove everything (including the trailing stretch) and rebuild.
    while (QLayoutItem *item = m_sceneListLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    for (int i = 0; i < m_scenes.size(); ++i) {
        Scene *scene = m_scenes.at(i);

        QFrame *card = new QFrame;
        card->setObjectName(QStringLiteral("sceneCard"));
        card->setProperty("sceneIndex", i);
        card->setCursor(Qt::PointingHandCursor);
        card->installEventFilter(this);

        QVBoxLayout *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(6);

        QLabel *number = new QLabel(QStringLiteral("SCENE %1").arg(scene->number));
        number->setStyleSheet(QStringLiteral(
            "color: #f5a623; font-size: 11px; font-weight: 700; border: none; background: transparent;"));
        cardLayout->addWidget(number);

        QLabel *location = new QLabel(scene->location);
        location->setWordWrap(true);
        location->setStyleSheet(QStringLiteral(
            "color: #ffffff; font-size: 12px; border: none; background: transparent;"));
        cardLayout->addWidget(location);
        // (Add Panel moved to the fixed control column on the panel strip.)

        m_sceneListLayout->addWidget(card);
        m_sceneCards.append(card);
    }

    m_sceneListLayout->addStretch(1);
    updateSceneCardStyles();
}

void StoryboardPage::updateSceneCardStyles()
{
    for (int i = 0; i < m_sceneCards.size(); ++i) {
        const bool selected = (i == m_currentScene);
        m_sceneCards.at(i)->setStyleSheet(
            selected
                ? QStringLiteral("QFrame#sceneCard { background-color: #1b1b1b;"
                                 " border: 1px solid #2a2a2a; border-left: 3px solid #f5a623;"
                                 " border-radius: 6px; }")
                : QStringLiteral("QFrame#sceneCard { background-color: #161616;"
                                 " border: 1px solid #2a2a2a; border-radius: 6px; }"));
    }
}

// --- Center column --------------------------------------------------------

QWidget *StoryboardPage::createCenterColumn()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Panel strip row: a FIXED control column (never scrolls), a thin divider,
    // then the horizontally-scrolling thumbnail area — the column sits OUTSIDE
    // the QScrollArea so it stays pinned at the left edge.
    QWidget *stripBar = new QWidget;
    stripBar->setAttribute(Qt::WA_StyledBackground, true);
    stripBar->setFixedHeight(140);
    stripBar->setStyleSheet(QStringLiteral(
        "background-color: #0d0d0d; border-bottom: 1px solid #1f1f1f;"));
    QHBoxLayout *stripBarLayout = new QHBoxLayout(stripBar);
    stripBarLayout->setContentsMargins(0, 0, 0, 0);
    stripBarLayout->setSpacing(0);

    stripBarLayout->addWidget(createPanelControls()); // pinned, non-scrolling

    QFrame *divider = new QFrame; // 0.5px vertical divider (1px is the renderable minimum)
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("background-color: #2a2a2a; border: none;"));
    stripBarLayout->addWidget(divider);

    QScrollArea *strip = new QScrollArea;
    strip->setWidgetResizable(true);
    strip->setFrameShape(QFrame::NoFrame);
    strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    strip->setStyleSheet(QStringLiteral("QScrollArea { background-color: #0d0d0d; border: none; }"));

    QWidget *stripContainer = new QWidget;
    stripContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_panelStripLayout = new QHBoxLayout(stripContainer);
    m_panelStripLayout->setContentsMargins(14, 12, 14, 12);
    m_panelStripLayout->setSpacing(12);
    m_panelStripLayout->addStretch(1);

    strip->setWidget(stripContainer);
    m_panelScroll = strip; // kept so we can scroll a new panel into view
    stripBarLayout->addWidget(strip, 1); // only the thumbnails scroll

    layout->addWidget(stripBar);

    // Drawing area (toolbar + canvas).
    QWidget *drawRow = new QWidget;
    QHBoxLayout *drawLayout = new QHBoxLayout(drawRow);
    drawLayout->setContentsMargins(0, 0, 0, 0);
    drawLayout->setSpacing(0);

    drawLayout->addWidget(createToolbar());

    m_canvas = new DrawingCanvas;
    connect(m_canvas, &DrawingCanvas::contentChanged, this, &StoryboardPage::refreshCurrentThumb);
    connect(m_canvas, &DrawingCanvas::layersChanged, this, &StoryboardPage::rebuildLayerPanel);
    drawLayout->addWidget(m_canvas, 1);

    layout->addWidget(drawRow, 1);

    return column;
}

QWidget *StoryboardPage::createToolbar()
{
    QWidget *toolbar = new QWidget;
    toolbar->setAttribute(Qt::WA_StyledBackground, true);
    toolbar->setFixedWidth(72);
    toolbar->setStyleSheet(QStringLiteral(
        "background-color: #111111; border-right: 1px solid #1f1f1f;"));

    QVBoxLayout *layout = new QVBoxLayout(toolbar);
    layout->setContentsMargins(8, 12, 8, 12);
    layout->setSpacing(8);

    // Tool selection (exclusive).
    QButtonGroup *tools = new QButtonGroup(this);
    tools->setExclusive(true);

    QPushButton *brush = toolButton(QStringLiteral("Pen"), QStringLiteral("Brush \xE2\x80\x94 freehand"));
    QPushButton *eraser = toolButton(QStringLiteral("Erase"), QStringLiteral("Eraser"));
    QPushButton *line = toolButton(QStringLiteral("Line"), QStringLiteral("Straight line (click-drag)"));
    QPushButton *fill = toolButton(QStringLiteral("Fill"), QStringLiteral("Flood fill"));
    brush->setChecked(true);

    tools->addButton(brush);
    tools->addButton(eraser);
    tools->addButton(line);
    tools->addButton(fill);

    connect(brush, &QPushButton::clicked, this, [this] { m_canvas->setTool(DrawingCanvas::Brush); });
    connect(eraser, &QPushButton::clicked, this, [this] { m_canvas->setTool(DrawingCanvas::Eraser); });
    connect(line, &QPushButton::clicked, this, [this] { m_canvas->setTool(DrawingCanvas::Line); });
    connect(fill, &QPushButton::clicked, this, [this] { m_canvas->setTool(DrawingCanvas::Fill); });

    layout->addWidget(brush);
    layout->addWidget(eraser);
    layout->addWidget(line);
    layout->addWidget(fill);

    // Onion skin toggle (independent of the exclusive tool group).
    m_onionButton = toolButton(QStringLiteral("Onion"),
                               QStringLiteral("Onion skin (O) \xE2\x80\x94 ghost of previous panel"));
    connect(m_onionButton, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setOnionSkinEnabled(on);
        updateOnionGhost();
    });
    layout->addWidget(m_onionButton);

    // Color swatch.
    QPushButton *color = new QPushButton;
    color->setCursor(Qt::PointingHandCursor);
    color->setToolTip(QStringLiteral("Color"));
    color->setFixedHeight(28);
    color->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #000000; border: 2px solid #2a2a2a; border-radius: 4px; }"));
    connect(color, &QPushButton::clicked, this, [this, color] {
        const QColor chosen = QColorDialog::getColor(Qt::black, this, QStringLiteral("Brush color"));
        if (chosen.isValid()) {
            m_canvas->setColor(chosen);
            color->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; border: 2px solid #2a2a2a; border-radius: 4px; }")
                .arg(chosen.name()));
        }
    });
    layout->addWidget(color);

    // Brush size slider (vertical).
    QLabel *sizeLabel = new QLabel(QStringLiteral("Size"));
    sizeLabel->setAlignment(Qt::AlignCenter);
    sizeLabel->setStyleSheet(QStringLiteral("color: #777777; font-size: 10px;"));
    layout->addWidget(sizeLabel);

    QSlider *size = new QSlider(Qt::Vertical);
    size->setRange(1, 20);
    size->setValue(4);
    connect(size, &QSlider::valueChanged, this, [this](int v) { m_canvas->setBrushSize(v); });
    layout->addWidget(size, 1, Qt::AlignHCenter);

    // Undo / clear.
    QPushButton *undo = new QPushButton(QStringLiteral("Undo"));
    undo->setCursor(Qt::PointingHandCursor);
    undo->setFixedHeight(30);
    undo->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; font-size: 11px; } QPushButton:hover { background-color: #262626; }"));
    connect(undo, &QPushButton::clicked, this, [this] { m_canvas->undo(); });
    layout->addWidget(undo);

    QPushButton *clear = new QPushButton(QStringLiteral("Clear"));
    clear->setCursor(Qt::PointingHandCursor);
    clear->setFixedHeight(30);
    clear->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; font-size: 11px; } QPushButton:hover { color: #e06c6c; border-color: #e06c6c; }"));
    connect(clear, &QPushButton::clicked, this, [this] { m_canvas->clearCanvas(); });
    layout->addWidget(clear);

    return toolbar;
}

void StoryboardPage::rebuildPanelStrip()
{
    m_panelThumbs.clear();
    m_panelThumbImages.clear();
    while (QLayoutItem *item = m_panelStripLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    Scene *scene = currentScene();
    if (scene) {
        for (int i = 0; i < scene->panels.size(); ++i) {
            Panel *panel = scene->panels.at(i);

            QLabel *thumb = new QLabel;
            thumb->setObjectName(QStringLiteral("panelThumb"));
            thumb->setProperty("panelIndex", i);
            thumb->setFixedSize(kThumbW, kThumbH);
            thumb->setCursor(Qt::PointingHandCursor);
            thumb->setScaledContents(true);
            thumb->setPixmap(panel->flattenedPixmap().scaled(kThumbW, kThumbH,
                                                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            thumb->installEventFilter(this);

            // Panel number overlay, bottom-left.
            QLabel *num = new QLabel(QStringLiteral("P%1").arg(i + 1), thumb);
            num->setStyleSheet(QStringLiteral(
                "color: #f5a623; font-size: 11px; font-weight: 700;"
                " background: rgba(0,0,0,140); padding: 1px 4px; border-radius: 3px;"));
            num->move(5, kThumbH - 20);

            m_panelStripLayout->addWidget(thumb);
            m_panelThumbs.append(thumb);
            m_panelThumbImages.append(thumb);
        }
    }

    // (Add Panel now lives ONLY in the fixed control column at the left of the strip.)
    m_panelStripLayout->addStretch(1);
    updatePanelThumbStyles();
}

void StoryboardPage::updatePanelThumbStyles()
{
    for (int i = 0; i < m_panelThumbs.size(); ++i) {
        const bool selected = (i == m_currentPanel);
        m_panelThumbs.at(i)->setStyleSheet(
            selected
                ? QStringLiteral("QLabel#panelThumb { border: 2px solid #f5a623; border-radius: 4px; }")
                : QStringLiteral("QLabel#panelThumb { border: 1px solid #2a2a2a; border-radius: 4px;"
                                 " background-color: #161616; }"));
    }
}

// --- Right column ---------------------------------------------------------

QWidget *StoryboardPage::createRightColumn()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setFixedWidth(220);
    column->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    QLabel *header = new QLabel(QStringLiteral("Shot Info"));
    header->setStyleSheet(QStringLiteral(
        "color: #ffffff; font-size: 13px; font-weight: 600; letter-spacing: 0.5px;"));
    layout->addWidget(header);

    const QString labelStyle = QStringLiteral("color: #888888; font-size: 11px;");
    const QString fieldStyle = QStringLiteral(
        "QComboBox, QLineEdit, QPlainTextEdit {"
        "  background-color: #1a1a1a; color: #ffffff; border: 1px solid #2a2a2a;"
        "  border-radius: 4px; padding: 5px; font-size: 12px;"
        "}"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background-color: #1a1a1a; color: #ffffff;"
        "  selection-background-color: #f5a623; selection-color: #0a0a0a; }");

    auto addLabel = [&](const QString &text) {
        QLabel *l = new QLabel(text);
        l->setStyleSheet(labelStyle);
        layout->addWidget(l);
    };

    addLabel(QStringLiteral("Shot type"));
    m_shotType = new QComboBox;
    m_shotType->addItems({QStringLiteral("Close-up"), QStringLiteral("Medium"),
                          QStringLiteral("Wide"), QStringLiteral("Extreme Close-up"),
                          QStringLiteral("Extreme Wide"), QStringLiteral("Over-the-shoulder")});
    m_shotType->setStyleSheet(fieldStyle);
    layout->addWidget(m_shotType);

    addLabel(QStringLiteral("Camera angle"));
    m_camera = new QComboBox;
    m_camera->addItems({QStringLiteral("Eye level"), QStringLiteral("Low angle"),
                        QStringLiteral("High angle"), QStringLiteral("Bird's eye"),
                        QStringLiteral("Dutch angle")});
    m_camera->setStyleSheet(fieldStyle);
    layout->addWidget(m_camera);

    addLabel(QStringLiteral("Lens"));
    m_lens = new QComboBox;
    m_lens->addItems({QStringLiteral("Wide (16-24mm)"), QStringLiteral("Normal (35-50mm)"),
                      QStringLiteral("Telephoto (85mm+)")});
    m_lens->setStyleSheet(fieldStyle);
    layout->addWidget(m_lens);

    addLabel(QStringLiteral("Mood"));
    m_mood = new QLineEdit;
    m_mood->setPlaceholderText(QStringLiteral("e.g. tense, calm, mysterious"));
    m_mood->setStyleSheet(fieldStyle);
    layout->addWidget(m_mood);

    addLabel(QStringLiteral("Notes"));
    m_notes = new QPlainTextEdit;
    m_notes->setPlaceholderText(QStringLiteral("Director notes for this panel..."));
    m_notes->setStyleSheet(fieldStyle);
    m_notes->setFixedHeight(90);
    layout->addWidget(m_notes);

    // Parent scene action.
    QLabel *actionTitle = new QLabel(QStringLiteral("Action"));
    actionTitle->setStyleSheet(labelStyle);
    layout->addWidget(actionTitle);

    m_actionLabel = new QLabel;
    m_actionLabel->setWordWrap(true);
    m_actionLabel->setStyleSheet(QStringLiteral(
        "color: #777777; font-size: 12px; font-style: italic;"));
    layout->addWidget(m_actionLabel);

    layout->addStretch(1);

    // Auto-save on any change.
    connect(m_shotType, &QComboBox::currentTextChanged, this, [this] { saveShotInfo(); });
    connect(m_camera, &QComboBox::currentTextChanged, this, [this] { saveShotInfo(); });
    connect(m_lens, &QComboBox::currentTextChanged, this, [this] { saveShotInfo(); });
    connect(m_mood, &QLineEdit::textChanged, this, [this] { saveShotInfo(); });
    connect(m_notes, &QPlainTextEdit::textChanged, this, [this] { saveShotInfo(); });

    return column;
}

// --- Bottom bar -----------------------------------------------------------

QWidget *StoryboardPage::createBottomBar()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(56);
    bar->setStyleSheet(QStringLiteral(
        "background-color: #111111; border-top: 1px solid #2a2a2a;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);

    QPushButton *back = new QPushButton(QString::fromUtf8("\xE2\x86\x90  Script Editor"));
    back->setCursor(Qt::PointingHandCursor);
    back->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #999999; border: none; font-size: 14px;"
        " padding: 8px 6px; } QPushButton:hover { color: #ffffff; }"));
    connect(back, &QPushButton::clicked, this, &StoryboardPage::backRequested);
    layout->addWidget(back);

    QPushButton *consistency = new QPushButton(QStringLiteral("Consistency Board"));
    consistency->setCursor(Qt::PointingHandCursor);
    consistency->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 7px 14px; font-size: 13px; }"
        "QPushButton:hover { color: #f5a623; border-color: #f5a623; }"));
    connect(consistency, &QPushButton::clicked, this, &StoryboardPage::consistencyBoardRequested);
    layout->addWidget(consistency);

    // (Duplicate Panel moved to the fixed control column on the panel strip.)

    m_importButton = new QPushButton(QStringLiteral("Import Image"));
    m_importButton->setCursor(Qt::PointingHandCursor);
    m_importButton->setToolTip(QStringLiteral("Import a reference image onto this panel (Ctrl+I)"));
    m_importButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 7px 14px; font-size: 13px; }"
        "QPushButton:hover { color: #f5a623; border-color: #f5a623; }"
        "QPushButton:disabled { color: #555555; border-color: #1f1f1f; }"));
    connect(m_importButton, &QPushButton::clicked, this, &StoryboardPage::importImageToPanel);
    layout->addWidget(m_importButton);

    layout->addStretch(1);

    QPushButton *animatic = new QPushButton(QStringLiteral("Continue to Animatic"));
    animatic->setCursor(Qt::PointingHandCursor);
    animatic->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #ffffff; color: #0a0a0a; border: none;"
        " border-radius: 6px; padding: 9px 22px; font-size: 14px; font-weight: 600; }"
        "QPushButton:hover { background-color: #e6e6e6; }"));
    connect(animatic, &QPushButton::clicked, this,
            [this] { emit continueToAnimaticRequested(m_scenes); });
    layout->addWidget(animatic);

    return bar;
}

// --- Data / selection -----------------------------------------------------

void StoryboardPage::loadScenes(const QVector<Scene *> &scenes)
{
    m_scenes = scenes; // non-owning; MainWindow owns the Scene/Panel objects
    m_currentScene = -1;
    m_currentPanel = -1;

    rebuildSceneList();
    if (!m_scenes.isEmpty())
        selectScene(0);
    else
        rebuildPanelStrip();
}

void StoryboardPage::selectScene(int index)
{
    if (index < 0 || index >= m_scenes.size())
        return;
    m_currentScene = index;
    updateSceneCardStyles();

    Scene *scene = m_scenes.at(index);
    m_actionLabel->setText(scene->action);

    rebuildPanelStrip();
    if (!scene->panels.isEmpty())
        selectPanel(0);
    else {
        m_currentPanel = -1;
        m_canvas->setActivePanel(nullptr);
        updateDuplicateButton();
    }
}

void StoryboardPage::selectPanel(int index)
{
    Scene *scene = currentScene();
    if (!scene || index < 0 || index >= scene->panels.size())
        return;
    m_currentPanel = index;
    m_canvas->setActivePanel(scene->panels.at(index));
    updatePanelThumbStyles();
    loadShotInfo();
    updateOnionGhost();
    updateDuplicateButton();
    rebuildLayerPanel();
}

void StoryboardPage::updateOnionGhost()
{
    if (!m_canvas)
        return;
    Scene *scene = currentScene();
    const bool show = m_onionButton && m_onionButton->isChecked() && scene
        && m_currentPanel > 0 && m_currentPanel < scene->panels.size();
    if (show)
        m_canvas->setPreviousPixmap(scene->panels.at(m_currentPanel - 1)->flattenedPixmap());
    else
        m_canvas->setPreviousPixmap(QPixmap()); // clears the ghost
}

void StoryboardPage::addPanelToScene(int sceneIndex)
{
    if (sceneIndex < 0 || sceneIndex >= m_scenes.size())
        return;
    if (sceneIndex != m_currentScene)
        selectScene(sceneIndex);

    Scene *scene = m_scenes.at(sceneIndex);
    scene->panels.append(makePanel());
    rebuildPanelStrip();
    selectPanel(scene->panels.size() - 1);
}

// --- Shot info ------------------------------------------------------------

void StoryboardPage::loadShotInfo()
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    m_loadingShotInfo = true;
    m_shotType->setCurrentText(panel->shotType);
    m_camera->setCurrentText(panel->cameraAngle);
    m_lens->setCurrentText(panel->lens);
    m_mood->setText(panel->mood);
    m_notes->setPlainText(panel->notes);
    m_loadingShotInfo = false;
}

void StoryboardPage::saveShotInfo()
{
    if (m_loadingShotInfo)
        return;
    Panel *panel = currentPanel();
    if (!panel)
        return;
    panel->shotType = m_shotType->currentText();
    panel->cameraAngle = m_camera->currentText();
    panel->lens = m_lens->currentText();
    panel->mood = m_mood->text();
    panel->notes = m_notes->toPlainText();
}

void StoryboardPage::refreshCurrentThumb()
{
    Panel *panel = currentPanel();
    if (!panel || m_currentPanel < 0 || m_currentPanel >= m_panelThumbImages.size())
        return;
    m_panelThumbImages.at(m_currentPanel)
        ->setPixmap(panel->flattenedPixmap().scaled(kThumbW, kThumbH,
                                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// --- Helpers --------------------------------------------------------------

Scene *StoryboardPage::currentScene() const
{
    if (m_currentScene < 0 || m_currentScene >= m_scenes.size())
        return nullptr;
    return m_scenes.at(m_currentScene);
}

Panel *StoryboardPage::currentPanel() const
{
    Scene *scene = currentScene();
    if (!scene || m_currentPanel < 0 || m_currentPanel >= scene->panels.size())
        return nullptr;
    return scene->panels.at(m_currentPanel);
}

bool StoryboardPage::eventFilter(QObject *object, QEvent *event)
{
    // Panel thumbnails: select on click, drag to reorder.
    const QVariant panelIdx = object->property("panelIndex");
    if (panelIdx.isValid()) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                selectPanel(panelIdx.toInt());
                m_dragSourceIndex = panelIdx.toInt();
                m_dragStartGlobal = me->globalPosition().toPoint();
                m_panelPressActive = true;
                m_panelDragging = false;
            }
            return false; // let the label keep the implicit mouse grab
        }
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (!m_panelPressActive || !(me->buttons() & Qt::LeftButton))
                return false;
            const QPoint g = me->globalPosition().toPoint();
            if (!m_panelDragging
                && (g - m_dragStartGlobal).manhattanLength() >= 8) {
                beginPanelDrag();
            }
            if (m_panelDragging) {
                updatePanelDrag(g);
                return true;
            }
            return false;
        }
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && m_panelDragging) {
                finishPanelDrag();
                return true;
            }
            m_panelPressActive = false;
            return false;
        }
        default:
            break;
        }
    }

    // Scene cards: select on click.
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        const QVariant sceneIdx = object->property("sceneIndex");
        if (sceneIdx.isValid()) {
            selectScene(sceneIdx.toInt());
            return false;
        }
    }
    // Layer rows: click = make that layer active, double-click = rename.
    const QVariant layerIdx = object->property("layerIndex");
    if (layerIdx.isValid()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                setActiveLayer(layerIdx.toInt());
            return false;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            renameLayer(layerIdx.toInt());
            return true;
        }
    }

    return QWidget::eventFilter(object, event);
}

// --- Panel reordering -----------------------------------------------------

void StoryboardPage::beginPanelDrag()
{
    Scene *scene = currentScene();
    if (!scene || m_dragSourceIndex < 0 || m_dragSourceIndex >= m_panelThumbImages.size())
        return;
    m_panelDragging = true;

    // Semi-transparent ghost that follows the cursor.
    m_dragGhost = new QLabel(nullptr, Qt::FramelessWindowHint | Qt::Tool
                                          | Qt::WindowStaysOnTopHint
                                          | Qt::WindowTransparentForInput);
    m_dragGhost->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_dragGhost->setAttribute(Qt::WA_TranslucentBackground, true);
    m_dragGhost->setWindowOpacity(0.6);
    m_dragGhost->setFixedSize(kThumbW, kThumbH);
    m_dragGhost->setScaledContents(true);
    m_dragGhost->setPixmap(m_panelThumbImages.at(m_dragSourceIndex)->pixmap());
    m_dragGhost->show();

    // Amber drop-position indicator, drawn inside the strip container.
    QWidget *container = m_panelStripLayout->parentWidget();
    m_dropIndicator = new QWidget(container);
    m_dropIndicator->setStyleSheet(QStringLiteral("background-color: #f5a623;"));
    m_dropIndicator->hide();
}

void StoryboardPage::updatePanelDrag(const QPoint &globalPos)
{
    if (m_dragGhost)
        m_dragGhost->move(globalPos.x() - kThumbW / 2, globalPos.y() - kThumbH / 2);

    m_dropTarget = dropTargetForX(globalPos);

    if (m_dropIndicator && !m_panelThumbs.isEmpty()) {
        const QRect first = m_panelThumbs.first()->geometry();
        int x;
        if (m_dropTarget < m_panelThumbs.size())
            x = m_panelThumbs.at(m_dropTarget)->geometry().left() - 7;
        else
            x = m_panelThumbs.last()->geometry().right() + 5;
        m_dropIndicator->setGeometry(x, first.top(), 2, first.height());
        m_dropIndicator->raise();
        m_dropIndicator->show();
    }
}

int StoryboardPage::dropTargetForX(const QPoint &globalPos) const
{
    QWidget *container = m_panelStripLayout->parentWidget();
    if (!container)
        return 0;
    const int cx = container->mapFromGlobal(globalPos).x();
    int target = 0;
    for (int i = 0; i < m_panelThumbs.size(); ++i) {
        if (cx > m_panelThumbs.at(i)->geometry().center().x())
            target = i + 1;
        else
            break;
    }
    return target;
}

void StoryboardPage::finishPanelDrag()
{
    const int src = m_dragSourceIndex;
    const int target = m_dropTarget;
    cancelPanelDrag(); // tears down ghost/indicator, resets flags
    if (src >= 0)
        movePanel(src, target);
}

void StoryboardPage::cancelPanelDrag()
{
    if (m_dragGhost) {
        m_dragGhost->deleteLater();
        m_dragGhost = nullptr;
    }
    if (m_dropIndicator) {
        m_dropIndicator->deleteLater();
        m_dropIndicator = nullptr;
    }
    m_panelDragging = false;
    m_panelPressActive = false;
    m_dragSourceIndex = -1;
    m_dropTarget = -1;
}

void StoryboardPage::movePanel(int from, int target)
{
    Scene *scene = currentScene();
    if (!scene || from < 0 || from >= scene->panels.size())
        return;
    // Dropping right before or right after itself is a no-op.
    if (target == from || target == from + 1)
        return;
    target = qBound(0, target, scene->panels.size());

    Panel *selected = currentPanel(); // keep selection by pointer
    Panel *moved = scene->panels.takeAt(from);
    const int dest = (target > from) ? target - 1 : target;
    scene->panels.insert(dest, moved);

    rebuildPanelStrip();
    int newSel = scene->panels.indexOf(selected);
    if (newSel < 0)
        newSel = scene->panels.indexOf(moved);
    selectPanel(newSel >= 0 ? newSel : 0);
}

void StoryboardPage::movePanelBy(int delta)
{
    Scene *scene = currentScene();
    if (!scene || m_currentPanel < 0)
        return;
    const int dst = m_currentPanel + delta;
    if (dst < 0 || dst >= scene->panels.size())
        return; // clamp at boundaries, no wrap-around
    scene->panels.move(m_currentPanel, dst);
    rebuildPanelStrip();
    selectPanel(dst);
}

void StoryboardPage::duplicatePanel()
{
    Scene *scene = currentScene();
    Panel *source = currentPanel();
    if (!scene || !source || m_currentPanel < 0)
        return;

    Panel *copy = new Panel;
    copy->layers = source->layers; // QImage is copy-on-write: painting detaches, so this is a safe deep copy
    for (Layer &layer : copy->layers) // fresh ids so undo/UI never cross panels
        layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy->activeLayerIndex = source->activeLayerIndex;
    copy->duration = source->duration;
    copy->shotType = source->shotType;
    copy->cameraAngle = source->cameraAngle;
    copy->lens = source->lens;
    copy->mood = source->mood;
    copy->notes = source->notes;
    // undoStack intentionally left empty — the duplicate starts fresh.

    const int insertAt = m_currentPanel + 1;
    scene->panels.insert(insertAt, copy);

    rebuildPanelStrip();
    selectPanel(insertAt);

    // Scroll the new panel into view.
    if (m_panelScroll && insertAt < m_panelThumbs.size())
        m_panelScroll->ensureWidgetVisible(m_panelThumbs.at(insertAt));
}

void StoryboardPage::importImageToPanel()
{
    if (!currentPanel() || !m_canvas)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Image"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
        return;
    m_canvas->importImage(path); // handles blank check, warning, undo, refresh
}

// --- Fixed panel-control column ------------------------------------------

namespace {

enum class CtrlIcon { Add, Duplicate, Delete };

// Crisp painted glyphs (no font/emoji dependency). knockout = the colour drawn
// behind the front square of the Duplicate icon so the two squares read as
// stacked rather than as overlapping outlines.
QIcon paintCtrlIcon(CtrlIcon kind, const QColor &color, const QColor &knockout)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    if (kind == CtrlIcon::Add) {
        p.drawLine(QPointF(10, 4), QPointF(10, 16));
        p.drawLine(QPointF(4, 10), QPointF(16, 10));
    } else if (kind == CtrlIcon::Duplicate) {
        p.drawRoundedRect(QRectF(7.5, 3.5, 9, 9), 1.5, 1.5);       // back square
        p.setBrush(knockout);                                     // hide the overlap
        p.drawRoundedRect(QRectF(3.5, 7.5, 9, 9), 1.5, 1.5);      // front square
        p.setBrush(Qt::NoBrush);
    } else { // Delete: trash can
        p.drawLine(QPointF(4, 6), QPointF(16, 6));                 // lid
        p.drawLine(QPointF(8, 6), QPointF(8.6, 4)); p.drawLine(QPointF(12, 6), QPointF(11.4, 4)); // handle
        p.drawLine(QPointF(6, 6.5), QPointF(6.9, 16.5));          // body left
        p.drawLine(QPointF(14, 6.5), QPointF(13.1, 16.5));       // body right
        p.drawLine(QPointF(6.9, 16.5), QPointF(13.1, 16.5));     // body bottom
        p.drawLine(QPointF(10, 8.5), QPointF(10, 14.5));         // centre rib
    }
    p.end();
    return QIcon(pm);
}

QPushButton *makeCtrlButton(CtrlIcon kind, const QColor &iconColor,
                            const QString &tooltipHtml, const QString &styleSheet)
{
    QPushButton *button = new QPushButton;
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(40, 40);
    button->setIcon(paintCtrlIcon(kind, iconColor, QColor(0x0d, 0x0d, 0x0d)));
    button->setIconSize(QSize(20, 20));
    button->setToolTip(tooltipHtml); // Qt renders HTML tooltips automatically
    button->setStyleSheet(styleSheet);
    return button;
}

} // namespace

QWidget *StoryboardPage::createPanelControls()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setFixedWidth(56);
    column->setStyleSheet(QStringLiteral("background-color: #0d0d0d;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(8, 12, 8, 12);
    layout->setSpacing(8);

    // Add — purple fill, white icon.
    m_addPanelButton = makeCtrlButton(
        CtrlIcon::Add, QColor(0xff, 0xff, 0xff),
        QStringLiteral("<b>Add Panel</b> | Creates a new storyboard panel."),
        QStringLiteral(
            "QPushButton { background-color: #7c6ef6; border: none; border-radius: 8px; }"
            "QPushButton:hover { background-color: #8f82f8; }"
            "QPushButton:disabled { background-color: #3a3550; }"));
    connect(m_addPanelButton, &QPushButton::clicked, this, [this] { addPanelAfterSelected(); });
    layout->addWidget(m_addPanelButton);

    // Duplicate — transparent, light grey icon, grey border.
    m_dupPanelButton = makeCtrlButton(
        CtrlIcon::Duplicate, QColor(0xcc, 0xcc, 0xcc),
        QStringLiteral("<b>Duplicate Panel</b> | Copies the selected panel."),
        QStringLiteral(
            "QPushButton { background-color: transparent; border: 1px solid #3a3a3a; border-radius: 8px; }"
            "QPushButton:hover { border-color: #5a5a5a; background-color: #161616; }"
            "QPushButton:disabled { border-color: #242424; }"));
    connect(m_dupPanelButton, &QPushButton::clicked, this, [this] { duplicatePanel(); });
    layout->addWidget(m_dupPanelButton);

    // Delete — transparent, red icon, dark-red border.
    m_deletePanelButton = makeCtrlButton(
        CtrlIcon::Delete, QColor(0xe0, 0x65, 0x5f),
        QStringLiteral("<b>Delete Panel</b> | Removes the selected panel. Asks first."),
        QStringLiteral(
            "QPushButton { background-color: transparent; border: 1px solid #5a2a2a; border-radius: 8px; }"
            "QPushButton:hover { border-color: #e0655f; background-color: #1a0f0f; }"
            "QPushButton:disabled { border-color: #2a1a1a; }"));
    connect(m_deletePanelButton, &QPushButton::clicked, this, [this] { deleteSelectedPanel(); });
    layout->addWidget(m_deletePanelButton);

    layout->addStretch(1); // buttons pinned to the top
    return column;
}

void StoryboardPage::addPanelAfterSelected()
{
    Scene *scene = currentScene();
    if (!scene)
        return;
    // Insert immediately AFTER the selected panel (or append when the scene is empty).
    const int insertAt = (m_currentPanel >= 0 && m_currentPanel < scene->panels.size())
        ? m_currentPanel + 1
        : scene->panels.size();
    scene->panels.insert(insertAt, makePanel());
    rebuildPanelStrip();
    selectPanel(insertAt);
    if (m_panelScroll && insertAt < m_panelThumbs.size())
        m_panelScroll->ensureWidgetVisible(m_panelThumbs.at(insertAt));
}

void StoryboardPage::deleteSelectedPanel()
{
    Scene *scene = currentScene();
    Panel *panel = currentPanel();
    if (!scene || !panel || m_currentPanel < 0)
        return;

    if (scene->panels.size() <= 1) {
        QMessageBox::information(this, QStringLiteral("Delete Panel"),
                                 QStringLiteral("A scene must keep at least one panel."));
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Delete this panel?"), QStringLiteral("This cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    const int removeAt = m_currentPanel;
    delete scene->panels.at(removeAt); // Scene owns its Panel objects; free the removed one
    scene->panels.removeAt(removeAt);

    rebuildPanelStrip();
    selectPanel(qBound(0, removeAt, scene->panels.size() - 1)); // nearest remaining panel
}

void StoryboardPage::updateDuplicateButton()
{
    Scene *scene = currentScene();
    const bool hasScene = (scene != nullptr);
    const bool hasPanel = (currentPanel() != nullptr);
    if (m_addPanelButton)
        m_addPanelButton->setEnabled(hasScene);                 // needs a scene to add into
    if (m_dupPanelButton)
        m_dupPanelButton->setEnabled(hasPanel);
    if (m_deletePanelButton)                                     // blocked on the last remaining panel
        m_deletePanelButton->setEnabled(hasPanel && scene && scene->panels.size() > 1);
    if (m_importButton)
        m_importButton->setEnabled(hasPanel);
}

// --- Layer panel ------------------------------------------------------------

namespace {

QPushButton *layerActionButton(const QString &text, const QString &tip)
{
    QPushButton *button = new QPushButton(text);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(26);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; font-size: 10px; }"
        "QPushButton:hover { background-color: #262626; }"
        "QPushButton:disabled { color: #555555; }"));
    return button;
}

// 40x22 layer thumbnail over a dark checker-free backdrop (transparent areas
// read as the panel background, not white).
QPixmap layerThumb(const Layer &layer)
{
    QImage out(40, 22, QImage::Format_ARGB32_Premultiplied);
    out.fill(QColor(0x1b, 0x1b, 0x1b));
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!layer.image.isNull())
        painter.drawImage(QRect(0, 0, 40, 22), layer.image);
    painter.end();
    return QPixmap::fromImage(out);
}

} // namespace

QWidget *StoryboardPage::createLayerPanel()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setFixedWidth(200);
    column->setStyleSheet(QStringLiteral(
        "background-color: #111111; border-left: 1px solid #1f1f1f;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(10, 12, 10, 12);
    layout->setSpacing(8);

    QLabel *title = new QLabel(QStringLiteral("LAYERS"));
    title->setStyleSheet(QStringLiteral(
        "color: #777777; font-size: 11px; font-weight: 700; letter-spacing: 2px; border: none;"));
    layout->addWidget(title);

    // Opacity of the ACTIVE layer.
    QHBoxLayout *opacityRow = new QHBoxLayout;
    opacityRow->setSpacing(6);
    QLabel *opacityLabel = new QLabel(QStringLiteral("Opacity"));
    opacityLabel->setStyleSheet(QStringLiteral("color: #999999; font-size: 10px; border: none;"));
    m_layerOpacityValue = new QLabel(QStringLiteral("100%"));
    m_layerOpacityValue->setStyleSheet(QStringLiteral("color: #f5a623; font-size: 10px; border: none;"));
    opacityRow->addWidget(opacityLabel);
    opacityRow->addStretch(1);
    opacityRow->addWidget(m_layerOpacityValue);
    layout->addLayout(opacityRow);

    m_layerOpacity = new QSlider(Qt::Horizontal);
    m_layerOpacity->setRange(0, 100);
    m_layerOpacity->setValue(100);
    connect(m_layerOpacity, &QSlider::valueChanged, this, [this](int v) {
        if (m_updatingLayerUi)
            return;
        Panel *panel = currentPanel();
        Layer *layer = panel ? panel->activeLayer() : nullptr;
        if (!layer)
            return;
        layer->opacity = v / 100.0;
        m_layerOpacityValue->setText(QStringLiteral("%1%").arg(v));
        refreshLayerCanvas(); // live: canvas composite + panel thumbnail
    });
    layout->addWidget(m_layerOpacity);

    // Layer rows (top = frontmost), scrollable.
    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    QWidget *listHost = new QWidget;
    listHost->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_layerListLayout = new QVBoxLayout(listHost);
    m_layerListLayout->setContentsMargins(0, 0, 0, 0);
    m_layerListLayout->setSpacing(4);
    m_layerListLayout->addStretch(1);
    scroll->setWidget(listHost);
    layout->addWidget(scroll, 1);

    // Actions.
    QHBoxLayout *row1 = new QHBoxLayout;
    row1->setSpacing(6);
    QPushButton *add = layerActionButton(QStringLiteral("+ Layer"),
                                         QStringLiteral("Add a blank layer above the active one"));
    QPushButton *addImage = layerActionButton(QStringLiteral("+ Image"),
                                              QStringLiteral("Import an image as a new layer"));
    connect(add, &QPushButton::clicked, this, [this] { layerAdd(); });
    connect(addImage, &QPushButton::clicked, this, [this] { layerAddImage(); });
    row1->addWidget(add);
    row1->addWidget(addImage);
    layout->addLayout(row1);

    QHBoxLayout *row2 = new QHBoxLayout;
    row2->setSpacing(6);
    m_layerMergeButton = layerActionButton(QStringLiteral("Merge Down"),
                                           QStringLiteral("Flatten the active layer into the one below"));
    m_layerDeleteButton = layerActionButton(QStringLiteral("Delete"),
                                            QStringLiteral("Remove the active layer"));
    connect(m_layerMergeButton, &QPushButton::clicked, this, [this] { layerMergeDown(); });
    connect(m_layerDeleteButton, &QPushButton::clicked, this, [this] { layerDelete(); });
    row2->addWidget(m_layerMergeButton);
    row2->addWidget(m_layerDeleteButton);
    layout->addLayout(row2);

    QHBoxLayout *row3 = new QHBoxLayout;
    row3->setSpacing(6);
    QPushButton *up = layerActionButton(QStringLiteral("▲"),
                                        QStringLiteral("Move the active layer up (toward the front)"));
    QPushButton *down = layerActionButton(QStringLiteral("▼"),
                                          QStringLiteral("Move the active layer down (toward the back)"));
    connect(up, &QPushButton::clicked, this, [this] { layerMove(+1); });   // front = higher index
    connect(down, &QPushButton::clicked, this, [this] { layerMove(-1); });
    row3->addWidget(up);
    row3->addWidget(down);
    layout->addLayout(row3);

    return column;
}

void StoryboardPage::rebuildLayerPanel()
{
    if (!m_layerListLayout)
        return;

    m_layerRows.clear();
    while (QLayoutItem *item = m_layerListLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    Panel *panel = currentPanel();
    const bool hasPanel = (panel != nullptr) && !panel->layers.isEmpty();

    // Sync the opacity slider to the active layer (guarded — no feedback loop).
    m_updatingLayerUi = true;
    const Layer *active = hasPanel ? panel->activeLayer() : nullptr;
    const int pct = active ? qRound(qBound(0.0, active->opacity, 1.0) * 100.0) : 100;
    if (m_layerOpacity) {
        m_layerOpacity->setEnabled(active != nullptr);
        m_layerOpacity->setValue(pct);
    }
    if (m_layerOpacityValue)
        m_layerOpacityValue->setText(QStringLiteral("%1%").arg(pct));
    m_updatingLayerUi = false;

    if (m_layerDeleteButton)
        m_layerDeleteButton->setEnabled(hasPanel && panel->layers.size() > 1);
    if (m_layerMergeButton)
        m_layerMergeButton->setEnabled(hasPanel && panel->activeLayerIndex > 0);

    if (!hasPanel) {
        m_layerListLayout->addStretch(1);
        return;
    }

    // Top row = frontmost layer = highest vector index.
    for (int i = panel->layers.size() - 1; i >= 0; --i) {
        const Layer &layer = panel->layers.at(i);
        const bool isActive = (i == panel->activeLayerIndex);

        QFrame *row = new QFrame;
        row->setObjectName(QStringLiteral("layerRow"));
        row->setProperty("layerIndex", i);
        row->setFixedHeight(32);
        row->setCursor(Qt::PointingHandCursor);
        row->installEventFilter(this); // click = activate, double-click = rename
        row->setStyleSheet(
            isActive
                ? QStringLiteral("QFrame#layerRow { background-color: #1b1b1b;"
                                 " border: 1px solid #2a2a2a; border-left: 3px solid #f5a623;"
                                 " border-radius: 4px; }")
                : QStringLiteral("QFrame#layerRow { background-color: #161616;"
                                 " border: 1px solid #232323; border-radius: 4px; }"));

        QHBoxLayout *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 2, 4, 2);
        rowLayout->setSpacing(5);

        // Visibility (eye) toggle.
        QPushButton *eye = new QPushButton(layer.visible ? QStringLiteral("\U0001F441")
                                                         : QStringLiteral("–"));
        eye->setToolTip(QStringLiteral("Show / hide layer"));
        eye->setCursor(Qt::PointingHandCursor);
        eye->setFixedSize(20, 20);
        eye->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; color: %1; font-size: 11px; }")
            .arg(layer.visible ? QStringLiteral("#cccccc") : QStringLiteral("#555555")));
        connect(eye, &QPushButton::clicked, this, [this, i] {
            Panel *p = currentPanel();
            if (!p || i < 0 || i >= p->layers.size())
                return;
            p->layers[i].visible = !p->layers[i].visible;
            refreshLayerCanvas();
            rebuildLayerPanel();
        });
        rowLayout->addWidget(eye);

        // Layer thumbnail.
        QLabel *thumb = new QLabel;
        thumb->setFixedSize(40, 22);
        thumb->setStyleSheet(QStringLiteral("border: 1px solid #2a2a2a; border-radius: 2px;"));
        thumb->setPixmap(layerThumb(layer));
        rowLayout->addWidget(thumb);

        // Name (double-click the row to rename).
        QLabel *name = new QLabel(layer.name);
        name->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 11px; border: none; background: transparent;")
            .arg(layer.visible ? QStringLiteral("#cccccc") : QStringLiteral("#666666")));
        rowLayout->addWidget(name, 1);

        // Lock toggle (padlock).
        QPushButton *lock = new QPushButton(layer.locked ? QStringLiteral("\U0001F512")
                                                         : QStringLiteral("\U0001F513"));
        lock->setToolTip(QStringLiteral("Lock / unlock layer"));
        lock->setCursor(Qt::PointingHandCursor);
        lock->setFixedSize(20, 20);
        lock->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; color: %1; font-size: 10px; }")
            .arg(layer.locked ? QStringLiteral("#f5a623") : QStringLiteral("#555555")));
        connect(lock, &QPushButton::clicked, this, [this, i] {
            Panel *p = currentPanel();
            if (!p || i < 0 || i >= p->layers.size())
                return;
            p->layers[i].locked = !p->layers[i].locked;
            rebuildLayerPanel();
        });
        rowLayout->addWidget(lock);

        m_layerListLayout->addWidget(row);
        m_layerRows.append(row);
    }
    m_layerListLayout->addStretch(1);
}

void StoryboardPage::setActiveLayer(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    panel->activeLayerIndex = index;
    rebuildLayerPanel(); // amber highlight + slider follow the new active layer
}

void StoryboardPage::renameLayer(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename Layer"), QStringLiteral("Layer name:"),
        QLineEdit::Normal, panel->layers.at(index).name, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    panel->layers[index].name = name.trimmed();
    rebuildLayerPanel();
}

void StoryboardPage::layerAdd()
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    Layer layer = makeRasterLayer(QStringLiteral("Layer %1").arg(panel->layers.size() + 1));
    const int insertAt = qBound(0, panel->activeLayerIndex + 1, panel->layers.size());
    panel->layers.insert(insertAt, layer);
    panel->activeLayerIndex = insertAt;
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerAddImage()
{
    if (!currentPanel() || !m_canvas)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Image Layer"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
        return;
    m_canvas->importImage(path); // inserts the layer + emits layersChanged -> rebuild
}

void StoryboardPage::layerDelete()
{
    Panel *panel = currentPanel();
    if (!panel || panel->layers.size() <= 1)
        return; // the last remaining layer can never be deleted
    panel->layers.removeAt(panel->activeLayerIndex);
    panel->activeLayerIndex = qBound(0, panel->activeLayerIndex, panel->layers.size() - 1);
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerMergeDown()
{
    Panel *panel = currentPanel();
    if (!panel || panel->activeLayerIndex <= 0
        || panel->activeLayerIndex >= panel->layers.size())
        return;

    const int idx = panel->activeLayerIndex;
    Layer &below = panel->layers[idx - 1];
    const Layer &top = panel->layers.at(idx);

    QPainter painter(&below.image); // bake the active layer (with its opacity) into the one below
    painter.setOpacity(qBound(0.0, top.opacity, 1.0));
    painter.drawImage(0, 0, top.image);
    painter.end();

    panel->layers.removeAt(idx);
    panel->activeLayerIndex = idx - 1;
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerMove(int delta)
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    const int from = panel->activeLayerIndex;
    const int to = from + delta;
    if (from < 0 || from >= panel->layers.size() || to < 0 || to >= panel->layers.size())
        return;
    panel->layers.swapItemsAt(from, to);
    panel->activeLayerIndex = to;
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::refreshLayerCanvas()
{
    if (m_canvas)
        m_canvas->update();
    refreshCurrentThumb();
}
