#pragma once

#include "ProjectMedia.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QString>

class QProgressDialog;
class QWidget;

// The progress window for a media file being copied into the project
// (ProjectMedia::adopt): one implementation for Import Audio, Locate, Copy
// Audio Into Project and Save As.
//
// IT APPEARS ONLY IF THE COPY TAKES LONG ENOUGH TO NOTICE (half a second:
// measured 2026-10-04, a 30-minute WAV copies in 0.3 s on this machine's
// SSD; a hard disk, a USB stick or a network share is where it will show).
// It is modal, with Cancel - not a background copy: until the file is in
// the project the track has no file, and a save, an undo or an export in
// that state would each need its own answer.
//
// Nothing is created until the first byte is reported, so a save that had
// nothing to copy never makes a window.
class MediaCopyProgress
{
public:
    MediaCopyProgress(QWidget *parent, const QString &label);
    ~MediaCopyProgress();
    MediaCopyProgress(const MediaCopyProgress &) = delete;
    MediaCopyProgress &operator=(const MediaCopyProgress &) = delete;

    // Hand this to ProjectMedia / ProjectIO. Valid while this object lives.
    ProjectMedia::Progress callback();

    // For the gate, which cannot press Cancel: asked before the window is,
    // on every report; false cancels the copy.
    static void setTapForTest(ProjectMedia::Progress tap);

private:
    bool report(qint64 done, qint64 total);

    QWidget *m_parent = nullptr;
    QString m_label;
    QElapsedTimer m_clock;
    QPointer<QProgressDialog> m_dialog;
};
