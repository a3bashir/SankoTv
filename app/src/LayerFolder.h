#pragma once

// THE PANELS AT REST (batch 6b-V, 2026-11-07; the measurement of 2026-11-06). Only
// the panel on the canvas holds full images. Entering a panel unfolds its layers
// in parallel, one to a worker (the wait the artist feels: 16 ms at worst on an
// 8-layer 4K panel, 31 ms with a 25 % margin, against 49 and 102 one after
// another). Leaving one folds it on a worker: its full images stay alive until
// that is done, so the switch pays nothing for it and a moment holds one panel
// extra. A panel left unchanged folds at once and for nothing: its tiles still say
// what its images say.
//
// Everything here is the UI thread's except the workers' row copies, which read
// only what they were handed. A result is taken only if its layer still holds the
// image it was made from; any other edit, undo or panel's going is simply no match.

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QVector>

struct Panel;
struct Scene;

class LayerFolder : public QObject
{
    Q_OBJECT
public:
    explicit LayerFolder(QObject *parent = nullptr);
    ~LayerFolder() override;

    // The project's scenes, read when a fold is planned and when one lands.
    void setScenes(const QVector<Scene *> *scenes) { m_scenes = scenes; }
    // `panel` is the one on the canvas now: its full images, in parallel.
    void open(Panel *panel);
    Panel *openPanel() const { return m_open; }
    // Folds every other panel that holds full images.
    void sweep();
    // The scenes are going: waits for the workers, and nothing they did lands.
    void forget();

    // Bytes held, for the recorder and the checks.
    struct Held { qint64 denseBytes = 0, tileBytes = 0; int denseLayers = 0, foldedLayers = 0; };
    Held held() const;
    double lastUnfoldMs() const { return m_lastUnfoldMs; }
    int lastUnfoldLayers() const { return m_lastUnfoldLayers; }

    // Until every fold has landed (the checks: nothing may depend on timing).
    void waitForFoldsForTest();
    int foldsDoneForTest() const { return m_foldsDone; }
    int foldsDroppedForTest() const { return m_foldsDropped; }
    int foldsPendingForTest() const { return int(m_flying.size()); }
    // Keeps a fold from starting until released: a check can then edit or leave
    // while one is in flight.
    void setHeldForTest(bool held) { m_heldForTest = held; if (!held) sweep(); }
    // One after another, as a control against the workers.
    void setSerialForTest(bool serial) { m_serialForTest = serial; }

private:
    struct Result;
    void land(const Result &result);
    bool panelLive(const Panel *panel) const;

    const QVector<Scene *> *m_scenes = nullptr;
    Panel *m_open = nullptr;
    QThreadPool m_foldPool;
    QThreadPool m_unfoldPool;
    QSet<QString> m_flying;   // panel, layer, image: a fold on its way
    int m_generation = 0;     // forget() moves it on; older results are nothing
    int m_foldsDone = 0;
    int m_foldsDropped = 0;
    double m_lastUnfoldMs = 0.0;
    int m_lastUnfoldLayers = 0;
    bool m_heldForTest = false;
    bool m_serialForTest = false;
};
