#include "ScriptEditorPage.h"

#include "CodeEditor.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <Qt>

namespace {

const char *kPlaceholder =
    "Write your script here. Use screenplay format or plain language \xE2\x80\x94 both work.";

// Button stylesheets shared between states.
const char *kAmberButton =
    "QPushButton {"
    "  background-color: #f5a623; color: #0a0a0a; border: none; border-radius: 6px;"
    "  padding: 9px 22px; font-size: 14px; font-weight: 600;"
    "}"
    "QPushButton:hover { background-color: #ffb733; }"
    "QPushButton:pressed { background-color: #e0991c; }"
    "QPushButton:disabled { background-color: #5a4416; color: #997a3a; }";

// --- Local scene parsing ---------------------------------------------------
// Builds the scene breakdown directly from the script text — no API, no
// network. Screenplay INT./EXT. headings become scenes; otherwise each
// blank-line-separated paragraph becomes a scene.

QString detectTimeOfDay(const QString &text)
{
    const QString upper = text.toUpper();
    if (upper.contains(QLatin1String("NIGHT"))) return QStringLiteral("NIGHT");
    if (upper.contains(QLatin1String("DAWN")))  return QStringLiteral("DAWN");
    if (upper.contains(QLatin1String("DUSK")))  return QStringLiteral("DUSK");
    if (upper.contains(QLatin1String("DAY")))   return QStringLiteral("DAY");
    return QStringLiteral("UNSPECIFIED");
}

QString truncateAction(QString text)
{
    text = text.simplified();
    if (text.length() > 100)
        text = text.left(97) + QStringLiteral("...");
    return text;
}

QJsonObject makeScene(int number, const QString &location,
                      const QString &timeOfDay, const QString &action)
{
    QJsonObject scene;
    scene[QStringLiteral("scene_number")] = number;
    scene[QStringLiteral("location")] = location.isEmpty() ? QStringLiteral("UNKNOWN") : location;
    scene[QStringLiteral("time_of_day")] = timeOfDay;
    scene[QStringLiteral("action")] = action.isEmpty() ? QStringLiteral("Scene continues.") : action;
    return scene;
}

// Derive a pseudo-location from the first few words of a paragraph.
QString deriveLocation(const QString &paragraph)
{
    const QStringList words = paragraph.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList head;
    for (int i = 0; i < words.size() && i < 3; ++i)
        head << words.at(i);
    return head.join(QLatin1Char(' ')).toUpper();
}

QJsonArray buildSceneBreakdown(const QString &script)
{
    QJsonArray scenes;

    // Heading detection / prefix-strip regexes for screenplay slug lines.
    static const QRegularExpression headingRe(
        QStringLiteral("^\\s*(?:INT|EXT)\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression prefixRe(
        QStringLiteral("^\\s*(?:INT|EXT)\\.?(?:/(?:INT|EXT)\\.?)?\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression separatorRe(
        QStringLiteral("\\s[-\\x{2013}\\x{2014}]\\s")); // " - ", en/em dash

    const QStringList lines = script.split(QLatin1Char('\n'));

    QVector<int> headings;
    for (int i = 0; i < lines.size(); ++i) {
        if (headingRe.match(lines.at(i)).hasMatch())
            headings.append(i);
    }

    int number = 1;

    if (!headings.isEmpty()) {
        // Screenplay format: one scene per INT./EXT. heading.
        for (int h = 0; h < headings.size(); ++h) {
            const int start = headings.at(h);
            const int end = (h + 1 < headings.size()) ? headings.at(h + 1) : lines.size();

            const QString heading = lines.at(start).trimmed();
            QString rest = heading;
            rest.remove(prefixRe);
            rest = rest.trimmed();

            QString location = rest;
            QString timeText = heading;
            const int sep = rest.indexOf(separatorRe);
            if (sep >= 0) {
                location = rest.left(sep).trimmed();
                timeText = rest.mid(sep).trimmed();
            }

            QStringList actionLines;
            for (int j = start + 1; j < end; ++j) {
                const QString t = lines.at(j).trimmed();
                if (!t.isEmpty())
                    actionLines << t;
            }

            scenes.append(makeScene(number++, location,
                                    detectTimeOfDay(timeText),
                                    truncateAction(actionLines.join(QLatin1Char(' ')))));
        }
    } else {
        // Plain language: one scene per blank-line-separated paragraph.
        static const QRegularExpression paragraphSplit(QStringLiteral("\\n\\s*\\n"));
        const QStringList paragraphs =
            script.split(paragraphSplit, Qt::SkipEmptyParts);
        for (const QString &paragraph : paragraphs) {
            const QString para = paragraph.simplified();
            if (para.isEmpty())
                continue;
            scenes.append(makeScene(number++, deriveLocation(para),
                                    detectTimeOfDay(para), truncateAction(para)));
        }
        // Fallback: a single non-empty line with no blank separators.
        if (scenes.isEmpty() && !script.trimmed().isEmpty()) {
            scenes.append(makeScene(1, deriveLocation(script),
                                    detectTimeOfDay(script),
                                    truncateAction(script)));
        }
    }

    return scenes;
}

} // namespace

ScriptEditorPage::ScriptEditorPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Main content row: writing area (60%) + breakdown panel (40%).
    QHBoxLayout *content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(createWritingPanel(), 60);
    content->addWidget(createBreakdownPanel(), 40);

    root->addLayout(content, 1);
    root->addWidget(createBottomBar());

}

QWidget *ScriptEditorPage::createWritingPanel()
{
    QWidget *panel = new QWidget;
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(8);

    // --- Autosave indicator (top right) ----------------------------------
    QHBoxLayout *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->addStretch(1);

    m_savedDot = new QLabel;
    m_savedDot->setFixedSize(8, 8);
    m_savedDot->setStyleSheet(QStringLiteral(
        "background-color: #666666; border-radius: 4px;"));
    statusRow->addWidget(m_savedDot);

    m_savedText = new QLabel(QStringLiteral("Saved"));
    m_savedText->setStyleSheet(QStringLiteral("color: #666666; font-size: 12px;"));
    statusRow->addWidget(m_savedText);

    layout->addLayout(statusRow);

    // --- Editor ----------------------------------------------------------
    m_editor = new CodeEditor;
    m_editor->setPlaceholderText(QString::fromUtf8(kPlaceholder));
    m_editor->setFrameShape(QFrame::NoFrame);

    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(14);
    m_editor->setFont(mono);

    m_editor->setStyleSheet(QStringLiteral(
        "QPlainTextEdit {"
        "  background-color: #0a0a0a; color: #ffffff; border: none;"
        "  selection-background-color: #f5a623; selection-color: #0a0a0a;"
        "}"));
    layout->addWidget(m_editor, 1);

    // Simple autosave simulation: while typing show "Saving…", settle to "Saved".
    m_savedTimer = new QTimer(this);
    m_savedTimer->setSingleShot(true);
    m_savedTimer->setInterval(800);
    connect(m_savedTimer, &QTimer::timeout, this, [this] {
        m_savedDot->setStyleSheet(QStringLiteral("background-color: #666666; border-radius: 4px;"));
        m_savedText->setText(QStringLiteral("Saved"));
        m_savedText->setStyleSheet(QStringLiteral("color: #666666; font-size: 12px;"));
    });
    connect(m_editor, &CodeEditor::textChanged, this, [this] {
        m_savedDot->setStyleSheet(QStringLiteral("background-color: #f5a623; border-radius: 4px;"));
        m_savedText->setText(QStringLiteral("Saving\xE2\x80\xA6"));
        m_savedText->setStyleSheet(QStringLiteral("color: #999999; font-size: 12px;"));
        m_savedTimer->start();
    });

    return panel;
}

QWidget *ScriptEditorPage::createBreakdownPanel()
{
    QWidget *panel = new QWidget;
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    QLabel *header = new QLabel(QStringLiteral("Scene Breakdown"));
    header->setStyleSheet(QStringLiteral(
        "color: #ffffff; font-size: 13px; font-weight: 600; letter-spacing: 0.5px;"));
    layout->addWidget(header);

    // Scrollable list of scene cards.
    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    QWidget *container = new QWidget;
    container->setAttribute(Qt::WA_StyledBackground, true);
    container->setStyleSheet(QStringLiteral("background: transparent;"));
    m_scenesLayout = new QVBoxLayout(container);
    m_scenesLayout->setContentsMargins(0, 0, 0, 0);
    m_scenesLayout->setSpacing(12);

    scroll->setWidget(container);
    layout->addWidget(scroll, 1);

    // Initial empty state.
    showBreakdownMessage(QStringLiteral("Parse your script to see scenes"),
                         QStringLiteral("#666666"));

    return panel;
}

QWidget *ScriptEditorPage::createBottomBar()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(56);
    bar->setStyleSheet(QStringLiteral(
        "background-color: #111111; border-top: 1px solid #2a2a2a;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(10);

    // Back arrow (left).
    QPushButton *back = new QPushButton(QString::fromUtf8("\xE2\x86\x90  Dashboard"));
    back->setCursor(Qt::PointingHandCursor);
    back->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #999999; border: none;"
        "  font-size: 14px; padding: 8px 6px;"
        "}"
        "QPushButton:hover { color: #ffffff; }"));
    connect(back, &QPushButton::clicked, this, &ScriptEditorPage::backRequested);
    layout->addWidget(back);

    layout->addStretch(1);

    // Parse Script (center) — LOCAL parsing only, never the API.
    m_parseButton = new QPushButton(QStringLiteral("Parse Script"));
    m_parseButton->setCursor(Qt::PointingHandCursor);
    m_parseButton->setStyleSheet(QString::fromUtf8(kAmberButton));
    connect(m_parseButton, &QPushButton::clicked, this, &ScriptEditorPage::onParseClicked);
    layout->addWidget(m_parseButton);

    layout->addStretch(1);

    // Skip (right group) — straight to the Storyboard with one blank scene;
    // no parsing, no API. Secondary "ghost" style so Continue stays primary.
    m_skipButton = new QPushButton(QStringLiteral("Skip"));
    m_skipButton->setCursor(Qt::PointingHandCursor);
    m_skipButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        "  border-radius: 6px; padding: 9px 22px; font-size: 14px; font-weight: 600;"
        "}"
        "QPushButton:hover { color: #ffffff; border-color: #444444; }"
        "QPushButton:pressed { background-color: #1a1a1a; }"));
    connect(m_skipButton, &QPushButton::clicked, this, &ScriptEditorPage::onSkipClicked);
    layout->addWidget(m_skipButton);

    // Continue to Storyboard (right) — disabled until at least one scene parsed.
    m_continueButton = new QPushButton(QStringLiteral("Continue to Storyboard"));
    m_continueButton->setCursor(Qt::PointingHandCursor);
    connect(m_continueButton, &QPushButton::clicked, this,
            [this] { emit continueRequested(); });
    layout->addWidget(m_continueButton);
    setContinueEnabled(false);

    return bar;
}

void ScriptEditorPage::setContinueEnabled(bool enabled)
{
    m_continueButton->setEnabled(enabled);
    if (enabled) {
        m_continueButton->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: #ffffff; color: #0a0a0a; border: none; border-radius: 6px;"
            "  padding: 9px 22px; font-size: 14px; font-weight: 600;"
            "}"
            "QPushButton:hover { background-color: #e6e6e6; }"));
    } else {
        m_continueButton->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: #1c1c1c; color: #555555; border: 1px solid #2a2a2a;"
            "  border-radius: 6px; padding: 9px 22px; font-size: 14px; font-weight: 600;"
            "}"));
    }
}

void ScriptEditorPage::onParseClicked()
{
    const QString script = m_editor->toPlainText().trimmed();
    if (script.isEmpty()) {
        showBreakdownMessage(QStringLiteral("Write some script text first."),
                             QStringLiteral("#999999"));
        return;
    }

    // Parse entirely on-device — no network request, no API key.
    populateScenes(buildSceneBreakdown(script));
}

void ScriptEditorPage::onSkipClicked()
{
    // No parsing, no API: hand the Storyboard a single blank scene so the
    // artist can start boarding immediately.
    QJsonObject scene;
    scene[QStringLiteral("scene_number")] = 1;
    scene[QStringLiteral("location")] = QStringLiteral("UNTITLED");
    scene[QStringLiteral("time_of_day")] = QStringLiteral("UNSPECIFIED");
    scene[QStringLiteral("action")] = QString();

    m_scenes = QJsonArray{ scene };
    emit scenesReady(m_scenes); // MainWindow materializes Scene 1 (blank panel)
    emit continueRequested();   // straight to the Storyboard, no breakdown
}

void ScriptEditorPage::clearScenes()
{
    if (!m_scenesLayout)
        return;
    while (QLayoutItem *item = m_scenesLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void ScriptEditorPage::showBreakdownMessage(const QString &text, const QString &color)
{
    clearScenes();
    QLabel *label = new QLabel(text);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(color));
    m_scenesLayout->addStretch(1);
    m_scenesLayout->addWidget(label);
    m_scenesLayout->addStretch(1);
}

void ScriptEditorPage::populateScenes(const QJsonArray &scenes)
{
    m_scenes = scenes; // remembered for the Continue-to-Storyboard handoff
    if (!scenes.isEmpty())
        emit scenesReady(scenes); // let MainWindow materialize Scene objects now
    clearScenes();

    if (scenes.isEmpty()) {
        QLabel *empty = new QLabel(QStringLiteral("No scenes were found in the script."));
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("color: #999999; font-size: 13px;"));
        m_scenesLayout->addWidget(empty);
        m_scenesLayout->addStretch(1);
        setContinueEnabled(false);
        return;
    }

    for (const QJsonValue &value : scenes)
        m_scenesLayout->addWidget(createSceneCard(value.toObject()));
    m_scenesLayout->addStretch(1);

    setContinueEnabled(true);
}

QWidget *ScriptEditorPage::createSceneCard(const QJsonObject &scene)
{
    const int number = scene.value(QStringLiteral("scene_number")).toInt();
    const QString location = scene.value(QStringLiteral("location")).toString();
    const QString timeOfDay = scene.value(QStringLiteral("time_of_day")).toString();
    const QString action = scene.value(QStringLiteral("action")).toString();

    QFrame *card = new QFrame;
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #161616; border: 1px solid #2a2a2a; border-radius: 8px; }"));

    QVBoxLayout *layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);

    // Top row: scene number (amber) + location (white) + time of day (grey).
    QHBoxLayout *topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    QLabel *sceneNo = new QLabel(QStringLiteral("SCENE %1").arg(number));
    sceneNo->setStyleSheet(QStringLiteral(
        "color: #f5a623; font-size: 12px; font-weight: 700; border: none;"));
    topRow->addWidget(sceneNo);

    QLabel *loc = new QLabel(location);
    loc->setStyleSheet(QStringLiteral(
        "color: #ffffff; font-size: 13px; font-weight: 600; border: none;"));
    topRow->addWidget(loc, 1);

    QLabel *tod = new QLabel(timeOfDay);
    tod->setStyleSheet(QStringLiteral("color: #888888; font-size: 12px; border: none;"));
    topRow->addWidget(tod);

    layout->addLayout(topRow);

    // Action summary.
    QLabel *summary = new QLabel(action);
    summary->setWordWrap(true);
    summary->setStyleSheet(QStringLiteral("color: #bbbbbb; font-size: 12px; border: none;"));
    layout->addWidget(summary);

    return card;
}
