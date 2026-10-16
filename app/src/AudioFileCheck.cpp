#include "AudioFileCheck.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QEventLoop>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>

namespace AudioFileCheck {

namespace {

QString inWords(const QString &readerSays)
{
    const QString said = readerSays.simplified();
    return said.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(said);
}

} // namespace

Result check(const QString &path, int timeoutMs)
{
    Result result;
    if (!QFileInfo(path).isFile()) {
        result.reason = QStringLiteral("The file could not be found.");
        return result;
    }
    const QUrl url = QUrl::fromLocalFile(path);

    // 1. THE PLAYER: does it open, with an audio stream and a length?
    {
        QMediaPlayer player; // no output: nothing is played
        QEventLoop loop;
        QString error;
        bool timedOut = false;
        auto settled = [&player] {
            const QMediaPlayer::MediaStatus s = player.mediaStatus();
            return s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia
                || s == QMediaPlayer::InvalidMedia;
        };
        QObject::connect(&player, &QMediaPlayer::errorOccurred, &loop,
                         [&](QMediaPlayer::Error, const QString &text) {
                             error = text.isEmpty() ? QStringLiteral("unknown error")
                                                    : text;
                             loop.quit();
                         });
        QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop, [&] {
            if (settled())
                loop.quit();
        });
        QTimer::singleShot(timeoutMs, &loop, [&] {
            timedOut = true;
            loop.quit();
        });
        player.setSource(url);
        if (!settled() && error.isEmpty())
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        if (!error.isEmpty() || player.mediaStatus() == QMediaPlayer::InvalidMedia) {
            result.reason = QStringLiteral("It could not be opened as audio%1.")
                                .arg(inWords(error.isEmpty() ? player.errorString()
                                                             : error));
            return result;
        }
        if (timedOut && !settled()) {
            result.reason = QStringLiteral("It could not be read: there was no "
                                           "answer after %1 seconds.")
                                .arg(timeoutMs / 1000);
            return result;
        }
        if (!player.hasAudio()) {
            result.reason = QStringLiteral("It contains no audio.");
            return result;
        }
        if (player.duration() <= 0) {
            result.reason = QStringLiteral("It has no length: there is no sound "
                                           "in it to play.");
            return result;
        }
        result.durationMs = player.duration();
    }

    // 2. THE DECODER, asked as the MP4 export asks: does sound come out?
    {
        QAudioDecoder decoder;
        QAudioFormat wanted;
        wanted.setSampleRate(48000);
        wanted.setChannelCount(2);
        wanted.setSampleFormat(QAudioFormat::Int16);
        decoder.setAudioFormat(wanted);
        decoder.setSource(url);
        QEventLoop loop;
        QString error;
        bool gotSound = false, finished = false, timedOut = false;
        QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&] {
            while (decoder.bufferAvailable())
                if (decoder.read().frameCount() > 0)
                    gotSound = true;
            if (gotSound)
                loop.quit();
        });
        QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&] {
            finished = true;
            loop.quit();
        });
        QObject::connect(&decoder,
                         QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
                         &loop, [&](QAudioDecoder::Error) {
                             error = decoder.errorString().isEmpty()
                                 ? QStringLiteral("unknown error")
                                 : decoder.errorString();
                             loop.quit();
                         });
        QTimer::singleShot(timeoutMs, &loop, [&] {
            timedOut = true;
            loop.quit();
        });
        decoder.start();
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        decoder.stop();
        if (!error.isEmpty()) {
            result.reason = QStringLiteral("Its sound could not be decoded%1.")
                                .arg(inWords(error));
            return result;
        }
        if (!gotSound) {
            result.reason = timedOut && !finished
                ? QStringLiteral("Its sound could not be decoded: there was no "
                                 "answer after %1 seconds.").arg(timeoutMs / 1000)
                : QStringLiteral("It contains no sound to decode.");
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace AudioFileCheck
