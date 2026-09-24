#include "ytres/ytres.h"

namespace ytres {

namespace {

// 251 is what yt-dlp's -f bestaudio lands on for nearly every video today,
// so preferring it keeps the library playing what the bot plays now. 140 is
// the AAC stream nearly every video has carried for years.
const int PREFERRED_AUDIO_ITAGS[] = {251, 140};

bool isAudioOnly(const Format &format)
{
    return format.mimeType.rfind("audio/", 0) == 0;
}

}

std::optional<Format> VideoInfo::bestAudio() const
{
    for (int itag : PREFERRED_AUDIO_ITAGS) {
        for (const Format &format : formats) {
            if (format.itag == itag && isAudioOnly(format)) {
                return format;
            }
        }
    }

    const Format *best = nullptr;
    for (const Format &format : formats) {
        if (isAudioOnly(format) && (!best || format.bitrate > best->bitrate)) {
            best = &format;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return *best;
}

}
