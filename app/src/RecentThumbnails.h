#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>

// Thumbnails for a recents list: decoded ONCE per project at the size they
// are drawn, kept for the session, and never decoded on a paint.
//
// This is the cache the 3b performance pass put inside the New Project
// dialog's list, moved out with the list and changed in two ways:
//
//  * It lives as long as the start window, not as long as one dialog. The
//    dialog's cache died every time the dialog closed, so every opening
//    paid for every row again.
//
//  * Decoding is DEFERRED. The dialog decoded inside its first paintEvent,
//    which was tolerable for a dialog; the start window is what the app
//    opens to, and a 4K first panel costs about 64 ms to decode (measured
//    in 3b - PNG has no cheap scaled read, the "scaled decode" still reads
//    every pixel and then shrinks). Ten of those before the first frame is
//    most of a second of blank window. So request() only QUEUES; one
//    thumbnail is decoded per turn of the event loop, and ready() says
//    which project to repaint.
//
// What it keeps from 3b: the decode goes through
// QImageReader::setScaledSize, so a 33 MB frame is never held as a pixmap,
// and freshness is the image's modification time - a project saved again
// shows its new first panel the next time the list is loaded.
class RecentThumbnails : public QObject
{
    Q_OBJECT

public:
    explicit RecentThumbnails(QObject *parent = nullptr);

    // Ask for a project's thumbnail covering pixelSize (device pixels).
    // Cheap - two file stats - and queues a decode only when there is
    // nothing decoded yet or the image on disk has changed. Call it when a
    // list is LOADED, not when it is painted.
    void request(const QString &projectPath, const QSize &pixelSize);

    // What has been decoded so far: null until ready() has been emitted
    // for this project, and null for good if it has no picture. Never
    // decodes, never touches the disk - this is the paintEvent half.
    QPixmap pixmap(const QString &projectPath, const QSize &pixelSize) const;
    // Has the question been ANSWERED for this project at this size - a
    // picture decoded, or the finding that there is none? False while it is
    // still queued. A list uses it to tell "no picture" (draw the glyph)
    // from "not decoded yet" (draw nothing), so that a project's well does
    // not flash the no-picture glyph in the moment before its picture
    // arrives.
    bool answered(const QString &projectPath, const QSize &pixelSize) const;

    bool busy() const { return !m_queue.isEmpty(); }
    // Full image decodes performed, ever. The gate's measure that a repaint
    // storm decodes nothing and a changed file decodes exactly once.
    int decodeCountForTest() const { return m_decodes; }

signals:
    void ready(const QString &projectPath);

private:
    void decodeNext();

    struct Thumb
    {
        QPixmap pixmap;
        QString source;           // the image file it was decoded from
        QDateTime sourceModified; // ...as it was then
    };
    struct Job
    {
        QString projectPath;
        QSize pixelSize;
    };
    QHash<QString, Thumb> m_thumbs;
    QVector<Job> m_queue;
    QTimer m_pump;
    int m_decodes = 0;
};
