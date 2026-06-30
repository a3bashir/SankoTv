#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QVBoxLayout;

struct ConsistencyEntry;
struct Panel;
struct Scene;

// A tiny rotating arc used as a "generating" busy indicator. Its rotation is
// driven by a QPropertyAnimation on the `angle` property.
class GenSpinner : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal angle READ angle WRITE setAngle)

public:
    explicit GenSpinner(QWidget *parent = nullptr);

    qreal angle() const { return m_angle; }
    void setAngle(qreal a);

    void start();
    void stop();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    qreal m_angle = 0.0;
    class QPropertyAnimation *m_anim = nullptr;
};

// Generation screen: each approved animatic panel becomes a queue row that can
// be sent to Seedance 2.0 (via fal.ai) to produce an AI video clip. A Claude
// call silently builds an optimized prompt before each fal.ai job.
class GenerationPage : public QWidget
{
    Q_OBJECT

public:
    explicit GenerationPage(QWidget *parent = nullptr);

    // Same ownership pattern as Storyboard/Animatic: non-owning pointers.
    void setConsistencyEntries(const QVector<ConsistencyEntry> *entries);
    void setProjectDir(const QString &dir); // where generated clips are written
    void loadScenes(const QVector<Scene *> &scenes);

signals:
    void backRequested();

private:
    struct Row
    {
        Scene *scene = nullptr;
        Panel *panel = nullptr;
        int sceneIndex = 0;
        int panelIndex = 0;

        QWidget *widget = nullptr;
        QLabel *badge = nullptr;
        QPushButton *generateBtn = nullptr;
        QPushButton *previewBtn = nullptr;
        QPushButton *retryBtn = nullptr;
        GenSpinner *spinner = nullptr;
        QLabel *retryLabel = nullptr; // "(retry 1/2)" next to the spinner
        QLabel *costLabel = nullptr;  // "~$0.05" shown once Complete
        QLabel *promptLink = nullptr; // tiny "View Prompt" debug link

        QString prompt;       // Claude-generated, hidden from the main UI
        QString errorMessage; // shown on the Failed badge tooltip
        int retryCount = 0;     // session-only auto-retry counter (not saved)
        double costEstimate = 0.0; // ~$ for this clip, set on completion this session
    };

    QWidget *createTopBar();
    QWidget *createQueueArea();
    void rebuildRows();
    QWidget *buildRow(int index);
    void refreshRow(int index);

    // Queue processing (sequential).
    void queueRow(int index);
    void generateAllQueued();
    void processQueue();
    void finishRow(int index);

    // Per-job pipeline.
    void startClaudePrompt(int index);
    void onClaudePromptReply(int index, QNetworkReply *reply);
    void callFal(int index);
    void onFalSubmitReply(int index, QNetworkReply *reply);
    void pollStatus(int index);
    void onPollReply(int index, QNetworkReply *reply);
    void fetchResult(int index);
    void onResultReply(int index, QNetworkReply *reply);
    void downloadVideo(int index, const QString &url);
    void onVideoDownloaded(int index, QNetworkReply *reply);
    // retryable=false skips auto-retry (e.g. a missing API key needs user action).
    void failRow(int index, const QString &message, bool retryable = true);
    void updateSessionCost();
    static double clipCost(int durationSeconds); // ~$ for a clamped clip

    // Helpers.
    bool ensureFalKey(); // QMessageBox + false if FAL_API_KEY is missing
    QString buildClaudeRequestBody(int index) const;
    QVector<const ConsistencyEntry *> matchedEntries(int index) const;
    static bool isBlankPixmap(const QPixmap &pixmap);
    void openPreview(int index);

    QVector<Scene *> m_scenes; // non-owning
    const QVector<ConsistencyEntry> *m_entries = nullptr;
    QString m_projectDir;

    QVector<Row> m_rows;
    QVBoxLayout *m_rowsLayout = nullptr;
    QLabel *m_sessionCostLabel = nullptr;
    double m_sessionCost = 0.0; // session total for clips completed this run

    QNetworkAccessManager *m_net = nullptr;
    int m_processing = -1; // row index currently being generated, or -1 when idle
};
