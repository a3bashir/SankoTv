#pragma once

#include <QJsonArray>
#include <QWidget>

class CodeEditor;
class QJsonObject;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

// Second screen in the pipeline: write a script on the left, break it into a
// scene list on the right, then continue to the storyboard. Parsing is done
// entirely LOCALLY (screenplay-heading / paragraph detection) — no network
// request and no API key are ever used. "Skip" jumps straight to boarding with
// a single blank scene.
class ScriptEditorPage : public QWidget
{
    Q_OBJECT

public:
    explicit ScriptEditorPage(QWidget *parent = nullptr);

signals:
    void backRequested();
    void scenesReady(const QJsonArray &scenes); // emitted after each parse / skip
    void continueRequested();                   // navigate to the Storyboard

private slots:
    void onParseClicked(); // local parse only (no API)
    void onSkipClicked();  // blank Scene 1, straight to the Storyboard (no parse)

private:
    QWidget *createWritingPanel();
    QWidget *createBreakdownPanel();
    QWidget *createBottomBar();

    void clearScenes();
    void showBreakdownMessage(const QString &text, const QString &color);
    void populateScenes(const QJsonArray &scenes);
    QWidget *createSceneCard(const QJsonObject &scene);
    void setContinueEnabled(bool enabled);

    CodeEditor *m_editor = nullptr;
    QLabel *m_savedDot = nullptr;
    QLabel *m_savedText = nullptr;
    QTimer *m_savedTimer = nullptr;

    QVBoxLayout *m_scenesLayout = nullptr; // holds scene cards / status messages

    QPushButton *m_parseButton = nullptr;
    QPushButton *m_skipButton = nullptr;
    QPushButton *m_continueButton = nullptr;

    QJsonArray m_scenes; // last parsed scene breakdown, passed to the Storyboard
};
