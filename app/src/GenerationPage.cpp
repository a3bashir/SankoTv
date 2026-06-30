#include "GenerationPage.h"

#include "StoryboardModel.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
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

    QWidget *w = new QWidget;
    w->setAttribute(Qt::WA_StyledBackground, true);
    w->setFixedHeight(90);
    w->setStyleSheet(QStringLiteral(
        "background-color: #121212; border: 1px solid #1f1f1f; border-radius: 6px;"));

    QHBoxLayout *hl = new QHBoxLayout(w);
    hl->setContentsMargins(10, 10, 10, 10);
    hl->setSpacing(12);

    // Thumbnail (120x68).
    QLabel *thumb = new QLabel;
    thumb->setFixedSize(120, 68);
    thumb->setStyleSheet(QStringLiteral("border: 1px solid #2a2a2a; border-radius: 3px;"));
    thumb->setScaledContents(false);
    thumb->setAlignment(Qt::AlignCenter);
    QPixmap pm = row.panel->pixmap.scaled(120, 68, Qt::KeepAspectRatio,
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
    if (row.previewBtn)
        row.previewBtn->setVisible(status == QLatin1String("Complete"));
    if (row.retryBtn)
        row.retryBtn->setVisible(status == QLatin1String("Failed"));
    if (row.generateBtn)
        row.generateBtn->setVisible(status == QLatin1String("Not Queued")
                                    || status == QLatin1String("Failed"));
    if (row.promptLink)
        row.promptLink->setVisible(!row.prompt.isEmpty());
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
        if (m_rows.at(i).panel->generationStatus == QLatin1String("Queued")) {
            m_processing = i;
            m_rows[i].panel->generationStatus = QStringLiteral("Generating");
            refreshRow(i);
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

// --- Claude prompt building ----------------------------------------------

QVector<const ConsistencyEntry *> GenerationPage::matchedEntries(int index) const
{
    QVector<const ConsistencyEntry *> out;
    if (!m_entries)
        return out;
    const Row &row = m_rows.at(index);
    const QString haystack = (row.scene->action + QStringLiteral(" ") + row.panel->notes).toLower();
    for (const ConsistencyEntry &e : *m_entries) {
        if (e.name.isEmpty())
            continue;
        if (haystack.contains(e.name.toLower()))
            out.append(&e);
    }
    return out;
}

QString GenerationPage::buildClaudeRequestBody(int index) const
{
    const Row &row = m_rows.at(index);
    const Panel *panel = row.panel;
    const Scene *scene = row.scene;

    QString consistencyText;
    const auto matches = matchedEntries(index);
    for (const ConsistencyEntry *e : matches) {
        consistencyText += QStringLiteral("- %1 (%2): %3\n")
                               .arg(e->name, e->type, e->description);
    }
    if (consistencyText.isEmpty())
        consistencyText = QStringLiteral("(none matched)\n");

    const QString userContent = QStringLiteral(
        "Write a single optimized text-to-video generation prompt for an AI video "
        "model describing this shot. Use clear, concrete visual and cinematic "
        "language. Keep it under 200 words. Return ONLY the prompt text, with no "
        "preamble, labels, or quotation marks.\n\n"
        "SHOT METADATA:\n"
        "- Shot type: %1\n- Camera angle: %2\n- Lens: %3\n- Mood: %4\n- Notes: %5\n\n"
        "SCENE:\n- Location: %6\n- Action: %7\n\n"
        "CONSISTENCY REFERENCES (incorporate appearance/location details when relevant):\n%8")
        .arg(panel->shotType, panel->cameraAngle, panel->lens,
             panel->mood.isEmpty() ? QStringLiteral("(unspecified)") : panel->mood,
             panel->notes.isEmpty() ? QStringLiteral("(none)") : panel->notes)
        .arg(scene->location.isEmpty() ? QStringLiteral("(unspecified)") : scene->location,
             scene->action.isEmpty() ? QStringLiteral("(none)") : scene->action,
             consistencyText);

    QJsonObject userMessage;
    userMessage[QStringLiteral("role")] = QStringLiteral("user");
    userMessage[QStringLiteral("content")] = userContent;

    QJsonObject body;
    body[QStringLiteral("model")] = QStringLiteral("claude-sonnet-4-6");
    body[QStringLiteral("max_tokens")] = 600;
    body[QStringLiteral("system")] = QStringLiteral(
        "You are a cinematographer writing prompts for an AI video generation model. "
        "You translate storyboard shot metadata into vivid, concise visual prompts.");
    body[QStringLiteral("messages")] = QJsonArray{ userMessage };
    return QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void GenerationPage::startClaudePrompt(int index)
{
    const QByteArray apiKey = qgetenv("ANTHROPIC_API_KEY");
    if (apiKey.isEmpty()) {
        failRow(index, QStringLiteral("ANTHROPIC_API_KEY is not set; cannot build the prompt."));
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
        failRow(index, QStringLiteral("FAL_API_KEY is not set."));
        return;
    }

    Row &row = m_rows[index];
    const int clampedDuration = (qBound(1, row.panel->duration, 30) <= 7) ? 5 : 10;
    const bool useImage = !isBlankPixmap(row.panel->pixmap);

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
        row.panel->pixmap.save(&buffer, "PNG");
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
    QString dir = m_projectDir;
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/sankotv_generated");
    QDir().mkpath(dir);

    const QString fileName =
        QStringLiteral("generated_s%1_p%2.mp4").arg(row.sceneIndex).arg(row.panelIndex);
    QFile file(dir + QStringLiteral("/") + fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        failRow(index, QStringLiteral("Could not write the generated video to disk."));
        return;
    }
    file.write(bytes);
    file.close();

    row.panel->generatedVideoPath = fileName; // relative, like panel PNGs
    row.panel->generationStatus = QStringLiteral("Complete");
    refreshRow(index);
    finishRow(index);
}

void GenerationPage::failRow(int index, const QString &message)
{
    if (index < 0 || index >= m_rows.size())
        return;
    m_rows[index].errorMessage = message;
    m_rows[index].panel->generationStatus = QStringLiteral("Failed");
    refreshRow(index);
    finishRow(index);
}

// --- Preview --------------------------------------------------------------

void GenerationPage::openPreview(int index)
{
    const Row &row = m_rows.at(index);
    QString dir = m_projectDir;
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/sankotv_generated");
    const QString path = dir + QStringLiteral("/") + row.panel->generatedVideoPath;
    if (row.panel->generatedVideoPath.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Preview"),
                             QStringLiteral("The generated video file could not be found."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Preview — Scene %1 Panel %2")
                              .arg(row.scene->number).arg(row.panelIndex + 1));
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
