#pragma once

#include <QString>

// CAN THIS FILE BE READ AS AUDIO? Asked BEFORE a picked file is copied into
// a project (Import Audio, Locate Audio File), so that a file SankoTV cannot
// use is refused there, in words, instead of becoming a track that sits
// silent with no explanation (HANDOFF 2026-10-05).
//
// It asks the two readers the app will actually use, with the app's own Qt:
// the PLAYER the animatic plays through (does it open, does it have an
// audio stream and a length), and the DECODER the MP4 export reads with
// (does a first block of sound come out). A file that passes can still be
// decoded WRONGLY - nothing here can hear - but one that fails would have
// failed at Play or at Export.
//
// Runs a short event loop of its own (the readers answer asynchronously),
// with user input excluded: nothing can be clicked while it waits.
namespace AudioFileCheck {

struct Result
{
    bool ok = false;
    QString reason;        // !ok: why, in words that can be shown as they are
    qint64 durationMs = 0; // ok: the track's length
};

Result check(const QString &path, int timeoutMs = 8000);

} // namespace AudioFileCheck
