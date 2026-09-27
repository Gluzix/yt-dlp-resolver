#include "ytres/ytres.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

ytres::Format format(int itag, int bitrate, const std::string &mimeType)
{
    ytres::Format made;
    made.itag = itag;
    made.bitrate = bitrate;
    made.mimeType = mimeType;
    made.kind = mimeType.rfind("audio/", 0) == 0 ? ytres::Track::Audio : ytres::Track::Video;
    made.url = "https://example.invalid/" + std::to_string(itag);
    return made;
}

const std::string OPUS = R"(audio/webm; codecs="opus")";
const std::string AAC = R"(audio/mp4; codecs="mp4a.40.2")";
const std::string VP9 = R"(video/webm; codecs="vp9")";

int bestItag(const std::vector<ytres::Format> &formats)
{
    ytres::VideoInfo info;
    info.formats = formats;
    const auto best = info.bestAudio();
    return best ? best->itag : 0;
}

}

TEST_CASE("251 wins even over audio with a higher bitrate")
{
    // The bitrates are the ones YouTube gave for dQw4w9WgXcQ: 140 is the higher.
    CHECK(bestItag({format(140, 129502, AAC), format(249, 46234, OPUS), format(251, 128930, OPUS)}) == 251);
}

TEST_CASE("without 251, 140 wins")
{
    CHECK(bestItag({format(249, 46234, OPUS), format(250, 61149, OPUS), format(140, 129502, AAC), format(999, 300000, OPUS)}) == 140);
}

TEST_CASE("without 251 or 140, the highest bitrate wins")
{
    CHECK(bestItag({format(249, 46234, OPUS), format(250, 61149, OPUS), format(139, 48813, AAC)}) == 250);
}

TEST_CASE("only audio-only formats count")
{
    CHECK(bestItag({format(251, 9000000, VP9), format(249, 46234, OPUS)}) == 249);
    CHECK(bestItag({format(137, 3038377, R"(video/mp4; codecs="avc1.640028")")}) == 0);
    CHECK(bestItag({}) == 0);
}

TEST_CASE("the original language wins over a dub listed first")
{
    ytres::Format german = format(251, 128930, OPUS);
    german.isDefaultAudio = false;
    german.url += "-de";
    const ytres::Format original = format(251, 128930, OPUS);

    ytres::VideoInfo info;
    info.formats = {german, original};
    const auto best = info.bestAudio();
    REQUIRE(best);
    CHECK(best->url == original.url);
}

TEST_CASE("the plain stream wins over a DRC copy listed first")
{
    ytres::Format drc = format(251, 128930, OPUS);
    drc.isDrc = true;
    drc.url += "-drc";
    const ytres::Format plain = format(251, 128930, OPUS);

    ytres::VideoInfo info;
    info.formats = {drc, plain};
    const auto best = info.bestAudio();
    REQUIRE(best);
    CHECK(best->url == plain.url);
}

TEST_CASE("with no original plain audio, every audio format is in the running")
{
    ytres::Format dub = format(140, 129502, AAC);
    dub.isDefaultAudio = false;
    ytres::Format drc = format(251, 128930, OPUS);
    drc.isDrc = true;
    CHECK(bestItag({dub, drc}) == 251);

    // An original 140 still beats a dubbed or compressed 251.
    CHECK(bestItag({drc, format(140, 129502, AAC)}) == 140);
}
