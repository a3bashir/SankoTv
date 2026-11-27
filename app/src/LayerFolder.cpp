#include "LayerFolder.h"

#include "StoryboardModel.h"

#include <QCoreApplication>
#include <QThread>
#include <QtConcurrent>

#include <vector>

struct LayerFolder::Result
{
    int generation = 0;
    Panel *panel = nullptr;
    QString layerId;
    qint64 imageKey = 0;
    QRect stage;
    layertiles::TilesPtr tiles;
    QString flight;
};

LayerFolder::LayerFolder(QObject *parent) : QObject(parent)
{
    // A fold is background work: a few workers, the app keeps its cores. An
    // unfold is the wait the artist feels: the measurement's 8.
    m_foldPool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
    m_unfoldPool.setMaxThreadCount(qBound(1, QThread::idealThreadCount(), 8));
}

LayerFolder::~LayerFolder()
{
    ++m_generation;
    m_foldPool.waitForDone();
}

bool LayerFolder::panelLive(const Panel *panel) const
{
    if (!m_scenes)
        return false;
    for (const Scene *scene : *m_scenes)
        if (scene && scene->panels.contains(const_cast<Panel *>(panel)))
            return true;
    return false;
}

void LayerFolder::open(Panel *panel)
{
    m_open = panel;
    if (!panel)
        return;
    std::vector<Layer *> todo;
    for (Layer &layer : panel->layers)
        if (layer.atRest())
            todo.push_back(&layer);
    if (todo.empty())
        return;
    QElapsedTimer timer;
    timer.start();
    std::vector<QImage> images(todo.size());
    if (m_serialForTest || todo.size() == 1) {
        for (size_t i = 0; i < todo.size(); ++i)
            images[i] = layertiles::unfold(*todo[i]->tiles, todo[i]->stageRect);
    } else {
        QVector<int> index;
        for (int i = 0; i < int(todo.size()); ++i)
            index.append(i);
        QtConcurrent::blockingMap(&m_unfoldPool, index, [&todo, &images](int i) {
            images[size_t(i)] = layertiles::unfold(*todo[size_t(i)]->tiles, todo[size_t(i)]->stageRect);
        });
    }
    for (size_t i = 0; i < todo.size(); ++i) {
        todo[i]->image = images[i];
        todo[i]->tilesOf = images[i].cacheKey();
    }
    m_lastUnfoldMs = timer.nsecsElapsed() / 1e6;
    m_lastUnfoldLayers = int(todo.size());
}

void LayerFolder::sweep()
{
    if (!m_scenes || m_heldForTest)
        return;
    for (const Scene *scene : *m_scenes) {
        if (!scene)
            continue;
        for (Panel *panel : scene->panels) {
            if (!panel)
                continue;
            for (Layer &layer : panel->layers) {
                if (isGroupLayer(layer))
                    continue;
                // A RUN OF DRAWINGS (batch I-2): every drawing rests as a layer does. On the
                // panel on the canvas only the drawing each layer shows keeps its full image
                // (the layer's own pixels); the others fold as soon as the playhead leaves them.
                const LayerPixels *const own = &layer;
                layer.forEachPixels([&](LayerPixels &px) {
                    if (panel == m_open && &px == own)
                        return;
                    if (px.image.isNull())
                        return;
                    if (px.tilesCurrent()) { // left as it was: nothing to fold
                        px.image = QImage();
                        return;
                    }
                    if (!layertiles::foldable(px.image))
                        return; // stays a full image
                    const QString flight = QStringLiteral("%1/%2/%3")
                                               .arg(quintptr(panel))
                                               .arg(layer.id)
                                               .arg(px.image.cacheKey());
                    if (m_flying.contains(flight))
                        return;
                    m_flying.insert(flight);
                    // Handed to the worker: the image (a shared copy, so an edit
                    // detaches away from it) and the tiles so far.
                    const QImage image = px.image;
                    const QPoint frameAt = panel->frameAt();
                    const bool paper = layer.type == QLatin1String("background");
                    const layertiles::TilesPtr before = px.tiles;
                    const int generation = m_generation;
                    const QString layerId = layer.id;
                    auto job = [this, image, frameAt, paper, before, generation, panel, layerId, flight] {
                        Result r;
                        r.generation = generation;
                        r.panel = panel;
                        r.layerId = layerId;
                        r.imageKey = image.cacheKey();
                        r.stage = QRect(-frameAt, image.size());
                        r.flight = flight;
                        const quint32 fill = paper ? layertiles::paperFill(image, before) : 0u;
                        r.tiles = layertiles::fold(image, frameAt, fill, before);
                        QMetaObject::invokeMethod(this, [this, r] { land(r); }, Qt::QueuedConnection);
                    };
                    if (m_serialForTest)
                        job();
                    else
                        m_foldPool.start(job);
                });
            }
        }
    }
}

void LayerFolder::land(const Result &r)
{
    m_flying.remove(r.flight);
    if (r.generation != m_generation) {
        ++m_foldsDropped;
        return;
    }
    if (panelLive(r.panel))
        for (Layer &layer : r.panel->layers) {
            if (layer.id != r.layerId)
                continue;
            // The layer's own pixels, or one of its run's other drawings (batch I-2): whichever
            // still holds the image this was made from - every one that does (a drawing made
            // "from this one" shares its image with the one it was made from until either is
            // drawn on).
            bool landed = false;
            const LayerPixels *const own = &layer;
            layer.forEachPixels([&](LayerPixels &px) {
                if (px.image.isNull() || px.image.cacheKey() != r.imageKey)
                    return;
                px.tiles = r.tiles;
                px.stageRect = r.stage;
                px.tilesOf = r.imageKey;
                if (r.panel != m_open || &px != own)
                    px.image = QImage();
                landed = true;
            });
            if (landed) {
                ++m_foldsDone;
                return;
            }
        }
    // The layer moved on while this was made: an edit, an undo, the panel gone.
    ++m_foldsDropped;
    if (m_flying.isEmpty())
        QMetaObject::invokeMethod(this, [this] { sweep(); }, Qt::QueuedConnection);
}

void LayerFolder::forget()
{
    ++m_generation;
    m_open = nullptr;
    m_foldPool.waitForDone();
    m_flying.clear();
}

LayerFolder::Held LayerFolder::held() const
{
    Held h;
    if (!m_scenes)
        return h;
    for (const Scene *scene : *m_scenes) {
        if (!scene)
            continue;
        for (const Panel *panel : scene->panels) {
            if (!panel)
                continue;
            for (const Layer &layer : panel->layers) {
                if (isGroupLayer(layer))
                    continue;
                // Every drawing of a run counts as a layer does (batch I-2).
                layer.forEachPixels([&h](const LayerPixels &px) {
                    if (!px.image.isNull()) {
                        h.denseBytes += px.image.sizeInBytes();
                        ++h.denseLayers;
                    } else if (px.tiles) {
                        ++h.foldedLayers;
                    }
                    if (px.tiles)
                        h.tileBytes += px.tiles->bytes();
                });
            }
        }
    }
    return h;
}

void LayerFolder::waitForFoldsForTest()
{
    m_foldPool.waitForDone();
    QCoreApplication::sendPostedEvents(this);
    QCoreApplication::processEvents();
    m_foldPool.waitForDone();
    QCoreApplication::sendPostedEvents(this);
}
