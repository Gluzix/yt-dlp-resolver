#include "arguments.h"
#include "fake_http_client.h"
#include "scrub.h"

#include <doctest/doctest.h>

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
