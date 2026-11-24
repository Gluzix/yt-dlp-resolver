#include "arguments.h"
#include "fake_http_client.h"
#include "scrub.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

using ytres_cli::Arguments;
using ytres_cli::parseArguments;
using ytres_cli::scrubbed;

// The harness's pure parts. Addresses here are from the documentation
// ranges (198.51.100.0/24, 2001:db8::/32), never a real one.

TEST_CASE("the scrubber replaces the requesting address in every form a url carries it")
{
    struct Case
    {
        const char *in;
        const char *out;
    };
    const Case cases[] = {
        {"https://r1.googlevideo.com/videoplayback?expire=1&ip=198.51.100.23&id=o",
         "https://r1.googlevideo.com/videoplayback?expire=1&ip=203.0.113.7&id=o"},
        {"https://i.ytimg.com/vi/x/hq.jpg?ip=198.51.100.23", "https://i.ytimg.com/vi/x/hq.jpg?ip=203.0.113.7"},
        {"&ip=2001:db8:85a3::8a2e:370:7334&x=1", "&ip=203.0.113.7&x=1"},
        {"&ip=2001%3Adb8%3A%3A1&x=1", "&ip=203.0.113.7&x=1"},
        {"/api/manifest/ip/198.51.100.23/id/o", "/api/manifest/ip/203.0.113.7/id/o"},
        {"/ip/2001:db8::1/", "/ip/203.0.113.7/"},
        // percent-encoded whole, inside a signatureCipher
        {R"("signatureCipher":"s=AB&sp=sig&url=https%3A%2F%2Fr1.googlevideo.com%2Fvideoplayback%3Fexpire%3D1%26ip%3D198.51.100.23%26id%3Do")",
         R"("signatureCipher":"s=AB&sp=sig&url=https%3A%2F%2Fr1.googlevideo.com%2Fvideoplayback%3Fexpire%3D1%26ip%3D203.0.113.7%26id%3Do")"},
        {"url=https%3A%2F%2Fr1.googlevideo.com%2Fvideoplayback%3Fip%3D2001%253Adb8%253A%253A1%26id%3Do",
         "url=https%3A%2F%2Fr1.googlevideo.com%2Fvideoplayback%3Fip%3D203.0.113.7%26id%3Do"},
        {"url=https%3A%2F%2Fr1.googlevideo.com%2Fapi%2Fmanifest%2Fip%2F198.51.100.23%2Fid",
         "url=https%3A%2F%2Fr1.googlevideo.com%2Fapi%2Fmanifest%2Fip%2F203.0.113.7%2Fid"},
        // with the ampersand JSON-escaped, as a page may write it
        {R"("url":"https://r1.googlevideo.com/videoplayback?expire=1\u0026ip=198.51.100.23\u0026id=o")",
         R"("url":"https://r1.googlevideo.com/videoplayback?expire=1\u0026ip=203.0.113.7\u0026id=o")"},
    };
    for (const Case &c : cases) {
        const std::string in = c.in;
        CAPTURE(in);
        CHECK(scrubbed(in) == c.out);
    }
}

TEST_CASE("the scrubber leaves alone what only looks like an address parameter")
{
    for (const char *text : {"&skip=1", "&zip=12345", "&vip=ff", "?ship=1.2", "%26zip%3D12345", "/vip/1.2.3.4/", "ip=1.2.3.4"}) {
        const std::string shown = text;
        CAPTURE(shown);
        CHECK(scrubbed(text) == text);
    }
}

TEST_CASE("the scrubber replaces visitor data under both its names, in any spacing and either order")
{
    struct Case
    {
        const char *in;
        const char *out;
    };
    const Case cases[] = {
        {R"({"visitorData":"CgtREALVISITOR","x":1})", R"({"visitorData":"FIXTURE","x":1})"},
        {R"({"visitorData": "CgtREALVISITOR"})", R"({"visitorData":"FIXTURE"})"},
        {R"({"visitorData" : "Cgt\"abc","next":1})", R"({"visitorData":"FIXTURE","next":1})"},
        {R"({"key":"visitor_data","value":"CgtREALVISITOR"})", R"({"key":"visitor_data","value":"FIXTURE"})"},
        {R"({"key": "visitor_data", "value": "CgtREALVISITOR"})", R"({"key":"visitor_data","value":"FIXTURE"})"},
        {R"({"value":"CgtREALVISITOR","key":"visitor_data"})", R"({"value":"FIXTURE","key":"visitor_data"})"},
        {R"({"value": "Cgt\\x", "key": "visitor_data"})", R"({"value":"FIXTURE","key":"visitor_data"})"},
        // other tracking parameters keep their values
        {R"({"key":"logged_in","value":"0"},{"key":"visitor_data","value":"V"})",
         R"({"key":"logged_in","value":"0"},{"key":"visitor_data","value":"FIXTURE"})"},
    };
    for (const Case &c : cases) {
        const std::string in = c.in;
        CAPTURE(in);
        CHECK(scrubbed(in) == c.out);
    }
}

TEST_CASE("every committed fixture is scrubbed already, so scrubbing it again changes nothing")
{
    for (const char *name : {"player_bot_check.json", "player_dQw4w9WgXcQ.json", "player_unavailable.json",
                             "player_web_dQw4w9WgXcQ.json", "playlist_long_continuation.json", "playlist_long_first.json",
                             "playlist_missing.json", "playlist_music_curated.json", "playlist_small.json",
                             "search_continuation.json", "search_empty.json", "search_live.json", "search_videos.json"}) {
        const std::string fixture = name;
        CAPTURE(fixture);
        const std::string body = readFixture(fixture);
        REQUIRE_FALSE(body.empty());
        CHECK(scrubbed(body) == body);
    }
}

namespace {

// scrubbed(in) gives expected, and does not throw. Compared as a bool so
// that a failure does not print megabytes.
void checkScrubsTo(const std::string &in, const std::string &expected)
{
    std::string out;
    CHECK_NOTHROW(out = scrubbed(in));
    const bool same = out == expected;
    CHECK(same);
}

// n escape sequences, \" and \\ in turn.
std::string escapes(std::size_t n)
{
    std::string text;
    for (std::size_t i = 0; i < n; ++i) {
        text += i % 2 == 0 ? R"(\")" : R"(\\)";
    }
    return text;
}

}

// A real player response holds values of thousands of characters, and the
// scrubber once read a JSON string with a regular expression that MSVC
// matches by recursing once per character: it threw on a value of 1000.
TEST_CASE("the scrubber leaves a long value under another key alone")
{
    for (std::size_t n : {std::size_t{1000}, std::size_t{20000}, std::size_t{2000000}}) {
        CAPTURE(n);
        const std::string in = R"({"value":")" + std::string(n, 'a') + R"(","key":"other"})";
        checkScrubsTo(in, in);
    }
}

TEST_CASE("the scrubber replaces visitor data however long it is")
{
    for (std::size_t n : {std::size_t{5000}, std::size_t{200000}}) {
        CAPTURE(n);
        const std::string value(n, 'C');
        checkScrubsTo(R"({"visitorData":")" + value + R"(","x":1})", R"({"visitorData":"FIXTURE","x":1})");
    }
    const std::string value(5000, 'C');
    checkScrubsTo(R"({"key":"visitor_data","value":")" + value + R"("})", R"({"key":"visitor_data","value":"FIXTURE"})");
    checkScrubsTo(R"({"value":")" + value + R"(","key":"visitor_data"})", R"({"value":"FIXTURE","key":"visitor_data"})");
}

TEST_CASE("the scrubber reads a value full of escaped quotes and backslashes whole")
{
    const std::string value = escapes(10000);
    checkScrubsTo(R"({"visitorData":")" + value + R"(","x":1})", R"({"visitorData":"FIXTURE","x":1})");
    const std::string other = R"({"value":")" + value + R"(","key":"other"})";
    checkScrubsTo(other, other);
}

TEST_CASE("the scrubber leaves broken text alone and does not throw on it")
{
    std::string everyByte;
    for (int c = 0; c < 256; ++c) {
        everyByte += static_cast<char>(c);
    }
    const std::string cases[] = {
        "",
        "\"",
        "\\",
        "not json at all",
        everyByte,
        R"({"visitorData":"abc)",
        R"({"visitorData":"abc\)",
        R"({"visitorData":)",
        R"({"key":"visitor_data","value":"abc)",
        R"({"key":"visitor_data","value":"abc\)",
        R"({"value":"abc\)",
        R"({"value":"abc","key":"visitor_da)",
        "{\"visitorData\":\"a\\\nb\"}", // a backslash before a line break escapes nothing
        "&ip=",
        "&ip=%25",
        "%",
        "%2",
        "\\u00",
        "/ip/1.2",
        "%2Fip%2F1.2.3.4",
        R"({"visitorData":")" + std::string(2000000, 'C'),
        R"({"value":")" + std::string(2000000, 'a') + "\\",
        "/ip/" + std::string(2000000, '1'),
    };
    for (const std::string &in : cases) {
        CAPTURE(in.size());
        checkScrubsTo(in, in);
    }
    // Hex digits all the way are an address, however many there are.
    checkScrubsTo("&ip=" + std::string(2000000, 'a'), "&ip=203.0.113.7");
}

TEST_CASE("the scrubber gets through a large player response quickly")
{
    // A format as a player response lists it, with a signature as long as a
    // real one; the address is from a documentation range.
    const std::string signature(900, 'Q');
    const std::string format =
        R"({"itag":251,"url":"https://rr3---sn-abcdefgh.googlevideo.com/videoplayback?expire=1764000000&ei=AbCdEfGh)"
        R"(&ip=198.51.100.23&id=o-AAbbCCddEE&itag=251&source=youtube&requiressl=yes&mh=7c&mm=31%2C29)"
        R"(&mn=sn-abcdefgh%2Csn-ijklmnop&ms=au%2Crdu&mv=m&mvi=3&pl=24&vprv=1&svpuc=1&mime=audio%2Fwebm)"
        R"(&gir=yes&clen=3437753&dur=212.061&lmt=1714829870710966&fvip=4&keepalive=yes&c=IOS)"
        R"(&sparams=expire%2Cei%2Cip%2Cid%2Citag%2Csource%2Crequiressl%2Cvprv%2Csvpuc%2Cmime%2Cgir%2Cclen%2Cdur%2Clmt)"
        R"(&sig=)" + signature + R"(","mimeType":"audio/webm; codecs=\"opus\"","bitrate":135421},)"
        R"({"itag":18,"signatureCipher":"s=)" + signature +
        R"(&sp=sig&url=https%3A%2F%2Frr3---sn-abcdefgh.googlevideo.com%2Fvideoplayback%3Fexpire%3D1764000000)"
        R"(%26ip%3D2001%253Adb8%253A%253A1%26id%3Do-AAbbCCddEE"},)";
    std::string in = R"({"responseContext":{"visitorData":")" + std::string(560, 'C') +
                     R"("},"streamingData":{"hlsManifestUrl":"https://manifest.googlevideo.com/api/manifest/hls_variant/ip/198.51.100.23/id/o","adaptiveFormats":[)";
    std::size_t formats = 0;
    while (in.size() < 3000000) {
        in += format;
        ++formats;
    }
    in += R"({"itag":0}]}})";

    const auto started = std::chrono::steady_clock::now();
    std::string out;
    CHECK_NOTHROW(out = scrubbed(in));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CAPTURE(seconds);
    CHECK(seconds < 5.0); // generous: well under a second in a Debug build

    CHECK(out.find("198.51.100.23") == std::string::npos);
    CHECK(out.find("2001%253Adb8") == std::string::npos);
    std::size_t replaced = 0;
    for (std::size_t at = out.find("203.0.113.7"); at != std::string::npos; at = out.find("203.0.113.7", at + 1)) {
        ++replaced;
    }
    CHECK(replaced == 2 * formats + 1);
    CHECK(out.find(R"("visitorData":"FIXTURE")") != std::string::npos);
}

TEST_CASE("the command line: one mode, its own options, each once")
{
    using Line = std::vector<std::string>;
    const Line accepted[] = {
        {"dQw4w9WgXcQ"},
        {"dQw4w9WgXcQ", "--formats", "--client", "web", "--dump", "x.json"},
        {"--search", "q", "--max", "25", "--dump", "x.json"},
        {"--playlist", "PLx", "--max", "0"},
        {""},                      // an empty target is a target: the library says BadInput
        {"--search", ""},
    };
    for (const Line &line : accepted) {
        CAPTURE(line.size());
        CAPTURE(line.empty() ? std::string{} : line.front());
        Arguments args;
        CHECK(parseArguments(line, args));
    }
    const Line refused[] = {
        {},
        {"--formats"},
        {"", "--playlist", "RDxyz"},                       // the empty target does not vanish
        {"a", "b"},
        {"dQw4w9WgXcQ", "--search", "q"},
        {"--search", "q", "--playlist", "PLx"},
        {"--search", "q", "--search", "r"},
        {"--playlist", "PLx", "--max", "5", "--max", "0"}, // the last no longer wins
        {"x", "--client", "web", "--client", "visionos"},
        {"x", "--dump", "a", "--dump", "b"},
        {"x", "--formats", "--formats"},
        {"x", "--max", "3"},
        {"--search", "q", "--formats"},
        {"--search", "q", "--client", "web"},
        {"x", "--client", "tv"},
        {"--search", "q", "--max"},
        {"--search", "q", "--max", "-1"},
        {"--search", "q", "--max", "3x"},
        {"--search", "q", "--max", "99999999999999999999999"},
        {"--search"},
        {"--bogus", "1"},
    };
    for (const Line &line : refused) {
        std::string shown;
        for (const std::string &part : line) {
            shown += "[" + part + "]";
        }
        CAPTURE(shown);
        Arguments args;
        CHECK_FALSE(parseArguments(line, args));
    }

    Arguments empty;
    REQUIRE(parseArguments({""}, empty));
    REQUIRE(empty.target);
    CHECK(empty.target->empty());
    Arguments search;
    REQUIRE(parseArguments({"--search", "q", "--max", "25", "--dump", "x.json"}, search));
    CHECK(search.query == std::optional<std::string>("q"));
    CHECK(search.max == std::optional<std::size_t>(25));
    CHECK(search.dumpPath == std::optional<std::string>("x.json"));
    CHECK_FALSE(search.target);
}
