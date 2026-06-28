#include "StoryboardPage.h"

#include "DrawingCanvas.h"
#include "StoryboardModel.h"

#include <QButtonGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <Qt>

namespace {

constexpr int kThumbW = 160;
constexpr int kThumbH = 90; // 16:9

Panel *makePanel()
{
    Panel *panel = new Panel;
    panel->pixmap = QPixmap(DrawingCanvas::canvasSize());
    panel->pixmap.fill(Qt::white);
    return panel;
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
    content->addWidget(createRightColumn());

    root->addLayout(content, 1);
    root->addWidget(createBottomBar());
}

StoryboardPage::~StoryboardPage()
{
    for (Scene *scene : m_scenes)
        delete scene;
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

        QPushButton *addPanel = new QPushButton(QStringLiteral("+ Add Panel"));
        addPanel->setCursor(Qt::PointingHandCursor);
        addPanel->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: #1c1c1c; color: #999999; border: 1px solid #2a2a2a;"
            "  border-radius: 4px; padding: 4px; font-size: 11px;"
            "}"
            "QPushButton:hover { color: #f5a623; border-color: #f5a623; }"));
        connect(addPanel, &QPushButton::clicked, this, [this, i] { addPanelToScene(i); });
        cardLayout->addWidget(addPanel);

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

    // Panel thumbnail strip (top).
    QScrollArea *strip = new QScrollArea;
    strip->setWidgetResizable(true);
    strip->setFixedHeight(140);
    strip->setFrameShape(QFrame::NoFrame);
    strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    strip->setStyleSheet(QStringLiteral(
        "QScrollArea { background-color: #0d0d0d; border-bottom: 1px solid #1f1f1f; }"));

    QWidget *stripContainer = new QWidget;
    stripContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_panelStripLayout = new QHBoxLayout(stripContainer);
    m_panelStripLayout->setContentsMargins(14, 12, 14, 12);
    m_panelStripLayout->setSpacing(12);
    m_panelStripLayout->addStretch(1);

    strip->setWidget(stripContainer);
    layout->addWidget(strip);

    // Drawing area (toolbar + canvas).
    QWidget *drawRow = new QWidget;
    QHBoxLayout *drawLayout = new QHBoxLayout(drawRow);
    drawLayout->setContentsMargins(0, 0, 0, 0);
    drawLayout->setSpacing(0);

    drawLayout->addWidget(createToolbar());

    m_canvas = new DrawingCanvas;
    connect(m_canvas, &DrawingCanvas::contentChanged, this, &StoryboardPage::refreshCurrentThumb);
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
            thumb->setPixmap(panel->pixmap.scaled(kThumbW, kThumbH,
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

    // Add Panel button after the last thumbnail.
    QPushButton *add = new QPushButton(QStringLiteral("+ Add\nPanel"));
    add->setCursor(Qt::PointingHandCursor);
    add->setFixedSize(70, kThumbH);
    add->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #161616; color: #999999; border: 1px dashed #3a3a3a;"
        " border-radius: 6px; font-size: 11px; } QPushButton:hover { color: #f5a623; border-color: #f5a623; }"));
    connect(add, &QPushButton::clicked, this, [this] {
        if (m_currentScene >= 0)
            addPanelToScene(m_currentScene);
    });
    m_panelStripLayout->addWidget(add);

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

    layout->addStretch(1);

    QPushButton *animatic = new QPushButton(QStringLiteral("Continue to Animatic"));
    animatic->setEnabled(false);
    animatic->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #555555; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 9px 22px; font-size: 14px; font-weight: 600; }"));
    layout->addWidget(animatic);

    return bar;
}

// --- Data / selection -----------------------------------------------------

void StoryboardPage::loadScenes(const QJsonArray &scenes)
{
    for (Scene *scene : m_scenes)
        delete scene;
    m_scenes.clear();
    m_currentScene = -1;
    m_currentPanel = -1;

    for (const QJsonValue &value : scenes) {
        const QJsonObject obj = value.toObject();
        Scene *scene = new Scene;
        scene->number = obj.value(QStringLiteral("scene_number")).toInt();
        scene->location = obj.value(QStringLiteral("location")).toString();
        scene->timeOfDay = obj.value(QStringLiteral("time_of_day")).toString();
        scene->action = obj.value(QStringLiteral("action")).toString();
        scene->panels.append(makePanel()); // start each scene with one blank panel
        m_scenes.append(scene);
    }

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
        ->setPixmap(panel->pixmap.scaled(kThumbW, kThumbH,
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
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        const QVariant sceneIdx = object->property("sceneIndex");
        if (sceneIdx.isValid()) {
            selectScene(sceneIdx.toInt());
            return false;
        }
        const QVariant panelIdx = object->property("panelIndex");
        if (panelIdx.isValid()) {
            selectPanel(panelIdx.toInt());
            return false;
        }
    }
    return QWidget::eventFilter(object, event);
}
