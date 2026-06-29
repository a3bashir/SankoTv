#pragma once

#include <QJsonArray>
#include <QWidget>

class CodeEditor;
class QJsonObject;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTimer;
class QVBoxLayout;

// Second screen in the pipeline: write a script on the left, parse it into a
// scene breakdown on the right via the Claude API, then continue to storyboard.
class ScriptEditorPage : public QWidget
{
    Q_OBJECT

public:
    explicit ScriptEditorPage(QWidget *parent = nullptr);

signals:
    void backRequested();
    void scenesReady(const QJsonArray &scenes); // emitted after each successful parse
    void continueRequested();                   // navigate to the Storyboard

private slots:
    void onParseClicked();

private:
    QWidget *createWritingPanel();
    QWidget *createBreakdownPanel();
    QWidget *createBottomBar();

    void handleReply(QNetworkReply *reply, const QString &script);
    void clearScenes();
    void showBreakdownMessage(const QString &text, const QString &color);
    void populateScenes(const QJsonArray &scenes, bool demoMode = false);
    QWidget *createSceneCard(const QJsonObject &scene);
    void setParsing(bool parsing);
    void setContinueEnabled(bool enabled);

    CodeEditor *m_editor = nullptr;
    QLabel *m_savedDot = nullptr;
    QLabel *m_savedText = nullptr;
    QTimer *m_savedTimer = nullptr;

    QVBoxLayout *m_scenesLayout = nullptr; // holds scene cards / status messages

    QPushButton *m_parseButton = nullptr;
    QPushButton *m_continueButton = nullptr;

    QJsonArray m_scenes; // last parsed/mock scene breakdown, passed to Storyboard

    QNetworkAccessManager *m_net = nullptr;
};
