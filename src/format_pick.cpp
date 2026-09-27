#include "ytres/ytres.h"

#include <vector>

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

// A dubbed video repeats 251 once per language and YouTube may add a DRC
// copy of it too, so an itag alone does not say which stream is the one
// people uploaded.
bool isOriginalAudio(const Format &format)
{
    return isAudioOnly(format) && format.isDefaultAudio && !format.isDrc;
}

// 251, else 140, else the highest bitrate, among the formats that pass.
const Format *pick(const std::vector<Format> &formats, bool (*passes)(const Format &))
{
    for (int itag : PREFERRED_AUDIO_ITAGS) {
        for (const Format &format : formats) {
            if (format.itag == itag && passes(format)) {
                return &format;
            }
        }
    }
    const Format *best = nullptr;
    for (const Format &format : formats) {
        if (passes(format) && (!best || format.bitrate > best->bitrate)) {
            best = &format;
        }
    }
    return best;
}

}

std::optional<Format> VideoInfo::bestAudio() const
{
    const Format *best = pick(formats, isOriginalAudio);
    if (!best) {
        best = pick(formats, isAudioOnly);
    }
    if (!best) {
        return std::nullopt;
    }
    return *best;
}

}
