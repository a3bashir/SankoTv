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
            if (!panel || panel == m_open)
                continue;
            for (Layer &layer : panel->layers) {
                if (isGroupLayer(layer) || layer.image.isNull())
                    continue;
                if (layer.tilesCurrent()) { // left as it was: nothing to fold
                    layer.image = QImage();
                    continue;
                }
                if (!layertiles::foldable(layer.image))
                    continue; // stays a full image
                const QString flight = QStringLiteral("%1/%2/%3")
                                           .arg(quintptr(panel))
                                           .arg(layer.id)
                                           .arg(layer.image.cacheKey());
                if (m_flying.contains(flight))
                    continue;
                m_flying.insert(flight);
                // Handed to the worker: the image (a shared copy, so an edit
                // detaches away from it) and the tiles so far.
                const QImage image = layer.image;
                const QPoint frameAt = panel->frameAt();
                const bool paper = layer.type == QLatin1String("background");
                const layertiles::TilesPtr before = layer.tiles;
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
        for (Layer &layer : r.panel->layers)
            if (layer.id == r.layerId && !layer.image.isNull() && layer.image.cacheKey() == r.imageKey) {
                layer.tiles = r.tiles;
                layer.stageRect = r.stage;
                layer.tilesOf = r.imageKey;
                if (r.panel != m_open)
                    layer.image = QImage();
                ++m_foldsDone;
                return;
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
                if (!layer.image.isNull()) {
                    h.denseBytes += layer.image.sizeInBytes();
                    ++h.denseLayers;
                } else if (layer.tiles) {
                    ++h.foldedLayers;
                }
                if (layer.tiles)
                    h.tileBytes += layer.tiles->bytes();
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
