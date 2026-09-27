#include "innertube.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

using nlohmann::json;
using ytres::innertube::findClient;
using ytres::innertube::playerRequest;

namespace {

const char *const UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
const char *const CHROME_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36";

// The body in docs/innertube-notes.md, verbatim.
const char *const NOTES_BODY = R"json({
  "context": {
    "client": {
      "clientName": "VISIONOS",
      "clientVersion": "1.02",
      "deviceMake": "Apple",
      "deviceModel": "RealityDevice17,1",
      "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
      "osName": "visionOS",
      "osVersion": "26.5.23O471",
      "hl": "en",
      "timeZone": "UTC",
      "utcOffsetMinutes": 0
    }
  },
  "videoId": "dQw4w9WgXcQ",
  "playbackContext": {
    "contentPlaybackContext": { "html5Preference": "HTML5_PREF_WANTS" }
  },
  "contentCheckOk": true,
  "racyCheckOk": true
})json";

using Headers = std::vector<std::pair<std::string, std::string>>;

const Headers NOTES_HEADERS = {
    {"Content-Type", "application/json"},
    {"X-YouTube-Client-Name", "101"},
    {"X-YouTube-Client-Version", "1.02"},
    {"Origin", "https://www.youtube.com"},
    {"User-Agent", UA},
};

}

TEST_CASE("the client table has visionos, which needs no JS player")
{
    const ytres::innertube::ClientDef *client = findClient(ytres::ClientId::VisionOS);
    REQUIRE(client != nullptr);
    CHECK(client->contextClientName == 101);
    CHECK_FALSE(client->requireJsPlayer);
}

TEST_CASE("the player request is the one in the notes, field for field")
{
    const auto request = playerRequest(*findClient(ytres::ClientId::VisionOS), "dQw4w9WgXcQ", "en", "");
    CHECK(request.method == "POST");
    CHECK(request.url == "https://www.youtube.com/youtubei/v1/player?prettyPrint=false");
    CHECK(request.headers == NOTES_HEADERS);
    CHECK(json::parse(request.body) == json::parse(NOTES_BODY));
}

TEST_CASE("visitor data goes out as a header and in the client context, and nowhere else")
{
    const std::string visitorData = "CgtGSVhUVVJFAAAA%3D%3D";
    const auto request = playerRequest(*findClient(ytres::ClientId::VisionOS), "dQw4w9WgXcQ", "en", visitorData);

    Headers expectedHeaders = NOTES_HEADERS;
    expectedHeaders.emplace_back("X-Goog-Visitor-Id", visitorData);
    CHECK(request.headers == expectedHeaders);

    json expectedBody = json::parse(NOTES_BODY);
    expectedBody["context"]["client"]["visitorData"] = visitorData;
    CHECK(json::parse(request.body) == expectedBody);
}

TEST_CASE("the web client is yt-dlp's web row: no user agent in its context, desktop Chrome in the header")
{
    const ytres::innertube::ClientDef *web = findClient(ytres::ClientId::Web);
    REQUIRE(web != nullptr);
    CHECK(web->contextClientName == 1);
    CHECK(web->requireJsPlayer);

    const auto request = playerRequest(*web, "dQw4w9WgXcQ", "en", "");
    const Headers expectedHeaders = {
        {"Content-Type", "application/json"},
        {"X-YouTube-Client-Name", "1"},
        {"X-YouTube-Client-Version", "2.20260708.00.00"},
        {"Origin", "https://www.youtube.com"},
        {"User-Agent", CHROME_UA},
    };
    CHECK(request.headers == expectedHeaders);

    json expectedBody = json::parse(NOTES_BODY);
    expectedBody["context"]["client"] = {
        {"clientName", "WEB"}, {"clientVersion", "2.20260708.00.00"}, {"hl", "en"}, {"timeZone", "UTC"}, {"utcOffsetMinutes", 0},
    };
    CHECK(json::parse(request.body) == expectedBody);
}

TEST_CASE("language goes out as hl")
{
    const auto request = playerRequest(*findClient(ytres::ClientId::VisionOS), "dQw4w9WgXcQ", "pl", "");
    CHECK(json::parse(request.body)["context"]["client"]["hl"] == "pl");
}
