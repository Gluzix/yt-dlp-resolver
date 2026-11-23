#include "continuation.h"
#include "fake_http_client.h"
#include "innertube.h"
#include "json_read.h"
#include "search.h"
#include "test_resolver.h"

#include "ytres/ytres.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using nlohmann::json;
using nlohmann::ordered_json;
using ytres::Error;
using ytres::innertube::Continuation;
using ytres::innertube::continuationOf;
using ytres::innertube::parseSearchResponse;
using ytres::innertube::SearchPageKind;
using ytres::innertube::searchRequest;
using ytres::jsonread::durationSeconds;
using ytres::jsonread::readInt;
using ytres::jsonread::textOf;
using namespace std::chrono_literals;

namespace {

const char *const SEARCH_URL = "https://www.youtube.com/youtubei/v1/search?prettyPrint=false";
const char *const CHROME_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36";

using Headers = std::vector<std::pair<std::string, std::string>>;

// The web client's headers, as for its player request.
const Headers WEB_HEADERS = {
    {"Content-Type", "application/json"},
    {"X-YouTube-Client-Name", "1"},
    {"X-YouTube-Client-Version", "2.20260708.00.00"},
    {"Origin", "https://www.youtube.com"},
    {"User-Agent", CHROME_UA},
};

// Non-ASCII expectations are spelled as UTF-8 byte escapes, as in
// client_ladder_test.cpp, so this file stays ASCII: "\xC5\x82" is the Polish
// l with stroke. Mind that a hex escape runs on through any hex digit after
// it.

// The query the fixtures answer, and the channel of most of their results.
std::string artist()
{
    return "Dawid Podsiad\xC5\x82o";
}

// The first page's body in docs/innertube-notes.md, "Search", for artist().
std::string notesBody()
{
    return R"json({"context": {"client": {"clientName": "WEB", "clientVersion": "2.20260708.00.00",
                        "hl": "en", "timeZone": "UTC", "utcOffsetMinutes": 0}},
 "query": ")json"
         + artist() + R"json(",
 "params": "EgIQAfABAQ=="})json";
}

const ytres::innertube::ClientDef &webClient()
{
    return *ytres::innertube::findClient(ytres::ClientId::Web);
}

std::vector<std::string> idsOf(const std::vector<ytres::SearchResult> &results)
{
    std::vector<std::string> ids;
    for (const ytres::SearchResult &result : results) {
        ids.push_back(result.videoId);
    }
    return ids;
}

// Hand-made pages, in the shapes the fixtures have: a first page around the
// items of its section list, with YouTube's estimatedResults when given, a
// further page around its appended items, an item section around its
// contents, a continuation, and a videoRenderer with a title, a channel, a
// length and whatever extra adds (from a comma).

std::string firstPage(const std::string &items, const std::string &estimatedResults = {})
{
    const std::string count =
        estimatedResults.empty() ? std::string{} : R"("estimatedResults":")" + estimatedResults + R"(",)";
    return "{" + count + R"("contents":{"twoColumnSearchResultsRenderer":{"primaryContents":{"sectionListRenderer":{"contents":[)"
         + items + "]}}}}}";
}

std::string furtherPage(const std::string &items)
{
    return R"({"onResponseReceivedCommands":[{"appendContinuationItemsAction":{"continuationItems":[)" + items + "]}}]}";
}

std::string section(const std::string &contents)
{
    return R"({"itemSectionRenderer":{"contents":[)" + contents + "]}}";
}

std::string continuationItem(const std::string &token)
{
    return R"({"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":")" + token + R"("}}}})";
}

std::string video(const std::string &videoId, const std::string &extra = {})
{
    return R"({"videoRenderer":{"videoId":")" + videoId
         + R"(","title":{"runs":[{"text":"T"}]},"ownerText":{"runs":[{"text":"A"}]},"lengthText":{"simpleText":"1:00"})"
         + extra + "}}";
}

// An 11-character video id made of n, for pages of distinct videos.
std::string videoIdOf(int n)
{
    std::string digits = std::to_string(n);
    return "v" + std::string(10 - digits.size(), '0') + digits;
}

// The videos numbered first to first + count - 1, from a comma.
std::string videos(int first, int count)
{
    std::string items;
    for (int n = first; n < first + count; ++n) {
        items += (items.empty() ? "" : ",") + video(videoIdOf(n));
    }
    return items;
}

// page, an object, with a responseContext.visitorData of the JSON value given.
std::string withVisitorData(const std::string &value, const std::string &page)
{
    return R"({"responseContext":{"visitorData":)" + value + "}," + page.substr(1);
}

}

// ---- the request ----

TEST_CASE("the search request is the one in the notes, byte for byte")
{
    const auto request = searchRequest(webClient(), artist(), "en", "");
    CHECK(request.method == "POST");
    CHECK(request.url == SEARCH_URL);
    CHECK(request.headers == WEB_HEADERS);
    // In yt-dlp's order too: the context, then the query, then the filter.
    CHECK(request.body == ordered_json::parse(notesBody()).dump());
}

TEST_CASE("visitor data goes out with a search as with the player: a header and the client context")
{
    const auto request = searchRequest(webClient(), "q", "en", VISITOR_DATA);
    Headers expectedHeaders = WEB_HEADERS;
    expectedHeaders.emplace_back("X-Goog-Visitor-Id", VISITOR_DATA);
    CHECK(request.headers == expectedHeaders);

    json body = json::parse(request.body);
    CHECK(body["context"]["client"]["visitorData"] == VISITOR_DATA);
    CHECK(body["query"] == "q");
}

TEST_CASE("a search's language goes out as hl")
{
    CHECK(json::parse(searchRequest(webClient(), "q", "pl", "").body)["context"]["client"]["hl"] == "pl");
}

TEST_CASE("a further page repeats query and params and adds the continuation after them, as yt-dlp does")
{
    const auto request = searchRequest(webClient(), "q", "en", "", Continuation{"TOKEN", "CTP"});
    CHECK(request.url == SEARCH_URL);
    CHECK(request.headers == WEB_HEADERS);
    CHECK(request.body
          == ordered_json::parse(R"({"context":{"client":{"clientName":"WEB","clientVersion":"2.20260708.00.00",)"
                                 R"("hl":"en","timeZone":"UTC","utcOffsetMinutes":0}},"query":"q","params":"EgIQAfABAQ==",)"
                                 R"("continuation":"TOKEN","clickTracking":{"clickTrackingParams":"CTP"}})")
                 .dump());

    // No clickTrackingParams, no clickTracking.
    json bare = json::parse(searchRequest(webClient(), "q", "en", "", Continuation{"TOKEN", ""}).body);
    CHECK(bare["continuation"] == "TOKEN");
    CHECK(bare.count("clickTracking") == 0);
}

TEST_CASE("apiRequest puts the client's context first and the endpoint's fields after it, in their order")
{
    ordered_json fields = ordered_json::object();
    fields["browseId"] = "VLPL";
    fields["params"] = "P";
    const auto request = ytres::innertube::apiRequest(webClient(), "browse", "en", "", fields);
    CHECK(request.method == "POST");
    CHECK(request.url == "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false");
    CHECK(request.headers == WEB_HEADERS);
    CHECK(request.body == R"({"context":{"client":{"clientName":"WEB","clientVersion":"2.20260708.00.00","hl":"en",)"
                          R"("timeZone":"UTC","utcOffsetMinutes":0}},"browseId":"VLPL","params":"P"})");
}

// ---- continuations ----

TEST_CASE("continuationOf reads the token in every shape YouTube writes one")
{
    struct Case
    {
        const char *name;
        const char *item;
        const char *token;
        const char *clickTrackingParams;
    };
    const Case cases[] = {
        {"a renderer's endpoint, as a search page ends",
         R"({"continuationItemRenderer":{"continuationEndpoint":{"clickTrackingParams":"C1","continuationCommand":{"token":"T1"}}}})",
         "T1", "C1"},
        {"a renderer's button",
         R"({"continuationItemRenderer":{"button":{"buttonRenderer":{"command":)"
         R"({"clickTrackingParams":"C2","continuationCommand":{"token":"T2"}}}}}})",
         "T2", "C2"},
        {"a view model's innertubeCommand, as a playlist page ends",
         R"({"continuationItemViewModel":{"continuationCommand":{"innertubeCommand":)"
         R"({"clickTrackingParams":"C3","continuationCommand":{"token":"T3"}}}}})",
         "T3", "C3"},
        {"a commandExecutorCommand: the first of its commands with a token, and that command's tracking",
         R"({"continuationItemRenderer":{"continuationEndpoint":{"clickTrackingParams":"OUTER","commandExecutorCommand":{"commands":[)"
         R"({"clickTrackingParams":"NONE","showReloadUiCommand":{}},{"clickTrackingParams":"C4","continuationCommand":{"token":"T4"}}]}}}})",
         "T4", "C4"},
        {"a commandExecutorCommand in a view model",
         R"({"continuationItemViewModel":{"continuationCommand":{"innertubeCommand":{"commandExecutorCommand":{"commands":[)"
         R"({"continuationCommand":{"token":"T5"}}]}}}}})",
         "T5", ""},
        {"an endpoint without a token, then the button",
         R"({"continuationItemRenderer":{"continuationEndpoint":{"clickTrackingParams":"X"},)"
         R"("button":{"buttonRenderer":{"command":{"continuationCommand":{"token":"T6"}}}}}})",
         "T6", ""},
    };
    for (const Case &c : cases) {
        const std::string name = c.name; // a std::string, so a failure prints the text, not the pointer
        CAPTURE(name);
        const Continuation found = continuationOf(json::parse(c.item));
        CHECK(found.token == c.token);
        CHECK(found.clickTrackingParams == c.clickTrackingParams);
    }
}

TEST_CASE("an item that is no continuation reads as one without a token")
{
    const char *const items[] = {
        R"({"itemSectionRenderer":{"contents":[]}})",
        R"({"continuationItemRenderer":{}})",
        R"({"continuationItemRenderer":{"continuationEndpoint":{"clickTrackingParams":"C","continuationCommand":{"token":""}}}})",
        R"({"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":7}}}})",
        R"({"continuationItemViewModel":{"continuationCommand":{"token":"not where a view model keeps it"}}})",
        "[]",
        R"("text")",
        "null",
    };
    for (const char *item : items) {
        const std::string text = item;
        CAPTURE(text);
        const Continuation found = continuationOf(json::parse(item));
        CHECK(found.token.empty());
        CHECK(found.clickTrackingParams.empty());
    }
}

TEST_CASE("addContinuation adds clickTracking only when there is some, as yt-dlp does")
{
    ordered_json tracked = ordered_json::object();
    ytres::innertube::addContinuation(tracked, {"T", "C"});
    CHECK(tracked.dump() == R"({"continuation":"T","clickTracking":{"clickTrackingParams":"C"}})");

    ordered_json untracked = ordered_json::object();
    ytres::innertube::addContinuation(untracked, {"T", ""});
    CHECK(untracked.dump() == R"({"continuation":"T"})");
}

// ---- the readers ----

TEST_CASE("durationSeconds reads m:ss and h:mm:ss, and nothing else")
{
    CHECK(durationSeconds("4:36") == 276);
    CHECK(durationSeconds("2:31:48") == 9108);
    CHECK(durationSeconds("0:07") == 7);
    CHECK(durationSeconds("12:00:00") == 43200);
    for (const char *text : {"", "LIVE", "1:2x", "Upcoming", "45", ":36", "4:", "4::36", "1:2:3:4", " 4:36", "4:36 ",
                             "-4:36", "1234567890:00"}) {
        const std::string shown = text;
        CAPTURE(shown);
        CHECK(durationSeconds(text) == 0);
    }
}

TEST_CASE("readInt reads a number or a digit string, clamps an unsigned one past int64, and reads the rest as 0")
{
    const json object = json::parse(R"({"n":212,"s":"212","neg":-5,"huge":18446744073709551615,"top":9223372036854775807,)"
                                    R"("float":1e3,"text":"1,234","bool":true,"null":null})");
    CHECK(readInt(object, "n") == 212);
    CHECK(readInt(object, "s") == 212);
    CHECK(readInt(object, "neg") == -5);
    CHECK(readInt(object, "huge") == INT64_MAX); // not wrapped to -1
    CHECK(readInt(object, "top") == INT64_MAX);
    CHECK(readInt(object, "float") == 0);
    CHECK(readInt(object, "text") == 0);
    CHECK(readInt(object, "bool") == 0);
    CHECK(readInt(object, "null") == 0);
    CHECK(readInt(object, "missing") == 0);
}

TEST_CASE("textOf reads a label written any of InnerTube's three ways")
{
    const json object = json::parse(R"({"simple":{"simpleText":"S"},"runs":{"runs":[{"text":"R1"},{"text":"R2"},7]},)"
                                    R"("model":{"content":"C"},"plain":"P","empty":{},"number":{"simpleText":3}})");
    CHECK(textOf(object, "simple") == "S");
    CHECK(textOf(object, "runs") == "R1R2");
    CHECK(textOf(object, "model") == "C");
    CHECK(textOf(object, "plain").empty()); // a bare string is none of the three
    CHECK(textOf(object, "empty").empty());
    CHECK(textOf(object, "number").empty());
    CHECK(textOf(object, "missing").empty());
}

TEST_CASE("the recorded first page: the channel read past, four videos, the continuation")
{
    const auto page = parseSearchResponse(readFixture("search_videos.json"), SearchPageKind::First);
    REQUIRE(page);
    const std::vector<ytres::SearchResult> &results = page.value.results;
    REQUIRE(results.size() == 4);

    CHECK(results[0].videoId == "MxWXAIWsppY");
    CHECK(results[0].title == "Dawid Podsiad\xC5\x82o \"na b\xC5\x82ysk\"");
    CHECK(results[0].author == artist());
    CHECK(results[0].durationSeconds == 276);
    CHECK_FALSE(results[0].isLive);
    CHECK_FALSE(results[0].isUpcoming);

    CHECK(results[1].videoId == "oCZugu1ea18");
    CHECK(results[1].durationSeconds == 291);

    CHECK(results[2].videoId == "2DiP0mMeaT8");
    CHECK(results[2].title == "Dawid Podsiad\xC5\x82o - mori (Official Video)");
    CHECK(results[2].durationSeconds == 194);

    CHECK(results[3].videoId == "jyXHU7PtsmI");
    CHECK(results[3].title == "sezon");
    CHECK(results[3].durationSeconds == 182);

    CHECK(page.value.next.token.rfind("ErcDEhBE", 0) == 0);
    CHECK(page.value.next.clickTrackingParams.rfind("CEMQt6kL", 0) == 0);
}

TEST_CASE("the recorded second page: three videos and the next continuation")
{
    const auto page = parseSearchResponse(readFixture("search_continuation.json"), SearchPageKind::Further);
    REQUIRE(page);
    const std::vector<ytres::SearchResult> &results = page.value.results;
    CHECK(idsOf(results) == std::vector<std::string>{"g4UDeQTjMYk", "5rNXe7Z1qN0", "c9B4Z_HRAcI"});
    REQUIRE(results.size() == 3);
    CHECK(results[0].durationSeconds == 247);
    CHECK(results[1].durationSeconds == 244);
    CHECK(results[2].durationSeconds == 224);
    for (const ytres::SearchResult &result : results) {
        CHECK(result.author == artist());
    }
    CHECK(page.value.next.token.rfind("EqkDEhBE", 0) == 0);
    CHECK(page.value.next.clickTrackingParams.rfind("CAEQt6kL", 0) == 0);
}

TEST_CASE("the recorded empty search: Ok, no videos, no continuation")
{
    const auto page = parseSearchResponse(readFixture("search_empty.json"), SearchPageKind::First);
    REQUIRE(page);
    CHECK(page.value.results.empty());
    CHECK(page.value.next.token.empty());
}

TEST_CASE("the recorded live streams: live, no length, the channel's name")
{
    const auto page = parseSearchResponse(readFixture("search_live.json"), SearchPageKind::First);
    REQUIRE(page);
    const std::vector<ytres::SearchResult> &results = page.value.results;
    CHECK(idsOf(results) == std::vector<std::string>{"rFZHOHl-L8A", "JD-kMIpDfnY", "E2vONfzoyRI"});
    for (const ytres::SearchResult &result : results) {
        CAPTURE(result.videoId);
        CHECK(result.isLive);
        CHECK_FALSE(result.isUpcoming);
        CHECK(result.durationSeconds == 0);
        CHECK(result.author == "Lofi Girl");
    }
    REQUIRE_FALSE(results.empty());
    // A four-byte UTF-8 character, the books emoji, comes through whole.
    CHECK(results[0].title == "lofi hip hop radio \xF0\x9F\x93\x9A beats to relax/study to");
}

TEST_CASE("a result is live by its badge or its overlay, upcoming by its event data, and named by a byline if need be")
{
    const std::string items =
        video("aaaaaaaaaaa", R"(,"badges":[{"metadataBadgeRenderer":{"style":"BADGE_STYLE_TYPE_SIMPLE"}},)"
                             R"({"metadataBadgeRenderer":{"style":"BADGE_STYLE_TYPE_LIVE_NOW"}}])")
        + "," + video("bbbbbbbbbbb", R"(,"thumbnailOverlays":[{"thumbnailOverlayTimeStatusRenderer":{"style":"LIVE"}}])")
        + "," + video("ccccccccccc", R"(,"upcomingEventData":{"startTime":"1790000000"})")
        + "," + R"({"videoRenderer":{"videoId":"ddddddddddd","title":{"simpleText":"Simple"},)"
                R"("longBylineText":{"runs":[{"text":"Long"}]},"shortBylineText":{"runs":[{"text":"Short"}]}}})"
        + "," + R"({"videoRenderer":{"videoId":"eeeeeeeeeee","shortBylineText":{"runs":[{"text":"Sh"},{"text":"ort"}]}}})";
    const auto page = parseSearchResponse(firstPage(section(items)), SearchPageKind::First);
    REQUIRE(page);
    const std::vector<ytres::SearchResult> &results = page.value.results;
    REQUIRE(results.size() == 5);

    CHECK(results[0].isLive);
    CHECK_FALSE(results[0].isUpcoming);
    CHECK(results[0].durationSeconds == 60);
    CHECK(results[1].isLive);
    CHECK_FALSE(results[2].isLive);
    CHECK(results[2].isUpcoming);

    CHECK(results[3].title == "Simple");
    CHECK(results[3].author == "Long");
    CHECK(results[3].durationSeconds == 0);
    CHECK_FALSE(results[3].isLive);
    CHECK(results[4].title.empty());
    CHECK(results[4].author == "Short");
}

TEST_CASE("without lengthText, the length comes from the time status over the thumbnail, as yt-dlp reads it")
{
    const std::string items =
        R"({"videoRenderer":{"videoId":"aaaaaaaaaaa","thumbnailOverlays":[{"thumbnailOverlayToggleButtonRenderer":{}},)"
        R"({"thumbnailOverlayTimeStatusRenderer":{"text":{"simpleText":"12:34"},"style":"DEFAULT"}},)"
        R"({"thumbnailOverlayTimeStatusRenderer":{"text":{"simpleText":"9:59"}}}]}},)"
        R"({"videoRenderer":{"videoId":"bbbbbbbbbbb","thumbnailOverlays":)"
        R"([{"thumbnailOverlayTimeStatusRenderer":{"text":{"runs":[{"text":"LIVE"}]},"style":"LIVE"}}]}},)"
        + video("ccccccccccc", R"(,"thumbnailOverlays":[{"thumbnailOverlayTimeStatusRenderer":{"text":{"simpleText":"9:00"}}}])");
    const auto page = parseSearchResponse(firstPage(section(items)), SearchPageKind::First);
    REQUIRE(page);
    const std::vector<ytres::SearchResult> &results = page.value.results;
    REQUIRE(results.size() == 3);
    CHECK(results[0].durationSeconds == 754); // the first time status, 12:34
    CHECK(results[1].durationSeconds == 0);   // "LIVE"
    CHECK(results[1].isLive);
    CHECK(results[2].durationSeconds == 60);  // lengthText, 1:00, comes first
}

TEST_CASE("whatever is not a video with a valid id is read past, in any section, and the first continuation counts")
{
    const std::string firstSection =
        R"({"channelRenderer":{"channelId":"UC"}},{"shelfRenderer":{}},{"videoRenderer":{"videoId":"short"}},)"
        R"({"videoRenderer":"not an object"},42,null,)"
        + video("aaaaaaaaaaa") + ","
        + R"({"videoRenderer":{"videoId":"bbbbbbbbbbb","title":"a bare string","ownerText":7}})";
    const std::string items = section(firstSection) + R"(,"x",null,{"richShelfRenderer":{}},)" + section(video("ccccccccccc"))
                            + "," + continuationItem("FIRST") + "," + continuationItem("SECOND");
    const auto page = parseSearchResponse(firstPage(items), SearchPageKind::First);
    REQUIRE(page);
    CHECK(idsOf(page.value.results) == std::vector<std::string>{"aaaaaaaaaaa", "bbbbbbbbbbb", "ccccccccccc"});
    REQUIRE(page.value.results.size() == 3);
    CHECK(page.value.results[1].title.empty());
    CHECK(page.value.results[1].author.empty());
    CHECK(page.value.next.token == "FIRST");
}

TEST_CASE("a corrected query's card ahead of the videos is read past, and the videos are the results")
{
    // The shape of a misspelled query's first page on 2026-11-23: YouTube's
    // "Showing results for" card first in the item section, then the videos.
    const std::string corrected =
        R"({"showingResultsForRenderer":{"showingResultsFor":{"runs":[{"text":"Showing results for"}]},)"
        R"("correctedQuery":{"runs":[{"text":"rick "},{"text":"astley","italics":true}]},)"
        R"("searchInsteadFor":{"runs":[{"text":"Search instead for"}]},"originalQuery":{"simpleText":"rick astly"}}})";
    const auto page = parseSearchResponse(
        firstPage(section(corrected + "," + video("aaaaaaaaaaa") + "," + video("bbbbbbbbbbb")) + "," + continuationItem("NEXT"),
                  "1234"),
        SearchPageKind::First);
    REQUIRE(page);
    CHECK(idsOf(page.value.results) == std::vector<std::string>{"aaaaaaaaaaa", "bbbbbbbbbbb"});
    CHECK(page.value.next.token == "NEXT");
}

TEST_CASE("a first page that counts results but holds no video the library can read is Parse")
{
    // A search as it would look had YouTube moved its results into a view
    // model the reader does not know, as it has for playlists.
    const std::string unknownItem = R"({"lockupViewModel":{}})";
    const auto moved = parseSearchResponse(firstPage(section(unknownItem), "12"), SearchPageKind::First);
    CHECK(moved.status.code == Error::Parse);
    CHECK(moved.status.message == "The search response counts results but holds no video the library can read");
    CHECK(moved.value.results.empty());

    // Whatever form the count comes in, a count above nothing keeps the alarm on.
    for (const char *count : {R"("1,234")", "18446744073709551615", "1e3", "12", R"(" 7")", "-5"}) {
        const std::string shown = count;
        CAPTURE(shown);
        const std::string page = R"({"estimatedResults":)" + shown + "," + firstPage(section(unknownItem)).substr(1);
        CHECK(parseSearchResponse(page, SearchPageKind::First).status.code == Error::Parse);
    }
    for (const char *count : {R"("0")", "0", "0.0", R"("")", "null", "[]"}) {
        const std::string shown = count;
        CAPTURE(shown);
        const std::string page = R"({"estimatedResults":)" + shown + "," + firstPage(section(unknownItem)).substr(1);
        CHECK(parseSearchResponse(page, SearchPageKind::First));
    }

    // Nothing counted and nothing there is an empty search, as recorded.
    CHECK(parseSearchResponse(firstPage(section(unknownItem), "0"), SearchPageKind::First));
    CHECK(parseSearchResponse(firstPage(section(unknownItem)), SearchPageKind::First));
    CHECK(parseSearchResponse(readFixture("search_empty.json"), SearchPageKind::First));

    // A further page with nothing readable is the end of the results, count or not.
    const auto further = parseSearchResponse(
        R"({"estimatedResults":"12","onResponseReceivedCommands":[{"appendContinuationItemsAction":)"
        R"({"continuationItems":[{"itemSectionRenderer":{"contents":[{"lockupViewModel":{}}]}}]}}]})",
        SearchPageKind::Further);
    REQUIRE(further);
    CHECK(further.value.results.empty());

    // Through the Resolver: a failure the caller falls back on, not "nothing found".
    TestResolver test;
    test.http->apiBodies["search"] = {firstPage(section(unknownItem), "12")};
    CHECK(test.resolver.search(artist(), 5).status.code == Error::Parse);

    // Neither a correction card nor a shelf alone tells the reader where the
    // videos went: still Parse.
    for (const char *card : {R"({"showingResultsForRenderer":{}})", R"({"didYouMeanRenderer":{}})",
                             R"({"shelfRenderer":{}})", R"({"reelShelfRenderer":{}})"}) {
        const std::string shown = card;
        CAPTURE(shown);
        CHECK(parseSearchResponse(firstPage(section(shown), "12"), SearchPageKind::First).status.code == Error::Parse);
    }
    // Nor does a section list holding no item section at all.
    CHECK(parseSearchResponse(firstPage(R"({"richGridRenderer":{}})", "12"), SearchPageKind::First).status.code
          == Error::Parse);
    CHECK(parseSearchResponse(firstPage(section(""), "12"), SearchPageKind::First).status.code == Error::Parse);
}

TEST_CASE("a first page that holds only a channel is no alarm, and leads on to the videos when it can")
{
    // A query that matches a channel and no video.
    const std::string channel = R"({"channelRenderer":{"channelId":"UCx"}})";
    const auto alone = parseSearchResponse(firstPage(section(channel), "1"), SearchPageKind::First);
    REQUIRE(alone);
    CHECK(alone.value.results.empty());
    CHECK(alone.value.counted);
    CHECK_FALSE(alone.value.unreadable);
    TestResolver lone;
    lone.http->apiBodies["search"] = {firstPage(section(channel), "1")};
    const auto none = lone.resolver.search(artist(), 5);
    REQUIRE(none);
    CHECK(none.value.empty());
    CHECK(lone.http->requests.size() == 1);

    // A channel that fills page one, with the videos on page two.
    TestResolver test;
    test.http->apiBodies["search"] = {firstPage(section(channel) + "," + continuationItem("T1"), "100"),
                                      furtherPage(section(video("aaaaaaaaaaa") + "," + video("bbbbbbbbbbb")))};
    const auto found = test.resolver.search(artist(), 5);
    REQUIRE(found);
    CHECK(idsOf(found.value) == std::vector<std::string>{"aaaaaaaaaaa", "bbbbbbbbbbb"});
    CHECK(test.http->requests.size() == 2);
    CHECK_FALSE(test.logged(ytres::LogLevel::Warning));

    // ... or with nothing on page two either: Ok and empty, since a channel
    // is something the reader knows.
    TestResolver empty;
    empty.http->apiBodies["search"] = {firstPage(section(channel) + "," + continuationItem("T1"), "100"),
                                       furtherPage(section(""))};
    const auto nothing = empty.resolver.search(artist(), 5);
    REQUIRE(nothing);
    CHECK(nothing.value.empty());
    CHECK(empty.http->requests.size() == 2);
}

TEST_CASE("a first page with no video it can read but a continuation is judged by the page it leads to")
{
    const std::string unknownItem = R"({"lockupViewModel":{}})";
    const auto page =
        parseSearchResponse(firstPage(section(unknownItem) + "," + continuationItem("T1"), "12"), SearchPageKind::First);
    REQUIRE(page);
    CHECK(page.value.unreadable);
    CHECK(page.value.next.token == "T1");

    // The next page has videos: they are the results.
    TestResolver test;
    test.http->apiBodies["search"] = {firstPage(section(unknownItem) + "," + continuationItem("T1"), "12"),
                                      furtherPage(section(video("aaaaaaaaaaa")))};
    const auto found = test.resolver.search(artist(), 5);
    REQUIRE(found);
    CHECK(idsOf(found.value) == std::vector<std::string>{"aaaaaaaaaaa"});

    // It has none either: Parse, after two requests.
    TestResolver moved;
    moved.http->apiBodies["search"] = {firstPage(section(unknownItem) + "," + continuationItem("T1"), "12"),
                                       furtherPage(section(unknownItem) + "," + continuationItem("T2"))};
    const auto failed = moved.resolver.search(artist(), 5);
    CHECK(failed.status.code == Error::Parse);
    CHECK(failed.status.message == "The search response counts results but holds no video the library can read");
    CHECK(failed.value.empty());
    CHECK(moved.http->requests.size() == 2);

    // A first page that counts nothing and brings nothing still ends there.
    TestResolver uncounted;
    uncounted.http->apiBodies["search"] = {firstPage(section(unknownItem) + "," + continuationItem("T1"), "0")};
    const auto empty = uncounted.resolver.search(artist(), 5);
    REQUIRE(empty);
    CHECK(empty.value.empty());
    CHECK(uncounted.http->requests.size() == 1);
}

TEST_CASE("a continuation inside an item section serves when none sits beside the sections, as yt-dlp reads it")
{
    const std::string inner = section(video("aaaaaaaaaaa") + "," + continuationItem("INNER") + "," + continuationItem("LATER"));
    const auto alone = parseSearchResponse(firstPage(inner), SearchPageKind::First);
    REQUIRE(alone);
    CHECK(idsOf(alone.value.results) == std::vector<std::string>{"aaaaaaaaaaa"});
    CHECK(alone.value.next.token == "INNER");

    // The one beside the sections comes first, wherever it stands.
    const auto both = parseSearchResponse(furtherPage(inner + "," + continuationItem("BESIDE")), SearchPageKind::Further);
    REQUIRE(both);
    CHECK(both.value.next.token == "BESIDE");
}

TEST_CASE("a page is read from the container its request expects, even when it carries both")
{
    // A first page with an append action beside its section list: a shape
    // no capture has shown, read as the first page it was asked for.
    const std::string appended =
        R"("onResponseReceivedCommands":[{"appendContinuationItemsAction":{"continuationItems":[)" + section(video("bbbbbbbbbbb"))
        + "]}}],";
    const std::string bothFirst = "{" + appended + firstPage(section(video("aaaaaaaaaaa")) + "," + continuationItem("SIB")).substr(1);
    const auto first = parseSearchResponse(bothFirst, SearchPageKind::First);
    REQUIRE(first);
    CHECK(idsOf(first.value.results) == std::vector<std::string>{"aaaaaaaaaaa"});
    CHECK(first.value.next.token == "SIB");
    // The same answer to a continuation is read from its append action.
    const auto further = parseSearchResponse(bothFirst, SearchPageKind::Further);
    REQUIRE(further);
    CHECK(idsOf(further.value.results) == std::vector<std::string>{"bbbbbbbbbbb"});

    // And with an empty append action beside it, the first page keeps its
    // videos, and its alarm: a count with nothing readable is still Parse.
    const std::string emptyAppend = R"({"onResponseReceivedCommands":[{"appendContinuationItemsAction":{"continuationItems":[]}}],)";
    const auto kept = parseSearchResponse(emptyAppend + firstPage(section(video("aaaaaaaaaaa"))).substr(1), SearchPageKind::First);
    REQUIRE(kept);
    CHECK(idsOf(kept.value.results) == std::vector<std::string>{"aaaaaaaaaaa"});
    const auto alarmed =
        parseSearchResponse(emptyAppend + firstPage(section(R"({"lockupViewModel":{}})"), "12").substr(1), SearchPageKind::First);
    CHECK(alarmed.status.code == Error::Parse);

    // A page without the container its request expects is Parse.
    CHECK(parseSearchResponse(firstPage(section(video("aaaaaaaaaaa"))), SearchPageKind::Further).status.code == Error::Parse);
    CHECK(parseSearchResponse(furtherPage(section(video("aaaaaaaaaaa"))), SearchPageKind::First).status.code == Error::Parse);

    // Through the Resolver: page one's own videos, then page two's.
    TestResolver test;
    test.http->apiBodies["search"] = {bothFirst, furtherPage(section(video("ccccccccccc")))};
    const auto found = test.resolver.search(artist(), 10);
    REQUIRE(found);
    CHECK(idsOf(found.value) == std::vector<std::string>{"aaaaaaaaaaa", "ccccccccccc"});
}

TEST_CASE("a search page names the visitor YouTube took the caller for, when it may go out as a header")
{
    const auto recorded = parseSearchResponse(readFixture("search_videos.json"), SearchPageKind::First);
    REQUIRE(recorded);
    CHECK(recorded.value.visitorData == "FIXTURE");

    const std::string page = firstPage(section(video("aaaaaaaaaaa")));
    const auto without = parseSearchResponse(page, SearchPageKind::First);
    REQUIRE(without);
    CHECK(without.value.visitorData.empty());
    const auto fit = parseSearchResponse(withVisitorData(R"("CgtGSVhUVVJF%3D%3D")", page), SearchPageKind::First);
    REQUIRE(fit);
    CHECK(fit.value.visitorData == "CgtGSVhUVVJF%3D%3D");

    // The watch page's rule: never a value that could not travel as a header.
    const std::string refused[] = {R"("bad value")", R"("CgtG\r\nX-Injected: 1")", R"("")", "42",
                                   "\"" + std::string(4097, 'A') + "\""};
    for (const std::string &value : refused) {
        CAPTURE(value);
        const auto parsed = parseSearchResponse(withVisitorData(value, page), SearchPageKind::First);
        REQUIRE(parsed);
        CHECK(parsed.value.visitorData.empty());
        CHECK(parsed.value.results.size() == 1);
    }
}

TEST_CASE("with no visitor data cached, a later search page carries what the page before it named, as yt-dlp sends it")
{
    TestResolver test;
    test.http->apiBodies["search"] = {readFixture("search_videos.json"), readFixture("search_continuation.json")};
    REQUIRE(test.resolver.search(artist(), 6));
    REQUIRE(test.http->requests.size() == 2);
    CHECK(headerValue(test.http->requests[0], "X-Goog-Visitor-Id").empty());
    CHECK(headerValue(test.http->requests[1], "X-Goog-Visitor-Id") == "FIXTURE");
    CHECK(json::parse(test.http->requests[1].body)["context"]["client"]["visitorData"] == "FIXTURE");

    // It stays out of the cache: the next resolve still fetches a watch page.
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 4);
    CHECK(test.http->requests[2].method == "GET");
    CHECK(headerValue(test.http->requests[3], "X-Goog-Visitor-Id") == VISITOR_DATA);
}

TEST_CASE("with visitor data cached, every search page carries the cached one")
{
    TestResolver test;
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ")); // the watch page, the player
    test.http->apiBodies["search"] = {readFixture("search_videos.json"), readFixture("search_continuation.json")};
    REQUIRE(test.resolver.search(artist(), 6));
    REQUIRE(test.http->requests.size() == 4);
    for (std::size_t i = 2; i < 4; ++i) {
        CAPTURE(i);
        CHECK(headerValue(test.http->requests[i], "X-Goog-Visitor-Id") == VISITOR_DATA);
        CHECK(json::parse(test.http->requests[i].body)["context"]["client"]["visitorData"] == VISITOR_DATA);
    }
}

TEST_CASE("an answer that is no search response is a Parse failure")
{
    const char *const bodies[] = {
        "",
        "not json",
        "[]",
        "{}",
        R"({"responseContext":{}})",
        R"({"contents":{"twoColumnSearchResultsRenderer":{}}})",
        R"({"contents":{"twoColumnSearchResultsRenderer":{"primaryContents":{"sectionListRenderer":{"contents":{}}}}}})",
        R"({"onResponseReceivedCommands":[{"appendContinuationItemsAction":{}}]})",
        R"({"onResponseReceivedCommands":"x"})",
    };
    for (const char *body : bodies) {
        const std::string text = body;
        CAPTURE(text);
        for (const SearchPageKind kind : {SearchPageKind::First, SearchPageKind::Further}) {
            const auto page = parseSearchResponse(body, kind);
            CHECK(page.status.code == Error::Parse);
            CHECK(page.value.results.empty());
        }
    }
}

// ---- through the Resolver ----

TEST_CASE("a search the first page satisfies is one request, as the web client whatever the ladder says")
{
    TestResolver test; // its ladder is visionos alone
    test.http->apiBodies["search"] = {readFixture("search_videos.json")};
    const auto found = test.resolver.search(artist(), 2);
    REQUIRE(found);
    CHECK(idsOf(found.value) == std::vector<std::string>{"MxWXAIWsppY", "oCZugu1ea18"});

    // What the glue sends, not only what the builder builds.
    REQUIRE(test.http->requests.size() == 1);
    const ytres::HttpRequest &sent = test.http->requests[0];
    const ytres::HttpRequest built = searchRequest(webClient(), artist(), "en", "");
    CHECK(sent.method == "POST");
    CHECK(sent.url == SEARCH_URL);
    CHECK(sent.headers == built.headers);
    CHECK(sent.body == built.body);
    CHECK(headerValue(sent, "X-YouTube-Client-Name") == "1");
    CHECK(test.logLine(ytres::LogLevel::Debug, "search page 1: 4 videos") != nullptr);
}

TEST_CASE("a search past the first page sends its continuation, and stops once it holds max")
{
    TestResolver test;
    test.http->apiBodies["search"] = {readFixture("search_videos.json"), readFixture("search_continuation.json")};
    const auto found = test.resolver.search(artist(), 6);
    REQUIRE(found);
    CHECK(idsOf(found.value)
          == std::vector<std::string>{"MxWXAIWsppY", "oCZugu1ea18", "2DiP0mMeaT8", "jyXHU7PtsmI", "g4UDeQTjMYk", "5rNXe7Z1qN0"});
    REQUIRE(test.http->requests.size() == 2);

    const auto first = parseSearchResponse(readFixture("search_videos.json"), SearchPageKind::First);
    REQUIRE(first);
    const ytres::HttpRequest &second = test.http->requests[1];
    CHECK(second.url == SEARCH_URL);
    // With nothing cached, page one's visitor data goes along too.
    CHECK(second.body == searchRequest(webClient(), artist(), "en", first.value.visitorData, first.value.next).body);
    json body = json::parse(second.body);
    CHECK(body["query"] == artist());
    CHECK(body["params"] == "EgIQAfABAQ==");
    CHECK(body["continuation"] == first.value.next.token);
    CHECK(body["clickTracking"]["clickTrackingParams"] == first.value.next.clickTrackingParams);
    CHECK(test.logLine(ytres::LogLevel::Debug, "search page 2: 3 videos") != nullptr);
}

TEST_CASE("a search stops at the page that gives no continuation")
{
    TestResolver test;
    test.http->apiBodies["search"] = {readFixture("search_videos.json"), furtherPage(section(video("aaaaaaaaaaa")))};
    const auto found = test.resolver.search(artist(), 50);
    REQUIRE(found);
    REQUIRE(found.value.size() == 5);
    CHECK(found.value.back().videoId == "aaaaaaaaaaa");
    CHECK(test.http->requests.size() == 2);
}

TEST_CASE("a search stops at a page that brought no videos, continuation or not")
{
    TestResolver test;
    test.http->apiBodies["search"] = {readFixture("search_videos.json"),
                                      furtherPage(section("") + "," + continuationItem("MORE"))};
    const auto found = test.resolver.search(artist(), 50);
    REQUIRE(found);
    CHECK(found.value.size() == 4);
    CHECK(test.http->requests.size() == 2);
}

TEST_CASE("a search reads ten pages at most, whatever max asks for")
{
    // Fresh tokens and new videos on every page: only the cap stops it.
    TestResolver test;
    std::deque<std::string> &bodies = test.http->apiBodies["search"];
    bodies.push_back(firstPage(section(videos(0, 20)) + "," + continuationItem("T1")));
    for (int page = 2; page <= 11; ++page) {
        bodies.push_back(furtherPage(section(videos(page * 100, 20)) + "," + continuationItem("T" + std::to_string(page))));
    }
    const auto found = test.resolver.search(artist(), 1000);
    REQUIRE(found);
    CHECK(test.http->requests.size() == 10);
    CHECK(found.value.size() == 200);
    // Cut, not finished, and the log says so.
    CHECK(test.logLine(ytres::LogLevel::Warning, "The search stopped after 10 pages with 200 videos") != nullptr);

    // Ten pages that end of themselves are no cut.
    TestResolver ten;
    std::deque<std::string> &tenBodies = ten.http->apiBodies["search"];
    tenBodies.push_back(firstPage(section(videos(0, 20)) + "," + continuationItem("T1")));
    for (int page = 2; page <= 10; ++page) {
        const std::string more = page < 10 ? "," + continuationItem("T" + std::to_string(page)) : std::string{};
        tenBodies.push_back(furtherPage(section(videos(page * 100, 20)) + more));
    }
    REQUIRE(ten.resolver.search(artist(), 1000));
    CHECK(ten.http->requests.size() == 10);
    CHECK_FALSE(ten.logged(ytres::LogLevel::Warning));
}

TEST_CASE("a search whose continuation repeats stops there, with each video once")
{
    // Page two hands out its own token again, as a feed going round would.
    TestResolver test;
    test.http->apiBodies["search"] = {
        firstPage(section(videos(0, 20)) + "," + continuationItem("SAME")),
        furtherPage(section(videos(20, 20)) + "," + continuationItem("SAME")),
        furtherPage(section(videos(40, 20)) + "," + continuationItem("SAME")), // never asked for
    };
    const auto found = test.resolver.search(artist(), 200);
    REQUIRE(found);
    CHECK(found.value.size() == 40);
    CHECK(test.http->requests.size() == 2);
    CHECK(test.logLine(ytres::LogLevel::Warning, "leads back to a page already read, after page 2") != nullptr);
}

TEST_CASE("a video YouTube hands out twice is handed on once, and a page of nothing new ends the search")
{
    // Every further page serves page one's videos again under a fresh token.
    TestResolver test;
    test.http->apiBodies["search"] = {
        firstPage(section(videos(0, 20)) + "," + continuationItem("T1")),
        furtherPage(section(videos(0, 20)) + "," + continuationItem("T2")),
        furtherPage(section(videos(0, 20)) + "," + continuationItem("T3")), // never asked for
    };
    const auto found = test.resolver.search(artist(), 50);
    REQUIRE(found);
    CHECK(found.value.size() == 20);
    CHECK(test.http->requests.size() == 2);
    CHECK(test.logLine(ytres::LogLevel::Warning, "repeats on page 2 only videos it already read") != nullptr);

    // Within a page, and across pages beside new videos: each once, in order.
    TestResolver mixed;
    mixed.http->apiBodies["search"] = {
        firstPage(section(video("aaaaaaaaaaa") + "," + video("aaaaaaaaaaa") + "," + video("bbbbbbbbbbb")) + ","
                  + continuationItem("T1")),
        furtherPage(section(video("bbbbbbbbbbb") + "," + video("ccccccccccc"))),
    };
    const auto once = mixed.resolver.search(artist(), 50);
    REQUIRE(once);
    CHECK(idsOf(once.value) == std::vector<std::string>{"aaaaaaaaaaa", "bbbbbbbbbbb", "ccccccccccc"});
    CHECK(mixed.http->requests.size() == 2);
    CHECK_FALSE(mixed.logged(ytres::LogLevel::Warning));
}

TEST_CASE("a search YouTube finds nothing for is Ok and empty, after one request")
{
    TestResolver test;
    test.http->apiBodies["search"] = {readFixture("search_empty.json")};
    const auto found = test.resolver.search("zzqxjv no such thing", 5);
    CHECK(found);
    CHECK(found.value.empty());
    CHECK(test.http->requests.size() == 1);
}

TEST_CASE("a search YouTube refuses over HTTP is Http, with no videos")
{
    TestResolver limited;
    limited.http->apiStatus["search"] = 429;
    limited.http->apiBodies["search"] = {"<html>Too many requests</html>"};
    const auto refused = limited.resolver.search(artist(), 5);
    CHECK(refused.status.code == Error::Http);
    CHECK(refused.value.empty());
    CHECK(limited.logLine(ytres::LogLevel::Debug, "HTTP 429 from https://www.youtube.com/youtubei/v1/search") != nullptr);

    // A redirect CurlHttpClient does not follow, let through by the client.
    TestResolver redirected;
    redirected.http->apiStatus["search"] = 302;
    redirected.http->apiBodies["search"] = {""};
    const auto moved = redirected.resolver.search(artist(), 5);
    CHECK(moved.status.code == Error::Http);
    CHECK(moved.status.message.find("302") != std::string::npos);
}

TEST_CASE("a search answered with something other than JSON is Parse")
{
    TestResolver test;
    test.http->apiBodies["search"] = {"<html>not json</html>"};
    const auto found = test.resolver.search(artist(), 5);
    CHECK(found.status.code == Error::Parse);
    CHECK(found.value.empty());
}

TEST_CASE("a later page that fails returns its failure with the videos read before it")
{
    TestResolver unreadable;
    unreadable.http->apiBodies["search"] = {readFixture("search_videos.json"), "not json"};
    const auto parse = unreadable.resolver.search(artist(), 10);
    CHECK(parse.status.code == Error::Parse);
    CHECK(idsOf(parse.value) == std::vector<std::string>{"MxWXAIWsppY", "oCZugu1ea18", "2DiP0mMeaT8", "jyXHU7PtsmI"});
    CHECK(unreadable.http->requests.size() == 2);

    // Cancelled before page two: page one's videos are still there to use.
    TestResolver cancelled;
    cancelled.http->apiBodies["search"] = {readFixture("search_videos.json"), readFixture("search_continuation.json")};
    ytres::Request request;
    int checks = 0;
    request.cancelled = [&checks] { return ++checks > 1; };
    const auto stopped = cancelled.resolver.search(artist(), 10, request);
    CHECK(stopped.status.code == Error::Cancelled);
    CHECK(stopped.value.size() == 4);
    CHECK(cancelled.http->requests.size() == 1);
}

TEST_CASE("a search for no videos, or for nothing, is BadInput and sends nothing")
{
    TestResolver test;
    CHECK(test.resolver.search(artist(), 0).status.code == Error::BadInput);
    for (const char *query : {"", " ", " \t\r\n ", "\v\f", " \v \f "}) {
        const std::string shown = query;
        CAPTURE(shown);
        CHECK(test.resolver.search(query, 5).status.code == Error::BadInput);
    }
    CHECK(test.http->requests.empty());
}

TEST_CASE("a search query that is not UTF-8 goes out with U+FFFD in place of its broken bytes, and the log says so")
{
    // The Polish l with stroke as code page 1250 writes it, 0xB3.
    TestResolver broken;
    broken.http->apiBodies["search"] = {readFixture("search_videos.json")};
    CHECK(broken.resolver.search("Dawid Podsiad\xB3o", 1));
    REQUIRE(broken.http->requests.size() == 1);
    CHECK(json::parse(broken.http->requests[0].body)["query"] == "Dawid Podsiad\xEF\xBF\xBDo");
    CHECK(broken.logLine(ytres::LogLevel::Warning, "The search query is not valid UTF-8") != nullptr);

    // The same name in UTF-8 goes out as it is, and nothing is logged.
    TestResolver valid;
    valid.http->apiBodies["search"] = {readFixture("search_videos.json")};
    CHECK(valid.resolver.search(artist(), 1));
    REQUIRE(valid.http->requests.size() == 1);
    CHECK(json::parse(valid.http->requests[0].body)["query"] == artist());
    CHECK_FALSE(valid.logged(ytres::LogLevel::Warning));
}

TEST_CASE("a search sends the visitor data a resolve cached, and fetches none itself")
{
    TestResolver warm;
    REQUIRE(warm.resolver.resolve("dQw4w9WgXcQ"));
    warm.http->apiBodies["search"] = {readFixture("search_videos.json")};
    CHECK(warm.resolver.search(artist(), 1));
    REQUIRE(warm.http->requests.size() == 3); // the watch page, the player, the search
    const ytres::HttpRequest &sent = warm.http->requests[2];
    CHECK(sent.url == SEARCH_URL);
    CHECK(headerValue(sent, "X-Goog-Visitor-Id") == VISITOR_DATA);
    CHECK(json::parse(sent.body)["context"]["client"]["visitorData"] == VISITOR_DATA);

    TestResolver cold;
    cold.http->apiBodies["search"] = {readFixture("search_videos.json")};
    CHECK(cold.resolver.search(artist(), 1));
    REQUIRE(cold.http->requests.size() == 1); // no watch page
    CHECK(cold.http->requests[0].url == SEARCH_URL);
    CHECK(headerValue(cold.http->requests[0], "X-Goog-Visitor-Id").empty());
    CHECK(json::parse(cold.http->requests[0].body)["context"]["client"].count("visitorData") == 0);
}

TEST_CASE("a search cancelled up front sends nothing, and a cancel check that throws is Internal")
{
    TestResolver cancelled;
    ytres::Request request;
    request.cancelled = [] { return true; };
    CHECK(cancelled.resolver.search(artist(), 5, request).status.code == Error::Cancelled);
    CHECK(cancelled.http->requests.empty());

    TestResolver broken;
    request.cancelled = []() -> bool { throw std::runtime_error("the check broke"); };
    CHECK(broken.resolver.search(artist(), 5, request).status.code == Error::Internal);
    CHECK(broken.http->requests.empty());
}

TEST_CASE("a search keeps resolve()'s rules for the request timeout and the deadline")
{
    TestResolver zero(WATCH_PAGE, 0ms);
    const auto refused = zero.resolver.search(artist(), 5);
    CHECK(refused.status.code == Error::BadInput);
    CHECK(refused.status.message == "Options::requestTimeout must be positive");
    CHECK(zero.http->requests.empty());

    TestResolver spent;
    ytres::Request request;
    request.deadline = 0ms;
    CHECK(spent.resolver.search(artist(), 5, request).status.code == Error::Timeout);
    CHECK(spent.http->requests.empty());

    // Every page gets the request timeout, never more than the deadline leaves.
    TestResolver tight(WATCH_PAGE, 5s);
    tight.http->apiBodies["search"] = {readFixture("search_videos.json"), readFixture("search_continuation.json")};
    request.deadline = 2s;
    CHECK(tight.resolver.search(artist(), 6, request));
    REQUIRE(tight.http->requests.size() == 2);
    for (const ytres::HttpRequest &sent : tight.http->requests) {
        CHECK(sent.timeout > 0ms);
        CHECK(sent.timeout <= 2000ms);
    }
}

TEST_CASE("a moved-from Resolver refuses a search")
{
    TestResolver test;
    ytres::Resolver taken = std::move(test.resolver);
    const auto moved = test.resolver.search(artist(), 5); // NOLINT(bugprone-use-after-move): that is the point
    CHECK(moved.status.code == Error::BadInput);
    CHECK(taken.search(" ", 5).status.code == Error::BadInput); // the one it moved to still checks its input
    CHECK(test.http->requests.empty());
}

TEST_CASE("watchUrl is the canonical page url")
{
    CHECK(ytres::watchUrl("dQw4w9WgXcQ") == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
}
