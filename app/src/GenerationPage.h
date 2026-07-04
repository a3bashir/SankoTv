#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QVBoxLayout;

struct ConsistencyEntry;
struct GeneratedTake;
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
        QWidget *mainRow = nullptr;      // the shot row (thumbnail, badge, buttons)
        QWidget *takesStrip = nullptr;   // filmstrip of take chips below the row
        QHBoxLayout *takesLayout = nullptr;
        QLabel *badge = nullptr;
        QPushButton *generateBtn = nullptr; // "Generate" / "Generate Another Take"
        QPushButton *previewBtn = nullptr;  // previews the SELECTED take
        QPushButton *retryBtn = nullptr;
        GenSpinner *spinner = nullptr;
        QLabel *retryLabel = nullptr; // "(retry 1/2)" next to the spinner
        QLabel *costLabel = nullptr;  // total ~$ across this row's takes
        QLabel *promptLink = nullptr; // tiny "View Prompt" debug link

        QString prompt;       // Claude-generated, hidden from the main UI
        QString errorMessage; // shown on the Failed badge tooltip
        int retryCount = 0;     // session-only auto-retry counter (not saved)
        QString activeTakeId;   // the take currently being generated
    };

    QWidget *createTopBar();
    QWidget *createQueueArea();
    void rebuildRows();
    QWidget *buildRow(int index);
    void refreshRow(int index);

    // Version tree (takes) per row.
    void rebuildTakesStrip(int index);
    void selectTake(int index, const QString &takeId);
    void deleteTake(int index, const QString &takeId);
    void previewTake(int index, const QString &takeId);
    static int nextTakeNumber(const Panel *panel); // max existing take # + 1
    double rowTakesCost(const Panel *panel) const; // sum of this row's take costs

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
    static QString findFfmpeg(); // exe dir, then PATH; empty if not found

    // Helpers.
    bool ensureFalKey(); // QMessageBox + false if FAL_API_KEY is missing
    QString buildClaudeRequestBody(int index) const;
    QVector<const ConsistencyEntry *> matchedEntries(int index) const;
    static bool isBlankPixmap(const QPixmap &pixmap);
    void openPreview(int index); // previews the selected take of a row
    void openVideoDialog(const QString &path, const QString &title);

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
