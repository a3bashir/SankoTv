#include "MovieEncoder.h"

#ifdef Q_OS_WIN
#include "MfMovieEncoder.h"
#endif

namespace sankoexport {

MovieEncoder *createMovieEncoder(QObject *parent)
{
#ifdef Q_OS_WIN
    return new MfMovieEncoder(parent);
#else
    Q_UNUSED(parent);
    return nullptr;
#endif
}

QString movieEncoderUnavailableReason()
{
#ifdef Q_OS_WIN
    return MfMovieEncoder::unavailableReason();
#else
    return QStringLiteral("MP4 export is not available on this system yet.");
#endif
}

QString mediaFeaturePackMessage()
{
    return QStringLiteral(
        "MP4 export uses the video and audio encoders that are part of "
        "Windows, and this copy of Windows does not have them. That is usual "
        "on the \"N\" editions of Windows.\n\n"
        "To add them: open Windows Settings, go to Apps > Optional features, "
        "add \"Media Feature Pack\", then restart Windows.\n\n"
        "PNG and PDF export do not need it.");
}

} // namespace sankoexport
