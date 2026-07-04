#include "GenerationPage.h"

#include "StoryboardModel.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVideoWidget>

namespace {

// fal.ai / Seedance 2.0 endpoints (verified paths).
const QString kFalTextToVideo =
    QStringLiteral("https://fal.run/bytedance/seedance-2.0/text-to-video");
const QString kFalImageToVideo =
    QStringLiteral("https://fal.run/bytedance/seedance-2.0/image-to-video");
const QString kFalRequestsBase =
    QStringLiteral("https://fal.run/bytedance/seedance-2.0/requests/");

QString badgeStyle(const QString &status)
{
    if (status == QLatin1String("Queued"))
        return QStringLiteral("color:#5a9bff; border:1px solid #5a9bff; background:transparent;");
    if (status == QLatin1String("Generating"))
        return QStringLiteral("color:#0a0a0a; background:#f5a623; border:1px solid #f5a623;");
    if (status == QLatin1String("Complete"))
        return QStringLiteral("color:#0a0a0a; background:#4dff91; border:1px solid #4dff91;");
    if (status == QLatin1String("Failed"))
        return QStringLiteral("color:#ffffff; background:#e0504a; border:1px solid #e0504a;");
    // Not Queued.
    return QStringLiteral("color:#888888; border:1px solid #3a3a3a; background:transparent;");
}

// Pull a video URL out of a fal result/status object, trying common shapes.
QString extractVideoUrl(const QJsonObject &obj)
{
    const QJsonValue video = obj.value(QStringLiteral("video"));
    if (video.isObject()) {
        const QString u = video.toObject().value(QStringLiteral("url")).toString();
        if (!u.isEmpty()) return u;
    }
    if (video.isString() && !video.toString().isEmpty())
        return video.toString();
    const QString vu = obj.value(QStringLiteral("video_url")).toString();
    if (!vu.isEmpty()) return vu;
    const QString u = obj.value(QStringLiteral("url")).toString();
    if (!u.isEmpty()) return u;
    // Nested under "response" (fal sometimes wraps results).
    const QJsonValue resp = obj.value(QStringLiteral("response"));
    if (resp.isObject())
        return extractVideoUrl(resp.toObject());
    return QString();
}

} // namespace

// =====================================================================
// GenSpinner
// =====================================================================

GenSpinner::GenSpinner(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(16, 16);
    m_anim = new QPropertyAnimation(this, QByteArrayLiteral("angle"), this);
    m_anim->setStartValue(0.0);
    m_anim->setEndValue(360.0);
    m_anim->setDuration(900);
    m_anim->setLoopCount(-1); // forever
}

void GenSpinner::setAngle(qreal a)
{
    m_angle = a;
    update();
}

void GenSpinner::start()
{
    if (m_anim->state() != QPropertyAnimation::Running)
        m_anim->start();
}

void GenSpinner::stop()
{
    m_anim->stop();
}

void GenSpinner::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRectF r = rect().adjusted(2, 2, -2, -2);
    QPen pen(QColor("#f5a623"), 2);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    // A 270-degree arc rotated by the animated angle (Qt uses 1/16 degrees).
    p.drawArc(r, static_cast<int>(-m_angle * 16), 270 * 16);
}

// =====================================================================
// GenerationPage
// =====================================================================

GenerationPage::GenerationPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    m_net = new QNetworkAccessManager(this);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(createTopBar());
    root->addWidget(createQueueArea(), 1);
}

void GenerationPage::setConsistencyEntries(const QVector<ConsistencyEntry> *entries)
{
    m_entries = entries;
}

void GenerationPage::setProjectDir(const QString &dir)
{
    m_projectDir = dir;
}

void GenerationPage::loadScenes(const QVector<Scene *> &scenes)
{
    m_scenes = scenes;
    rebuildRows();
}

// --- Top bar --------------------------------------------------------------

QWidget *GenerationPage::createTopBar()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(60);
    bar->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);

    QPushButton *back = new QPushButton(QString::fromUtf8("\xE2\x86\x90"));
    back->setCursor(Qt::PointingHandCursor);
    back->setToolTip(QStringLiteral("Back to Animatic"));
    back->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: none; font-size: 20px;"
        " padding: 4px 8px; } QPushButton:hover { color: #ffffff; }"));
    connect(back, &QPushButton::clicked, this, [this] { emit backRequested(); });
    layout->addWidget(back);

    layout->addStretch(1);

    QLabel *title = new QLabel(QStringLiteral("Generation"));
    title->setStyleSheet(QStringLiteral("color: #ffffff; font-size: 15px; font-weight: 600;"));
    layout->addWidget(title);

    layout->addStretch(1);

    m_sessionCostLabel = new QLabel(QStringLiteral("Session cost: ~$0.00"));
    m_sessionCostLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 12px;"));
    layout->addWidget(m_sessionCostLabel);
    layout->addSpacing(12);

    QPushButton *genAll = new QPushButton(QStringLiteral("Generate All Queued"));
    genAll->setCursor(Qt::PointingHandCursor);
    genAll->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #7c6ef6; color: #ffffff; border: none;"
        " border-radius: 6px; padding: 8px 16px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background-color: #8f82ff; }"));
    connect(genAll, &QPushButton::clicked, this, &GenerationPage::generateAllQueued);
    layout->addWidget(genAll);

    return bar;
}

// --- Queue list -----------------------------------------------------------

QWidget *GenerationPage::createQueueArea()
{
    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: #0a0a0a; border: none; }"));

    QWidget *container = new QWidget;
    container->setStyleSheet(QStringLiteral("background: #0a0a0a;"));
    m_rowsLayout = new QVBoxLayout(container);
    m_rowsLayout->setContentsMargins(16, 16, 16, 16);
    m_rowsLayout->setSpacing(8);
    m_rowsLayout->addStretch(1);

    scroll->setWidget(container);
    return scroll;
}

void GenerationPage::rebuildRows()
{
    // Clear existing row widgets (keep the trailing stretch).
    for (const Row &r : m_rows) {
        if (r.widget)
            r.widget->deleteLater();
    }
    m_rows.clear();

    for (int si = 0; si < m_scenes.size(); ++si) {
        Scene *scene = m_scenes.at(si);
        for (int pi = 0; pi < scene->panels.size(); ++pi) {
            Row row;
            row.scene = scene;
            row.panel = scene->panels.at(pi);
            row.sceneIndex = si;
            row.panelIndex = pi;
            m_rows.append(row);
        }
    }

    for (int i = 0; i < m_rows.size(); ++i) {
        QWidget *w = buildRow(i);
        m_rows[i].widget = w;
        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, w); // before the stretch
    }

    if (m_rows.isEmpty()) {
        QLabel *empty = new QLabel(QStringLiteral("No panels to generate."));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("color: #555555; font-size: 13px; padding: 40px;"));
        m_rowsLayout->insertWidget(0, empty);
    }
}

QWidget *GenerationPage::buildRow(int index)
{
    Row &row = m_rows[index];

    // Outer container: the shot row, plus a takes filmstrip beneath it.
    QWidget *w = new QWidget;
    w->setAttribute(Qt::WA_StyledBackground, true);
    QVBoxLayout *outer = new QVBoxLayout(w);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    QWidget *mainRow = new QWidget;
    row.mainRow = mainRow;
    mainRow->setAttribute(Qt::WA_StyledBackground, true);
    mainRow->setFixedHeight(90);
    mainRow->setStyleSheet(QStringLiteral(
        "background-color: #121212; border: 1px solid #1f1f1f; border-radius: 6px;"));
    outer->addWidget(mainRow);

    QHBoxLayout *hl = new QHBoxLayout(mainRow);
    hl->setContentsMargins(10, 10, 10, 10);
    hl->setSpacing(12);

    // Thumbnail (120x68).
    QLabel *thumb = new QLabel;
    thumb->setFixedSize(120, 68);
    thumb->setStyleSheet(QStringLiteral("border: 1px solid #2a2a2a; border-radius: 3px;"));
    thumb->setScaledContents(false);
    thumb->setAlignment(Qt::AlignCenter);
    QPixmap pm = row.panel->flattenedPixmap().scaled(120, 68, Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation);
    thumb->setPixmap(pm);
    hl->addWidget(thumb);

    // Metadata column.
    QWidget *meta = new QWidget;
    meta->setStyleSheet(QStringLiteral("border: none;"));
    QVBoxLayout *ml = new QVBoxLayout(meta);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(3);
    QLabel *titleLbl = new QLabel(QStringLiteral("Scene %1  \xC2\xB7  Panel %2")
                                      .arg(row.scene->number)
                                      .arg(row.panelIndex + 1));
    titleLbl->setStyleSheet(QStringLiteral("color: #ffffff; font-size: 13px; font-weight: 600; border:none;"));
    ml->addWidget(titleLbl);

    QStringList parts;
    if (!row.panel->shotType.isEmpty()) parts << row.panel->shotType;
    if (!row.panel->cameraAngle.isEmpty()) parts << row.panel->cameraAngle;
    if (!row.panel->mood.isEmpty()) parts << row.panel->mood;
    QLabel *summary = new QLabel(parts.join(QStringLiteral("  \xC2\xB7  ")));
    summary->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px; border:none;"));
    ml->addWidget(summary);

    row.promptLink = new QLabel(QStringLiteral("<a href=\"#\" style=\"color:#555555;\">View Prompt</a>"));
    row.promptLink->setStyleSheet(QStringLiteral("font-size: 10px; border:none;"));
    row.promptLink->setVisible(false);
    connect(row.promptLink, &QLabel::linkActivated, this, [this, index] {
        QMessageBox::information(this, QStringLiteral("Generation Prompt"),
                                 m_rows.at(index).prompt);
    });
    ml->addWidget(row.promptLink);
    ml->addStretch(1);

    hl->addWidget(meta, 1);

    // Spinner (visible only while generating).
    row.spinner = new GenSpinner;
    row.spinner->setVisible(false);
    hl->addWidget(row.spinner);

    // Auto-retry suffix, shown beside the spinner during a retry.
    row.retryLabel = new QLabel;
    row.retryLabel->setStyleSheet(QStringLiteral("color: #f5a623; font-size: 10px; border:none;"));
    row.retryLabel->setVisible(false);
    hl->addWidget(row.retryLabel);

    // Status badge.
    row.badge = new QLabel;
    row.badge->setAlignment(Qt::AlignCenter);
    row.badge->setFixedHeight(24);
    row.badge->setMinimumWidth(86);
    hl->addWidget(row.badge);

    // Preview (Complete) / Retry (Failed) inline actions.
    row.previewBtn = new QPushButton(QString::fromUtf8("\xE2\x96\xB6 Preview"));
    row.previewBtn->setCursor(Qt::PointingHandCursor);
    row.previewBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #4dff91; border: 1px solid #4dff91;"
        " border-radius: 4px; padding: 4px 10px; font-size: 11px; }"
        "QPushButton:hover { background: #142a1e; }"));
    row.previewBtn->setVisible(false);
    connect(row.previewBtn, &QPushButton::clicked, this, [this, index] { openPreview(index); });
    hl->addWidget(row.previewBtn);

    // Per-shot cost estimate, shown once Complete.
    row.costLabel = new QLabel;
    row.costLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px; border:none;"));
    row.costLabel->setVisible(false);
    hl->addWidget(row.costLabel);

    row.retryBtn = new QPushButton(QStringLiteral("Retry"));
    row.retryBtn->setCursor(Qt::PointingHandCursor);
    row.retryBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #e0504a; border: 1px solid #e0504a;"
        " border-radius: 4px; padding: 4px 10px; font-size: 11px; }"
        "QPushButton:hover { background: #2a1414; }"));
    row.retryBtn->setVisible(false);
    connect(row.retryBtn, &QPushButton::clicked, this, [this, index] { queueRow(index); });
    hl->addWidget(row.retryBtn);

    // Per-row Generate button.
    row.generateBtn = new QPushButton(QStringLiteral("Generate"));
    row.generateBtn->setCursor(Qt::PointingHandCursor);
    row.generateBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #7c6ef6; color: #ffffff; border: none;"
        " border-radius: 4px; padding: 5px 14px; font-size: 12px; font-weight: 600; }"
        "QPushButton:hover { background-color: #8f82ff; }"));
    connect(row.generateBtn, &QPushButton::clicked, this, [this, index] { queueRow(index); });
    hl->addWidget(row.generateBtn);

    // Takes filmstrip beneath the row (hidden until the panel has takes).
    row.takesStrip = new QWidget;
    row.takesStrip->setStyleSheet(QStringLiteral("background: transparent;"));
    row.takesLayout = new QHBoxLayout(row.takesStrip);
    row.takesLayout->setContentsMargins(12, 2, 12, 6);
    row.takesLayout->setSpacing(6);
    row.takesLayout->addStretch(1);
    outer->addWidget(row.takesStrip);

    rebuildTakesStrip(index);
    refreshRow(index);
    return w;
}

void GenerationPage::refreshRow(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &row = m_rows[index];
    const QString status = row.panel->generationStatus;

    if (row.badge) {
        row.badge->setText(status);
        row.badge->setStyleSheet(
            QStringLiteral("QLabel { %1 border-radius: 12px; font-size: 11px; padding: 0 10px; }")
                .arg(badgeStyle(status)));
        row.badge->setToolTip(status == QLatin1String("Failed") ? row.errorMessage : QString());
    }

    const bool generating = (status == QLatin1String("Generating"));
    if (row.spinner) {
        row.spinner->setVisible(generating);
        if (generating) row.spinner->start();
        else row.spinner->stop();
    }
    if (row.retryLabel) {
        const bool showRetry = generating && row.retryCount > 0;
        row.retryLabel->setVisible(showRetry);
        if (showRetry)
            row.retryLabel->setText(QStringLiteral("(retry %1/2)").arg(row.retryCount));
    }
    // Cost label: total across ALL of this row's takes.
    const double cost = rowTakesCost(row.panel);
    if (row.costLabel) {
        row.costLabel->setVisible(cost > 0.0);
        if (cost > 0.0)
            row.costLabel->setText(QStringLiteral("~$%1").arg(cost, 0, 'f', 2));
    }

    // Main-row Preview shows when the SELECTED take is complete.
    bool selectedComplete = false;
    for (const GeneratedTake &t : row.panel->takes) {
        if (t.id == row.panel->selectedTakeId && t.status == QLatin1String("Complete")) {
            selectedComplete = true;
            break;
        }
    }
    if (row.previewBtn)
        row.previewBtn->setVisible(selectedComplete);

    // The standalone Retry button is superseded by "Generate Another Take".
    if (row.retryBtn)
        row.retryBtn->setVisible(false);

    // Generate / Generate Another Take: available whenever not mid-generation.
    if (row.generateBtn) {
        const bool hasTakes = !row.panel->takes.isEmpty();
        row.generateBtn->setText(hasTakes ? QStringLiteral("Generate Another Take")
                                          : QStringLiteral("Generate"));
        const bool busy = (status == QLatin1String("Generating")
                           || status == QLatin1String("Queued"));
        row.generateBtn->setVisible(!busy);
    }
    if (row.promptLink)
        row.promptLink->setVisible(!row.prompt.isEmpty());
}

void GenerationPage::rebuildTakesStrip(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &row = m_rows[index];
    if (!row.takesLayout || !row.takesStrip)
        return;

    // Clear existing chips (leave the trailing stretch to be re-added).
    while (QLayoutItem *item = row.takesLayout->takeAt(0)) {
        if (QWidget *wdg = item->widget())
            wdg->deleteLater();
        delete item;
    }

    const QVector<GeneratedTake> &takes = row.panel->takes;
    row.takesStrip->setVisible(!takes.isEmpty());
    if (takes.isEmpty()) {
        row.takesLayout->addStretch(1);
        return;
    }

    for (int i = 0; i < takes.size(); ++i) {
        const GeneratedTake &t = takes.at(i);
        const bool selected = (t.id == row.panel->selectedTakeId);
        const bool complete = (t.status == QLatin1String("Complete"));
        const QString takeId = t.id;

        QWidget *chip = new QWidget;
        chip->setAttribute(Qt::WA_StyledBackground, true);
        chip->setStyleSheet(QStringLiteral(
            "background-color: #161616; border: 1px solid %1; border-radius: 3px;")
            .arg(selected ? QStringLiteral("#f5a623") : QStringLiteral("#2a2a2a")));
        QHBoxLayout *cl = new QHBoxLayout(chip);
        cl->setContentsMargins(6, 2, 6, 2);
        cl->setSpacing(4);

        // Dot: green = complete, red = failed, amber = still generating.
        const QString dotColor = complete
            ? QStringLiteral("#4dff91")
            : (t.status == QLatin1String("Failed") ? QStringLiteral("#e0504a")
                                                   : QStringLiteral("#f5a623"));
        QLabel *dot = new QLabel(QString::fromUtf8("\xE2\x97\x8F")); // filled dot
        dot->setStyleSheet(QStringLiteral("color: %1; font-size: 8px; border: none;")
                               .arg(dotColor));
        cl->addWidget(dot);

        // "Take N" doubles as the select action for the chip.
        QPushButton *sel = new QPushButton(QStringLiteral("Take %1").arg(i + 1));
        sel->setCursor(Qt::PointingHandCursor);
        sel->setStyleSheet(QStringLiteral(
            "QPushButton { color: #cccccc; background: transparent; border: none;"
            " font-size: 7pt; } QPushButton:hover { color: #ffffff; }"));
        connect(sel, &QPushButton::clicked, this,
                [this, index, takeId] { selectTake(index, takeId); });
        cl->addWidget(sel);

        if (complete) {
            QPushButton *pv = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"));
            pv->setCursor(Qt::PointingHandCursor);
            pv->setToolTip(QStringLiteral("Preview this take"));
            pv->setStyleSheet(QStringLiteral(
                "QPushButton { color: #4dff91; background: transparent; border: none;"
                " font-size: 7pt; } QPushButton:hover { color: #8f82ff; }"));
            connect(pv, &QPushButton::clicked, this,
                    [this, index, takeId] { previewTake(index, takeId); });
            cl->addWidget(pv);
        }

        QPushButton *del = new QPushButton(QString::fromUtf8("\xC3\x97")); // times
        del->setCursor(Qt::PointingHandCursor);
        del->setToolTip(QStringLiteral("Delete this take"));
        del->setStyleSheet(QStringLiteral(
            "QPushButton { color: #888888; background: transparent; border: none;"
            " font-size: 9pt; } QPushButton:hover { color: #e0504a; }"));
        connect(del, &QPushButton::clicked, this,
                [this, index, takeId] { deleteTake(index, takeId); });
        cl->addWidget(del);

        row.takesLayout->addWidget(chip);
    }
    row.takesLayout->addStretch(1);
}

// --- Queue processing -----------------------------------------------------

bool GenerationPage::ensureFalKey()
{
    if (qgetenv("FAL_API_KEY").isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("FAL_API_KEY not set"),
            QStringLiteral(
                "The FAL_API_KEY environment variable is not set.\n\n"
                "Set it to your fal.ai key and restart SankoTV before generating "
                "video clips."));
        return false;
    }
    return true;
}

void GenerationPage::queueRow(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    if (!ensureFalKey())
        return;
    m_rows[index].errorMessage.clear();
    m_rows[index].retryCount = 0; // a manual Generate/Retry resets the auto-retry budget
    m_rows[index].panel->generationStatus = QStringLiteral("Queued");
    refreshRow(index);
    processQueue();
}

void GenerationPage::generateAllQueued()
{
    if (!ensureFalKey())
        return;
    processQueue();
}

void GenerationPage::processQueue()
{
    if (m_processing != -1)
        return; // already busy; one job at a time
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &row = m_rows[i];
        if (row.panel->generationStatus == QLatin1String("Queued")) {
            m_processing = i;
            row.panel->generationStatus = QStringLiteral("Generating");

            // A generation appends a NEW take (it never overwrites). The take's
            // own file lives at generated_s{}_p{}_take{N}.mp4.
            GeneratedTake take;
            take.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const int n = nextTakeNumber(row.panel);
            take.videoPath = QStringLiteral("generated_s%1_p%2_take%3.mp4")
                                 .arg(row.sceneIndex).arg(row.panelIndex).arg(n);
            take.status = QStringLiteral("Generating");
            row.panel->takes.append(take);
            row.activeTakeId = take.id;

            refreshRow(i);
            rebuildTakesStrip(i);
            startClaudePrompt(i);
            return;
        }
    }
    m_processing = -1; // nothing left queued
}

void GenerationPage::finishRow(int index)
{
    Q_UNUSED(index);
    m_processing = -1;
    processQueue(); // pick up the next queued row, if any
}

double GenerationPage::clipCost(int durationSeconds)
{
    // Flat fal.ai 720p estimate: 5s clip ~ $0.05, 10s clip ~ $0.10.
    const int clamped = (qBound(1, durationSeconds, 30) <= 7) ? 5 : 10;
    return clamped == 5 ? 0.05 : 0.10;
}

QString GenerationPage::findFfmpeg()
{
    // Same lookup order as the Animatic's Export MP4: exe folder, then PATH.
    const QString local =
        QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg.exe");
    if (QFile::exists(local))
        return local;
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

void GenerationPage::updateSessionCost()
{
    if (m_sessionCostLabel)
        m_sessionCostLabel->setText(
            QStringLiteral("Session cost: ~$%1").arg(m_sessionCost, 0, 'f', 2));
}

// --- Claude prompt building ----------------------------------------------

QString GenerationPage::buildClaudeRequestBody(int index) const
{
    const Row &row = m_rows.at(index);
    const Panel *panel = row.panel;
    const Scene *scene = row.scene;

    // The storyboard drawing is attached to the request (and described as the
    // action source) only when the panel actually has artwork.
    const bool attachImage = !isBlankPixmap(panel->flattenedPixmap());

    QString userContent = QStringLiteral(
        "Write a single optimized text-to-video generation prompt for an AI video "
        "model describing this shot. Use clear, concrete visual and cinematic "
        "language. Keep it under 200 words. Return ONLY the prompt text, with no "
        "preamble, labels, or quotation marks.\n\n");

    // 1. Storyboard drawing = SOURCE OF TRUTH for the action (image attached below).
    if (attachImage) {
        userContent += QStringLiteral(
            "You are looking at a hand-drawn storyboard panel (attached as an image). "
            "This drawing is the SOURCE OF TRUTH for what physically happens in this "
            "shot - the pose, action, composition, character positions, and camera "
            "framing shown in the drawing must be described accurately in your "
            "generated prompt. Study the drawing carefully: What is each character "
            "doing? What direction are they facing or moving? What is their physical "
            "relationship to other elements in the frame (jumping over something, "
            "reaching toward something, etc)? Describe this specific action precisely "
            "- do not default to a generic or passive description of the scene.\n\n");
    }

    // Full Consistency Board (no name matching) - Claude decides relevance.
    const int entryCount = m_entries ? m_entries->size() : 0;
    if (entryCount > 0) {
        userContent += QStringLiteral(
            "CONSISTENCY BOARD - characters and locations in this project:\n");
        for (int i = 0; i < entryCount; ++i) {
            const ConsistencyEntry &e = m_entries->at(i);
            userContent += QStringLiteral("%1. [%2] %3 - %4\n")
                               .arg(i + 1)
                               .arg(e.type, e.name,
                                    e.description.isEmpty()
                                        ? QStringLiteral("(no description)")
                                        : e.description);
        }
        userContent += QStringLiteral("\n");
    }

    // Current shot context.
    userContent += QStringLiteral(
        "SHOT METADATA:\n"
        "- Shot type: %1\n- Camera angle: %2\n- Lens: %3\n- Mood: %4\n- Notes: %5\n\n"
        "SCENE:\n- Location: %6\n- Action: %7\n\n")
        .arg(panel->shotType, panel->cameraAngle, panel->lens,
             panel->mood.isEmpty() ? QStringLiteral("(unspecified)") : panel->mood,
             panel->notes.isEmpty() ? QStringLiteral("(none)") : panel->notes)
        .arg(scene->location.isEmpty() ? QStringLiteral("(unspecified)") : scene->location,
             scene->action.isEmpty() ? QStringLiteral("(none)") : scene->action);

    // 2. Consistency Board = SOURCE OF TRUTH for visual design.
    if (entryCount > 0) {
        userContent += QStringLiteral(
            "Identify which of the above Consistency Board entries (if any) are "
            "relevant to this specific shot, based on the storyboard drawing, scene "
            "action, location, and notes. Only reference entries that actually apply "
            "to what's happening in this panel; omit irrelevant entries entirely.\n\n"
            "For any character or location identified above as relevant to this shot, "
            "their description from the Consistency Board is the SOURCE OF TRUTH for "
            "visual design - exact proportions, colours, textures, and distinguishing "
            "features. The storyboard drawing shows WHERE and HOW they move; the "
            "Consistency Board shows WHAT they look like. Combine both: describe the "
            "action and composition from the drawing, using the exact character "
            "design from the Consistency Board. Do not let the drawing's rough sketch "
            "style override the Consistency Board's specific design details - the "
            "drawing is for action and layout only, not final visual style. Do not "
            "invent details that contradict the Consistency Board.\n\n");
    }

    // 3. Explicit structure for the final prompt.
    userContent += QStringLiteral(
        "Structure your output prompt in this order:\n"
        "a) The specific action, pose, and movement shown in the storyboard.\n"
        "b) The specific character design from the Consistency Board for each "
        "character present.\n"
        "c) The location/environment design from the Consistency Board, if a "
        "location entry is relevant.\n"
        "d) Camera and shot framing from the panel metadata.\n"
        "e) Style and mood.\n");

    // 4. Attach the storyboard image (vision) only when the panel is not blank.
    QJsonObject userMessage;
    userMessage[QStringLiteral("role")] = QStringLiteral("user");
    if (attachImage) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        panel->flattenedPixmap().save(&buffer, "PNG");
        buffer.close();

        QJsonObject source;
        source[QStringLiteral("type")] = QStringLiteral("base64");
        source[QStringLiteral("media_type")] = QStringLiteral("image/png");
        source[QStringLiteral("data")] = QString::fromLatin1(png.toBase64());

        QJsonObject imageBlock;
        imageBlock[QStringLiteral("type")] = QStringLiteral("image");
        imageBlock[QStringLiteral("source")] = source;

        QJsonObject textBlock;
        textBlock[QStringLiteral("type")] = QStringLiteral("text");
        textBlock[QStringLiteral("text")] = userContent;

        userMessage[QStringLiteral("content")] = QJsonArray{ imageBlock, textBlock };
    } else {
        userMessage[QStringLiteral("content")] = userContent; // text only
    }

    QJsonObject body;
    body[QStringLiteral("model")] = QStringLiteral("claude-sonnet-4-6");
    body[QStringLiteral("max_tokens")] = 600;
    body[QStringLiteral("system")] = QStringLiteral(
        "You are a cinematographer writing prompts for an AI video generation model. "
        "You read hand-drawn storyboards for action and composition, and a "
        "Consistency Board for exact character and location design, then translate "
        "them into vivid, concise visual prompts.");
    body[QStringLiteral("messages")] = QJsonArray{ userMessage };
    return QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void GenerationPage::startClaudePrompt(int index)
{
    const QByteArray apiKey = qgetenv("ANTHROPIC_API_KEY");
    if (apiKey.isEmpty()) {
        failRow(index, QStringLiteral("ANTHROPIC_API_KEY is not set; cannot build the prompt."),
                /*retryable=*/false);
        return;
    }

    QNetworkRequest request(QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("x-api-key"), apiKey);
    request.setRawHeader(QByteArrayLiteral("anthropic-version"), QByteArrayLiteral("2023-06-01"));

    QNetworkReply *reply = m_net->post(request, buildClaudeRequestBody(index).toUtf8());
    connect(reply, &QNetworkReply::finished, this,
            [this, index, reply] { onClaudePromptReply(index, reply); });
}

void GenerationPage::onClaudePromptReply(int index, QNetworkReply *reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();

    if (reply->error() != QNetworkReply::NoError) {
        QString message = reply->errorString();
        const QJsonDocument errDoc = QJsonDocument::fromJson(data);
        if (errDoc.isObject()) {
            const QString apiMsg = errDoc.object().value(QStringLiteral("error"))
                                       .toObject().value(QStringLiteral("message")).toString();
            if (!apiMsg.isEmpty())
                message = apiMsg;
        }
        failRow(index, QStringLiteral("Prompt build failed: ") + message);
        return;
    }

    QString text;
    const QJsonArray blocks = QJsonDocument::fromJson(data).object()
                                  .value(QStringLiteral("content")).toArray();
    for (const QJsonValue &v : blocks) {
        const QJsonObject block = v.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            text += block.value(QStringLiteral("text")).toString();
    }
    text = text.trimmed();
    if (text.isEmpty()) {
        failRow(index, QStringLiteral("Claude returned an empty prompt."));
        return;
    }

    m_rows[index].prompt = text; // kept hidden; available via the View Prompt link
    refreshRow(index);
    callFal(index);
}

// --- fal.ai / Seedance ----------------------------------------------------

bool GenerationPage::isBlankPixmap(const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return true;
    const QImage img = pixmap.toImage()
                           .scaled(32, 18, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
    int nonWhite = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = img.pixel(x, y);
            if (qRed(px) < 244 || qGreen(px) < 244 || qBlue(px) < 244)
                ++nonWhite;
        }
    }
    return nonWhite < 6; // essentially an untouched white canvas
}

void GenerationPage::callFal(int index)
{
    const QByteArray falKey = qgetenv("FAL_API_KEY");
    if (falKey.isEmpty()) {
        failRow(index, QStringLiteral("FAL_API_KEY is not set."), /*retryable=*/false);
        return;
    }

    Row &row = m_rows[index];
    const int clampedDuration = (qBound(1, row.panel->duration, 30) <= 7) ? 5 : 10;
    const bool useImage = !isBlankPixmap(row.panel->flattenedPixmap());

    QJsonObject body;
    body[QStringLiteral("prompt")] = row.prompt;
    body[QStringLiteral("duration")] = QString::number(clampedDuration); // string per fal.ai
    body[QStringLiteral("resolution")] = QStringLiteral("720p");
    body[QStringLiteral("aspect_ratio")] = QStringLiteral("16:9");

    QString endpoint = kFalTextToVideo;
    if (useImage) {
        // fal.ai accepts a data URI for image inputs; inline the panel artwork.
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        row.panel->flattenedPixmap().save(&buffer, "PNG");
        buffer.close();
        body[QStringLiteral("image_url")] =
            QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        endpoint = kFalImageToVideo;
    }

    QNetworkRequest request{ QUrl(endpoint) };
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Key ") + falKey);

    QNetworkReply *reply =
        m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, index, reply] { onFalSubmitReply(index, reply); });
}

void GenerationPage::onFalSubmitReply(int index, QNetworkReply *reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();
    const QJsonObject obj = QJsonDocument::fromJson(data).object();

    if (reply->error() != QNetworkReply::NoError) {
        QString message = reply->errorString();
        const QString apiMsg = obj.value(QStringLiteral("detail")).toString();
        if (!apiMsg.isEmpty())
            message = apiMsg;
        failRow(index, QStringLiteral("fal.ai submit failed: ") + message);
        return;
    }

    // Async job: a request_id is returned immediately for polling.
    QString requestId = obj.value(QStringLiteral("request_id")).toString();
    if (requestId.isEmpty())
        requestId = obj.value(QStringLiteral("requestId")).toString();

    if (requestId.isEmpty()) {
        // Some fal endpoints return the result synchronously — handle that too.
        const QString url = extractVideoUrl(obj);
        if (!url.isEmpty()) {
            downloadVideo(index, url);
            return;
        }
        failRow(index, QStringLiteral("fal.ai did not return a request id."));
        return;
    }

    m_rows[index].panel->falRequestId = requestId;
    QTimer::singleShot(3000, this, [this, index] { pollStatus(index); });
}

void GenerationPage::pollStatus(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    const QByteArray falKey = qgetenv("FAL_API_KEY");
    const QString requestId = m_rows.at(index).panel->falRequestId;
    if (requestId.isEmpty()) {
        failRow(index, QStringLiteral("Lost the fal.ai request id."));
        return;
    }

    QNetworkRequest request{ QUrl(kFalRequestsBase + requestId + QStringLiteral("/status")) };
    request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Key ") + falKey);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, index, reply] { onPollReply(index, reply); });
}

void GenerationPage::onPollReply(int index, QNetworkReply *reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();

    if (reply->error() != QNetworkReply::NoError) {
        failRow(index, QStringLiteral("Status poll failed: ") + reply->errorString());
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    const QString status = obj.value(QStringLiteral("status")).toString().toUpper();

    if (status == QLatin1String("COMPLETED")) {
        // The status payload may already carry the result.
        const QString url = extractVideoUrl(obj);
        if (!url.isEmpty())
            downloadVideo(index, url);
        else
            fetchResult(index);
        return;
    }
    if (status == QLatin1String("FAILED") || status == QLatin1String("ERROR")) {
        failRow(index, QStringLiteral("Generation failed on fal.ai."));
        return;
    }
    // IN_QUEUE / IN_PROGRESS / anything else: keep polling.
    QTimer::singleShot(3000, this, [this, index] { pollStatus(index); });
}

void GenerationPage::fetchResult(int index)
{
    const QByteArray falKey = qgetenv("FAL_API_KEY");
    const QString requestId = m_rows.at(index).panel->falRequestId;
    QNetworkRequest request{ QUrl(kFalRequestsBase + requestId) };
    request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Key ") + falKey);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, index, reply] { onResultReply(index, reply); });
}

void GenerationPage::onResultReply(int index, QNetworkReply *reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        failRow(index, QStringLiteral("Result fetch failed: ") + reply->errorString());
        return;
    }
    const QString url = extractVideoUrl(QJsonDocument::fromJson(data).object());
    if (url.isEmpty()) {
        failRow(index, QStringLiteral("No video URL in the fal.ai result."));
        return;
    }
    downloadVideo(index, url);
}

void GenerationPage::downloadVideo(int index, const QString &url)
{
    QNetworkReply *reply = m_net->get(QNetworkRequest{ QUrl(url) });
    connect(reply, &QNetworkReply::finished, this,
            [this, index, reply] { onVideoDownloaded(index, reply); });
}

void GenerationPage::onVideoDownloaded(int index, QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        failRow(index, QStringLiteral("Video download failed: ") + reply->errorString());
        return;
    }
    const QByteArray bytes = reply->readAll();

    Row &row = m_rows[index];

    // Locate the take this download belongs to.
    GeneratedTake *take = nullptr;
    for (GeneratedTake &t : row.panel->takes) {
        if (t.id == row.activeTakeId) { take = &t; break; }
    }
    if (!take) {
        failRow(index, QStringLiteral("Internal error: no active take for this download."));
        return;
    }

    QString dir = m_projectDir;
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/sankotv_generated");
    QDir().mkpath(dir);

    const QString fileName = take->videoPath; // generated_s{}_p{}_take{N}.mp4
    const QString finalPath = dir + QStringLiteral("/") + fileName;
    const QString rawPath = dir + QStringLiteral("/raw_") + fileName;

    // Write the raw download first; it may then be trimmed into finalPath.
    QFile raw(rawPath);
    if (!raw.open(QIODevice::WriteOnly)) {
        failRow(index, QStringLiteral("Could not write the generated video to disk."));
        return;
    }
    raw.write(bytes);
    raw.close();

    // Seedance only renders 5s or 10s. When the panel's actual animatic duration
    // is shorter than what we asked for, trim the clip down (stream copy, no
    // re-encode) so the director's pacing from the Animatic is preserved.
    const int actual = qBound(1, row.panel->duration, 30);
    const int requested = (actual <= 7) ? 5 : 10;

    bool trimmed = false;
    if (actual < requested) {
        const QString ffmpeg = findFfmpeg();
        if (!ffmpeg.isEmpty()) {
            QProcess proc;
            const QStringList args{
                QStringLiteral("-i"), rawPath,
                QStringLiteral("-t"), QString::number(actual),
                QStringLiteral("-c"), QStringLiteral("copy"),
                QStringLiteral("-y"), finalPath };
            proc.start(ffmpeg, args);
            if (proc.waitForStarted(5000) && proc.waitForFinished(30000)
                && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0
                && QFileInfo::exists(finalPath)) {
                trimmed = true;
            }
        }
        // If ffmpeg is missing or the trim fails, fall through and keep the full clip.
    }

    if (trimmed) {
        QFile::remove(rawPath); // keep only the trimmed final clip
    } else {
        // No trim needed (duration >= requested), ffmpeg missing, or trim failed:
        // the raw download becomes the final clip unchanged.
        QFile::remove(finalPath); // clear any stale file at the destination
        if (!QFile::rename(rawPath, finalPath)) {
            QFile::copy(rawPath, finalPath);
            QFile::remove(rawPath);
        }
    }

    // Finalize the take record.
    take->status = QStringLiteral("Complete");
    take->promptUsed = row.prompt;
    take->timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    take->costEstimate = clipCost(row.panel->duration);

    // Cost tracking sums ALL takes (every generation cost money).
    m_sessionCost += take->costEstimate;
    updateSessionCost();

    // Auto-select the first successful take; the director may re-choose later.
    if (row.panel->selectedTakeId.isEmpty())
        row.panel->selectedTakeId = take->id;

    // Keep the generatedVideoPath mirror in sync with the selected take.
    for (const GeneratedTake &t : row.panel->takes) {
        if (t.id == row.panel->selectedTakeId)
            row.panel->generatedVideoPath = t.videoPath;
    }

    row.panel->generationStatus = QStringLiteral("Complete");
    row.retryCount = 0;
    row.activeTakeId.clear();

    refreshRow(index);
    rebuildTakesStrip(index);
    finishRow(index);
}

void GenerationPage::failRow(int index, const QString &message, bool retryable)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &row = m_rows[index];
    row.errorMessage = message;

    // Auto-retry up to twice (5s apart) before giving up. The row stays in the
    // "Generating" state and keeps the queue slot, so processing stays serial.
    if (retryable && row.retryCount < 2) {
        row.retryCount += 1;
        row.panel->generationStatus = QStringLiteral("Generating");
        refreshRow(index); // shows the "(retry N/2)" suffix beside the spinner
        QTimer::singleShot(5000, this, [this, index] {
            if (index >= 0 && index < m_rows.size()
                && m_rows.at(index).panel->generationStatus == QLatin1String("Generating"))
                startClaudePrompt(index);
        });
        return;
    }

    // Retries exhausted: mark the active take (if any) as Failed.
    for (GeneratedTake &t : row.panel->takes) {
        if (t.id == row.activeTakeId) {
            t.status = QStringLiteral("Failed");
            t.timestamp =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            break;
        }
    }
    row.activeTakeId.clear();
    row.panel->generationStatus = QStringLiteral("Failed");
    refreshRow(index);
    rebuildTakesStrip(index);
    finishRow(index);
}

// --- Preview --------------------------------------------------------------

void GenerationPage::openPreview(int index)
{
    // The main-row Preview plays the currently selected take.
    const Row &row = m_rows.at(index);
    previewTake(index, row.panel->selectedTakeId);
}

void GenerationPage::openVideoDialog(const QString &path, const QString &title)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Preview"),
                             QStringLiteral("The generated video file could not be found."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(800, 480);
    dialog.setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);

    QVideoWidget *videoWidget = new QVideoWidget(&dialog);
    layout->addWidget(videoWidget, 1);

    QMediaPlayer *player = new QMediaPlayer(&dialog);
    QAudioOutput *audio = new QAudioOutput(&dialog);
    player->setAudioOutput(audio);
    player->setVideoOutput(videoWidget);
    player->setSource(QUrl::fromLocalFile(path));

    QPushButton *close = new QPushButton(QStringLiteral("Close"));
    close->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #ffffff; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 8px 16px; font-size: 13px; margin: 10px; }"
        "QPushButton:hover { background-color: #262626; }"));
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close, 0, Qt::AlignRight);

    player->play();
    dialog.exec();
    player->stop();
}

// --- Version tree (takes) -------------------------------------------------

int GenerationPage::nextTakeNumber(const Panel *panel)
{
    // Never reuse a number, even across deletes/reloads: max existing + 1.
    int maxN = 0;
    static const QRegularExpression re(QStringLiteral("_take(\\d+)\\.mp4$"));
    for (const GeneratedTake &t : panel->takes) {
        const auto m = re.match(t.videoPath);
        if (m.hasMatch())
            maxN = qMax(maxN, m.captured(1).toInt());
    }
    return maxN + 1;
}

double GenerationPage::rowTakesCost(const Panel *panel) const
{
    double sum = 0.0;
    for (const GeneratedTake &t : panel->takes)
        sum += t.costEstimate;
    return sum;
}

void GenerationPage::previewTake(int index, const QString &takeId)
{
    if (index < 0 || index >= m_rows.size())
        return;
    const Row &row = m_rows.at(index);
    QString videoPath;
    for (const GeneratedTake &t : row.panel->takes) {
        if (t.id == takeId) { videoPath = t.videoPath; break; }
    }
    QString dir = m_projectDir;
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/sankotv_generated");
    openVideoDialog(dir + QStringLiteral("/") + videoPath,
                    QStringLiteral("Preview - Scene %1 Panel %2")
                        .arg(row.scene->number).arg(row.panelIndex + 1));
}

void GenerationPage::selectTake(int index, const QString &takeId)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &row = m_rows[index];
    row.panel->selectedTakeId = takeId;
    for (const GeneratedTake &t : row.panel->takes) {
        if (t.id == takeId)
            row.panel->generatedVideoPath = t.videoPath; // mirror for Export/Save
    }
    refreshRow(index);
    rebuildTakesStrip(index);
}

void GenerationPage::deleteTake(int index, const QString &takeId)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &row = m_rows[index];

    // Remove the take and delete its video file.
    QString dir = m_projectDir;
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/sankotv_generated");
    for (int i = 0; i < row.panel->takes.size(); ++i) {
        if (row.panel->takes.at(i).id == takeId) {
            QFile::remove(dir + QStringLiteral("/") + row.panel->takes.at(i).videoPath);
            row.panel->takes.removeAt(i);
            break;
        }
    }

    // If the deleted take was selected, fall back to the most recent remaining.
    if (row.panel->selectedTakeId == takeId) {
        row.panel->selectedTakeId.clear();
        row.panel->generatedVideoPath.clear();
        for (int i = row.panel->takes.size() - 1; i >= 0; --i) {
            if (row.panel->takes.at(i).status == QLatin1String("Complete")) {
                row.panel->selectedTakeId = row.panel->takes.at(i).id;
                row.panel->generatedVideoPath = row.panel->takes.at(i).videoPath;
                break;
            }
        }
    }

    // Reflect a now-empty version tree in the panel status.
    if (row.panel->takes.isEmpty())
        row.panel->generationStatus = QStringLiteral("Not Queued");

    refreshRow(index);
    rebuildTakesStrip(index);
}
