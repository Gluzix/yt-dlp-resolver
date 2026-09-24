#include "fake_http_client.h"
#include "innertube.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using ytres::Error;
using ytres::innertube::parsePlayerResponse;

namespace {

ytres::Result<ytres::VideoInfo> resolveWithFixture(const std::string &fixture, const std::string &videoId)
{
    ytres::Resolver::Options options;
    options.http = std::make_shared<FakeHttpClient>(readFixture(fixture));
    ytres::Resolver resolver(options);
    return resolver.resolve(videoId);
}

// A player response around one playability status and one streamingData.
std::string playerResponse(const std::string &playability, const std::string &streamingData,
                           const std::string &videoId = "dQw4w9WgXcQ")
{
    return R"({"playabilityStatus":)" + playability + R"(,"videoDetails":{"videoId":")" + videoId
         + R"(","title":"T","author":"A","lengthSeconds":"213"},"streamingData":)" + streamingData + "}";
}

const char *const OK = R"({"status":"OK"})";

}

TEST_CASE("the recorded dQw4w9WgXcQ response resolves offline")
{
    const auto result = resolveWithFixture("player_dQw4w9WgXcQ.json", "dQw4w9WgXcQ");
    REQUIRE(result);
    const ytres::VideoInfo &info = result.value;
    CHECK(info.videoId == "dQw4w9WgXcQ");
    CHECK(info.title == "Rick Astley - Never Gonna Give You Up (Official Video) (4K Remaster)");
    CHECK(info.author == "Rick Astley");
    CHECK(info.webpageUrl == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    CHECK(info.durationSeconds == 213);
    CHECK_FALSE(info.isLive);
    CHECK(info.formats.size() == 27);
    // expire= from the urls, not the expiresInSeconds fallback that moves with the clock
    CHECK(info.expiresAtUnix == 1790283253);

    const auto best = info.bestAudio();
    REQUIRE(best);
    CHECK(best->itag == 251);
    CHECK(best->kind == ytres::Track::Audio);
    CHECK(best->codec == "opus");
    CHECK(best->bitrate == 128930);          // averageBitrate, not the peak 136544
    CHECK(best->contentLength == 3433755);
    CHECK(best->url.find("itag=251") != std::string::npos);
}

TEST_CASE("the recorded unavailable response is Unavailable with YouTube's reason")
{
    const auto result = resolveWithFixture("player_unavailable.json", "00000000000");
    CHECK(result.status.code == Error::Unavailable);
    CHECK(result.status.message == "This video is unavailable");
}

TEST_CASE("the recorded bot check is LoginRequired with YouTube's reason")
{
    // What a player request without visitor data got for jNQXAC9IVRw.
    const auto result = resolveWithFixture("player_bot_check.json", "jNQXAC9IVRw");
    CHECK(result.status.code == Error::LoginRequired);
    CHECK(result.status.message.find("not a bot") != std::string::npos);
}

TEST_CASE("playability failures map to specific errors")
{
    struct Case
    {
        const char *playability;
        Error expected;
    };
    const Case cases[] = {
        {R"({"status":"LOGIN_REQUIRED","reason":"Sign in to confirm you're not a bot"})", Error::LoginRequired},
        {R"({"status":"LOGIN_REQUIRED","reason":"This video is private"})", Error::Unavailable},
        {R"({"status":"LOGIN_REQUIRED","reason":"Sign in to confirm your age"})", Error::AgeRestricted},
        {R"({"status":"ERROR","reason":"This video has been removed by the uploader"})", Error::Unavailable},
        {R"({"status":"UNPLAYABLE","reason":"The uploader has not made this video available in your country"})", Error::GeoBlocked},
        {R"({"status":"UNPLAYABLE","reason":"This video is age-restricted and only available on YouTube"})", Error::AgeRestricted},
        {R"({"status":"CONTENT_CHECK_REQUIRED","reason":"Viewer discretion is advised"})", Error::Unavailable},
    };
    for (const Case &c : cases) {
        const std::string playability = c.playability;
        CAPTURE(playability);
        const auto result = parsePlayerResponse(playerResponse(c.playability, "{}"), "dQw4w9WgXcQ", 0);
        CHECK(result.status.code == c.expected);
        CHECK_FALSE(result.status.message.empty());
    }
}

TEST_CASE("a region block spelled out only in errorScreen's subreason is GeoBlocked")
{
    const std::string runs = R"({"status":"UNPLAYABLE","reason":"Video unavailable","errorScreen":{"playerErrorMessageRenderer":)"
                             R"({"subreason":{"runs":[{"text":"The uploader has not made this video "},{"text":"available in your country"}]}}}})";
    const auto fromRuns = parsePlayerResponse(playerResponse(runs, "{}"), "dQw4w9WgXcQ", 0);
    CHECK(fromRuns.status.code == Error::GeoBlocked);
    CHECK(fromRuns.status.message == "Video unavailable. The uploader has not made this video available in your country");

    const std::string simpleText = R"({"status":"UNPLAYABLE","reason":"Video unavailable","errorScreen":{"playerErrorMessageRenderer":)"
                                   R"({"subreason":{"simpleText":"This video is not available in your country"}}}})";
    CHECK(parsePlayerResponse(playerResponse(simpleText, "{}"), "dQw4w9WgXcQ", 0).status.code == Error::GeoBlocked);
}

TEST_CASE("a failure without a reason still says something")
{
    const auto result = parsePlayerResponse(playerResponse(R"({"status":"UNPLAYABLE"})", "{}"), "dQw4w9WgXcQ", 0);
    CHECK(result.status.code == Error::Unavailable);
    CHECK(result.status.message == "YouTube says UNPLAYABLE");
}

TEST_CASE("an answer about another video is refused")
{
    const std::string formats = R"({"adaptiveFormats":[{"itag":251,"url":"https://x/?expire=5","mimeType":"audio/webm"}]})";
    const auto result = parsePlayerResponse(playerResponse(OK, formats, "jNQXAC9IVRw"), "dQw4w9WgXcQ", 0);
    CHECK(result.status.code == Error::Parse);
}

TEST_CASE("an age check with nothing behind it is AgeRestricted")
{
    const auto result = parsePlayerResponse(playerResponse(R"({"status":"AGE_CHECK_REQUIRED"})", "{}"), "dQw4w9WgXcQ", 0);
    CHECK(result.status.code == Error::AgeRestricted);
}

TEST_CASE("ciphered formats are skipped, and ciphered alone is NoFormats")
{
    const std::string ciphered = R"({"adaptiveFormats":[{"itag":251,"signatureCipher":"s=abc&sp=sig&url=https%3A%2F%2Fx","mimeType":"audio/webm"}]})";
    const auto none = parsePlayerResponse(playerResponse(OK, ciphered), "dQw4w9WgXcQ", 0);
    CHECK(none.status.code == Error::NoFormats);
    CHECK(none.status.message.find("1 ciphered") != std::string::npos);

    const std::string mixed = R"({"adaptiveFormats":[{"itag":251,"signatureCipher":"s=abc","mimeType":"audio/webm"},)"
                              R"({"itag":140,"url":"https://x/?expire=5","mimeType":"audio/mp4"}]})";
    const auto some = parsePlayerResponse(playerResponse(OK, mixed), "dQw4w9WgXcQ", 0);
    REQUIRE(some);
    REQUIRE(some.value.formats.size() == 1);
    CHECK(some.value.formats[0].itag == 140);
}

TEST_CASE("live segments, OTF streams and DRM formats are skipped, and alone they are NoFormats")
{
    const std::string unplayable =
        R"({"itag":140,"url":"https://x/?expire=5","mimeType":"audio/mp4","targetDurationSec":5},)"
        R"({"itag":140,"url":"https://x/?expire=5","mimeType":"audio/mp4","type":"FORMAT_STREAM_TYPE_OTF"},)"
        R"({"itag":141,"url":"https://x/?expire=5","mimeType":"audio/mp4","drmFamilies":["WIDEVINE"]})";
    const std::string plain = R"({"itag":251,"url":"https://x/?expire=5","mimeType":"audio/webm"})";

    const auto some = parsePlayerResponse(playerResponse(OK, R"({"adaptiveFormats":[)" + unplayable + "," + plain + "]}"),
                                          "dQw4w9WgXcQ", 0);
    REQUIRE(some);
    REQUIRE(some.value.formats.size() == 1);
    CHECK(some.value.formats[0].itag == 251);

    const auto none = parsePlayerResponse(playerResponse(OK, R"({"adaptiveFormats":[)" + unplayable + "]}"), "dQw4w9WgXcQ", 0);
    CHECK(none.status.code == Error::NoFormats);
    CHECK(none.status.message.find("live") != std::string::npos);
}

TEST_CASE("SABR-only streaming data is NoFormats")
{
    const std::string sabr = R"({"serverAbrStreamingUrl":"https://x/sabr","adaptiveFormats":[{"itag":251,"mimeType":"audio/webm"}]})";
    const auto result = parsePlayerResponse(playerResponse(OK, sabr), "dQw4w9WgXcQ", 0);
    CHECK(result.status.code == Error::NoFormats);
    CHECK(result.status.message.find("SABR") != std::string::npos);
}

TEST_CASE("a url without expire= falls back to now + expiresInSeconds")
{
    const std::string formats = R"({"expiresInSeconds":"21540","adaptiveFormats":[{"itag":251,"url":"https://x/videoplayback?id=1","mimeType":"audio/webm"}]})";
    const auto result = parsePlayerResponse(playerResponse(OK, formats), "dQw4w9WgXcQ", 1000);
    REQUIRE(result);
    CHECK(result.value.expiresAtUnix == 1000 + 21540);
}

TEST_CASE("the earliest expire= wins")
{
    const std::string formats = R"({"adaptiveFormats":[{"itag":251,"url":"https://x/?expire=900","mimeType":"audio/webm"},)"
                                R"({"itag":140,"url":"https://x/?expire=800","mimeType":"audio/mp4"}]})";
    const auto result = parsePlayerResponse(playerResponse(OK, formats), "dQw4w9WgXcQ", 0);
    REQUIRE(result);
    CHECK(result.value.expiresAtUnix == 800);
}

TEST_CASE("formats read numbers either way and know their kind")
{
    const std::string formats =
        R"({"formats":[{"itag":18,"url":"https://x/?expire=5","mimeType":"video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"","bitrate":500000,"width":640,"height":360,"fps":25}],)"
        R"("adaptiveFormats":[{"itag":140,"url":"https://x/?expire=5","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","bitrate":130677,)"
        R"("contentLength":"3449447","audioSampleRate":"44100","audioChannels":2},)"
        R"({"itag":137,"url":"https://x/?expire=5","mimeType":"video/mp4; codecs=\"avc1.640028\"","bitrate":4000000,"averageBitrate":3038377}]})";
    const auto result = parsePlayerResponse(playerResponse(OK, formats), "dQw4w9WgXcQ", 0);
    REQUIRE(result);
    REQUIRE(result.value.formats.size() == 3);

    const ytres::Format &muxed = result.value.formats[0];
    CHECK(muxed.kind == ytres::Track::Muxed);
    CHECK(muxed.codec == "avc1.42001E, mp4a.40.2");
    CHECK(muxed.width == 640);
    CHECK(muxed.fps == 25);

    const ytres::Format &audio = result.value.formats[1];
    CHECK(audio.kind == ytres::Track::Audio);
    CHECK(audio.bitrate == 130677);         // no averageBitrate: the peak
    CHECK(audio.contentLength == 3449447);  // a string on the wire
    CHECK(audio.audioSampleRate == 44100);
    CHECK(audio.audioChannels == 2);

    const ytres::Format &video = result.value.formats[2];
    CHECK(video.kind == ytres::Track::Video);
    CHECK(video.bitrate == 3038377);
}

TEST_CASE("an answer that is not a player response is a Parse failure")
{
    const char *const bodies[] = {"", "not json", "[]", R"({"responseContext":{}})", R"({"playabilityStatus":"OK"})"};
    for (const char *body : bodies) {
        const std::string text = body;
        CAPTURE(text);
        CHECK(parsePlayerResponse(body, "dQw4w9WgXcQ", 0).status.code == Error::Parse);
    }
}

TEST_CASE("formats say whether they are DRC copies or dubbed tracks")
{
    const std::string formats =
        R"({"adaptiveFormats":[)"
        R"({"itag":251,"url":"https://x/?expire=5&lang=de","mimeType":"audio/webm","audioTrack":{"id":"de.3","audioIsDefault":false}},)"
        R"({"itag":251,"url":"https://x/?expire=5&drc=1","mimeType":"audio/webm","isDrc":true,"audioTrack":{"id":"en.4","audioIsDefault":true}},)"
        R"({"itag":251,"url":"https://x/?expire=5&lang=en","mimeType":"audio/webm","audioTrack":{"id":"en.4","audioIsDefault":true}},)"
        R"({"itag":140,"url":"https://x/?expire=5","mimeType":"audio/mp4"}]})";
    const auto result = parsePlayerResponse(playerResponse(OK, formats), "dQw4w9WgXcQ", 0);
    REQUIRE(result);
    const auto &parsed = result.value.formats;
    REQUIRE(parsed.size() == 4);
    CHECK_FALSE(parsed[0].isDefaultAudio);
    CHECK(parsed[1].isDrc);
    CHECK(parsed[2].isDefaultAudio);
    CHECK_FALSE(parsed[2].isDrc);
    CHECK(parsed[3].isDefaultAudio); // no audioTrack: the only track there is

    const auto best = result.value.bestAudio();
    REQUIRE(best);
    CHECK(best->url == "https://x/?expire=5&lang=en");
}
