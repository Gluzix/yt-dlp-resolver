#include "continuation.h"
#include "fake_http_client.h"
#include "innertube.h"
#include "playlist.h"
#include "test_resolver.h"
#include "url_parse.h"

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
using ytres::Error;
using ytres::parsePlaylistId;
using ytres::innertube::Continuation;
using ytres::innertube::parsePlaylistResponse;
using ytres::innertube::playlistRequest;
using ytres::innertube::videoCount;
using namespace std::chrono_literals;

namespace {

const char *const BROWSE_URL = "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
const char *const CHROME_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36";

// The playlists the fixtures answer: 447 videos over five pages, and 7 on one.
const char *const LONG_PLAYLIST = "PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4";
const char *const SMALL_PLAYLIST = "PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk";

using Headers = std::vector<std::pair<std::string, std::string>>;

// The web client's headers, as for its player request and a search.
const Headers WEB_HEADERS = {
    {"Content-Type", "application/json"},
    {"X-YouTube-Client-Name", "1"},
    {"X-YouTube-Client-Version", "2.20260708.00.00"},
    {"Origin", "https://www.youtube.com"},
    {"User-Agent", CHROME_UA},
};

const ytres::innertube::ClientDef &webClient()
{
    return *ytres::innertube::findClient(ytres::ClientId::Web);
}

std::vector<std::string> idsOf(const std::vector<ytres::PlaylistEntry> &entries)
{
    std::vector<std::string> ids;
    for (const ytres::PlaylistEntry &entry : entries) {
        ids.push_back(entry.videoId);
    }
    return ids;
}

std::vector<std::int64_t> durationsOf(const std::vector<ytres::PlaylistEntry> &entries)
{
    std::vector<std::int64_t> durations;
    for (const ytres::PlaylistEntry &entry : entries) {
        durations.push_back(entry.durationSeconds);
    }
    return durations;
}

// Hand-made pages, in the shapes the fixtures have: a continuation in the
// view model form the new layout uses, and in the renderer form of the old;
// a first page around the contents of its item section, with a continuation
// beside the item section that must never be followed and with the
// sidebar's first stat when given ("12 videos"); a further page around its
// appended items; a lockupViewModel video with a title and a length badge;
// and an old-layout playlistVideoRenderer with whatever extra adds (from a
// comma).

std::string continuationItem(const std::string &token)
{
    return R"({"continuationItemViewModel":{"continuationCommand":{"innertubeCommand":{"continuationCommand":{"token":")"
         + token + R"("}}}}})";
}

std::string continuationRenderer(const std::string &token)
{
    return R"({"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":")" + token
         + R"("}}}})";
}

std::string firstPage(const std::string &items, const std::string &stat = {})
{
    const std::string sidebar =
        stat.empty() ? std::string{}
                     : R"(,"sidebar":{"playlistSidebarRenderer":{"items":[{"playlistSidebarPrimaryInfoRenderer":{"stats":[)"
                           R"({"simpleText":")" + stat + R"("}]}}]}})";
    return R"({"contents":{"twoColumnBrowseResultsRenderer":{"tabs":[{"tabRenderer":{"content":{"sectionListRenderer":)"
           R"({"contents":[{"itemSectionRenderer":{"contents":[)"
         + items + "]}}," + continuationItem("BESIDE") + "]}}}}]}}" + sidebar + "}";
}

std::string furtherPage(const std::string &items)
{
    return R"({"onResponseReceivedActions":[{"appendContinuationItemsAction":{"continuationItems":[)" + items + "]}}]}";
}

std::string lockup(const std::string &videoId, const std::string &title = "T", const std::string &length = "1:00",
                   const std::string &contentType = "LOCKUP_CONTENT_TYPE_VIDEO")
{
    return R"({"lockupViewModel":{"contentId":")" + videoId + R"(","contentType":")" + contentType
         + R"(","metadata":{"lockupMetadataViewModel":{"title":{"content":")" + title
         + R"("}}},"contentImage":{"thumbnailViewModel":{"overlays":[{"thumbnailBottomOverlayViewModel":{"badges":[)"
           R"({"thumbnailBadgeViewModel":{"text":")" + length + R"("}}]}}]}}}})";
}

std::string renderer(const std::string &videoId, const std::string &title, const std::string &extra = {})
{
    return R"({"playlistVideoRenderer":{"videoId":")" + videoId + R"(","title":{"runs":[{"text":")" + title + R"("}]})"
         + extra + "}}";
}

// The old layout, which no playlist came in on 2026-10-01 but yt-dlp still
// reads. Synthetic, written by hand from docs/m4-plan.md rather than
// recorded: a playlistVideoListRenderer in the item section holds two
// entries, one YouTube calls unplayable and the continuation; the old
// header holds the title and the count, and there is no metadata.
std::string oldLayoutPage()
{
    const std::string entries =
        renderer("aaaaaaaaaaa", "First", R"(,"lengthSeconds":"212","lengthText":{"simpleText":"1:00"})") + ","
        + renderer("bbbbbbbbbbb", "Second", R"(,"lengthText":{"simpleText":"4:36"})") + ","
        + renderer("ccccccccccc", "[Private video]", R"(,"isPlayable":false)") + "," + continuationRenderer("OLD");
    return R"({"contents":{"twoColumnBrowseResultsRenderer":{"tabs":[{"tabRenderer":{"content":{"sectionListRenderer":)"
           R"({"contents":[{"itemSectionRenderer":{"contents":[{"playlistVideoListRenderer":{"contents":[)"
         + entries
         + R"(]}}]}}]}}}}]}},"header":{"playlistHeaderRenderer":{"title":{"simpleText":"Old Layout"},)"
           R"("numVideosText":{"runs":[{"text":"3"},{"text":" videos"}]}}}})";
}

}

// ---- which playlist ----

TEST_CASE("every playlist link form gives the playlist id")
{
    const std::string id = LONG_PLAYLIST;
    const std::string accepted[] = {
        "https://www.youtube.com/playlist?list=" + id,
        "https://www.youtube.com/playlist?list=" + id + "&si=FIXTURE",
        "https://youtube.com/playlist?list=" + id,
        "https://m.youtube.com/playlist?list=" + id,
        "https://music.youtube.com/playlist?list=" + id,
        "https://www.youtube.com/watch?v=s7d2d8FhevU&list=" + id,
        "https://www.youtube.com/watch?v=s7d2d8FhevU&list=" + id + "&index=2",
        "https://www.youtube.com/watch?list=" + id,
        "https://youtu.be/s7d2d8FhevU?list=" + id,
        "https://youtu.be/s7d2d8FhevU?si=FIXTURE&list=" + id,
        "http://www.youtube.com/playlist?list=" + id,
        "www.youtube.com/playlist?list=" + id,
        "HTTPS://WWW.YouTube.com/playlist?list=" + id,
        "https://www.youtube.com/playlist?list=" + id + "#top",
        id,
        "  " + id + "\n",
    };
    for (const std::string &input : accepted) {
        CAPTURE(input);
        const auto result = parsePlaylistId(input);
        REQUIRE(result);
        CHECK(result.value == id);
    }
}

TEST_CASE("every public kind of playlist id is accepted, with ten or more characters after its prefix")
{
    for (const char *prefix : {"PL", "UU", "FL", "OLAK5uy_", "EC", "UL", "PU"}) {
        const std::string id = std::string(prefix) + "0123456789";
        CAPTURE(id);
        const auto result = parsePlaylistId(id);
        REQUIRE(result);
        CHECK(result.value == id);
        CHECK(parsePlaylistId(std::string(prefix) + "012345678").status.code == Error::BadInput); // nine
    }
}

TEST_CASE("a mix, Watch Later, Liked videos and the other lists made for one viewer are BadInput, and say so")
{
    const char *const personal[] = {
        "RDdQw4w9WgXcQ",
        "RDMM",
        "RDMMdQw4w9WgXcQ",
        "WL",
        "LL",
        "LM",
        "LLxxxxxxxxxxxxxxxxxxxxxx",
        "TLGGxxxxxxxxxxxxxxxx",
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=RDdQw4w9WgXcQ&start_radio=1",
        "https://www.youtube.com/playlist?list=WL",
        "https://www.youtube.com/playlist?list=LL",
        "https://music.youtube.com/playlist?list=LM",
    };
    for (const char *input : personal) {
        const std::string text = input; // a std::string, so a failure prints the text, not the pointer
        CAPTURE(text);
        const auto result = parsePlaylistId(input);
        CHECK(result.status.code == Error::BadInput);
        CHECK(result.status.message.find("signed-in viewer") != std::string::npos);
        CHECK(result.value.empty());
    }
}

TEST_CASE("anything that is not a public playlist link or id is BadInput")
{
    const std::string id = LONG_PLAYLIST;
    const std::string rejected[] = {
        "",
        "   ",
        "PL012345678",                                        // nine characters after the prefix
        "XX0123456789",                                       // no prefix of a playlist
        "PL0123456789.",                                      // outside the alphabet
        "dQw4w9WgXcQ",                                        // a video's id
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ",        // a video's link
        "https://www.youtube.com/playlist",
        "https://www.youtube.com/playlist?list=",
        "https://www.youtube.com/playlist?lists=" + id,
        "https://www.youtube.com/playlist?list=" + id + "%20", // still percent-encoded
        "https://www.youtube.com/playlist/x?list=" + id,
        "https://www.youtube.com/channel/UCSHZKyawb77ixDdsGog4iWA?list=" + id,
        "https://vimeo.com/playlist?list=" + id,
        "https://notyoutube.com/playlist?list=" + id,
        "https://www.youtube.com.evil.example/playlist?list=" + id,
        "https://user@www.youtube.com/playlist?list=" + id,
        "https://www.youtube.com:8080/playlist?list=" + id,
        "ftp://www.youtube.com/playlist?list=" + id,
    };
    for (const std::string &input : rejected) {
        CAPTURE(input);
        const auto result = parsePlaylistId(input);
        CHECK(result.status.code == Error::BadInput);
        CHECK(result.status.message == "Not a YouTube playlist link or id");
        CHECK(result.value.empty());
    }
}

// ---- the request ----

TEST_CASE("the browse request is the one in the notes, byte for byte")
{
    const auto request = playlistRequest(webClient(), LONG_PLAYLIST, "en", "");
    CHECK(request.method == "POST");
    CHECK(request.url == BROWSE_URL);
    CHECK(request.headers == WEB_HEADERS);
    // The context, then browseId, and nothing else: no params.
    CHECK(request.body == R"({"context":{"client":{"clientName":"WEB","clientVersion":"2.20260708.00.00","hl":"en",)"
                          R"("timeZone":"UTC","utcOffsetMinutes":0}},"browseId":"VLPLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4"})");
}

TEST_CASE("visitor data and the language go out with a browse as with the player")
{
    const auto request = playlistRequest(webClient(), LONG_PLAYLIST, "pl", VISITOR_DATA);
    Headers expectedHeaders = WEB_HEADERS;
    expectedHeaders.emplace_back("X-Goog-Visitor-Id", VISITOR_DATA);
    CHECK(request.headers == expectedHeaders);

    json body = json::parse(request.body);
    CHECK(body["context"]["client"]["visitorData"] == VISITOR_DATA);
    CHECK(body["context"]["client"]["hl"] == "pl");
    CHECK(body["browseId"] == "VLPLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4");
}

TEST_CASE("a further page of a playlist asks with the continuation alone, with no browseId, as yt-dlp does")
{
    const auto request = playlistRequest(webClient(), LONG_PLAYLIST, "en", "", Continuation{"TOKEN", "CTP"});
    CHECK(request.method == "POST");
    CHECK(request.url == BROWSE_URL);
    CHECK(request.headers == WEB_HEADERS);
    CHECK(request.body == R"({"context":{"client":{"clientName":"WEB","clientVersion":"2.20260708.00.00","hl":"en",)"
                          R"("timeZone":"UTC","utcOffsetMinutes":0}},"continuation":"TOKEN",)"
                          R"("clickTracking":{"clickTrackingParams":"CTP"}})");

    // No clickTrackingParams, no clickTracking.
    json bare = json::parse(playlistRequest(webClient(), LONG_PLAYLIST, "en", "", Continuation{"TOKEN", ""}).body);
    CHECK(bare["continuation"] == "TOKEN");
    CHECK(bare.count("clickTracking") == 0);
    CHECK(bare.count("browseId") == 0);
}

// ---- the reader ----

TEST_CASE("videoCount reads the count a stat starts with, and drops the separators")
{
    CHECK(videoCount("447 episodes") == 447);
    CHECK(videoCount("7 videos") == 7);
    CHECK(videoCount("6,000 videos") == 6000);
    CHECK(videoCount("No videos") == 0);
    CHECK(videoCount("1,234,567 videos") == 1234567);
    CHECK(videoCount("") == 0);
    CHECK(videoCount(",5 videos") == 0);
    CHECK(videoCount("1234567890 videos") == 0); // more than nine digits: unknown, not wrapped
}

TEST_CASE("the recorded small playlist: its title, its count, seven videos and no continuation to follow")
{
    const auto page = parsePlaylistResponse(readFixture("playlist_small.json"));
    REQUIRE(page);
    CHECK(page.value.title == "Our favourite Lex Fridman Podcast Episodes");
    CHECK(page.value.totalCount == 7);
    CHECK(idsOf(page.value.entries)
          == std::vector<std::string>{"DxREm3s1scA", "XW0QZmtbjvs", "4dC_nRYIDZU", "Fx0G6DHMfXM", "Iau6W5pjy9Y",
                                      "hGRNUw559SE", "KOwm7GUjcg8"});
    CHECK(durationsOf(page.value.entries) == std::vector<std::int64_t>{9108, 10921, 11840, 6750, 5180, 15320, 7499});
    REQUIRE_FALSE(page.value.entries.empty());
    CHECK(page.value.entries[0].title
          == "Elon Musk: SpaceX, Mars, Tesla Autopilot, Self-Driving, Robotics, and AI | Lex Fridman Podcast #252");
    // Its section list holds a continuationItemViewModel beside the item
    // section, which loads something else: there is no next page.
    CHECK(page.value.next.token.empty());
    CHECK(page.value.visitorData == "FIXTURE");
}

TEST_CASE("the recorded first page of a long playlist: the continuation among the entries, not the one beside them")
{
    const auto page = parsePlaylistResponse(readFixture("playlist_long_first.json"));
    REQUIRE(page);
    CHECK(page.value.title == "Lex Fridman Podcast");
    CHECK(page.value.totalCount == 447); // "447 episodes"
    CHECK(idsOf(page.value.entries) == std::vector<std::string>{"s7d2d8FhevU", "NYFGCESmikA", "l6USUAIKJls"});
    CHECK(durationsOf(page.value.entries) == std::vector<std::int64_t>{13016, 18951, 11578});
    REQUIRE(page.value.entries.size() == 3);
    CHECK(page.value.entries[1].title
          == "DHH: Future of Programming, AI, Agentic Engineering, Vibe Coding & Linux | Lex Fridman Podcast #501");
    CHECK(page.value.next.token.rfind("4qmFsgKBARIk", 0) == 0);
    CHECK(page.value.next.token.rfind("4qmFsgJbEiRW", 0) != 0); // the section list's, which loads something else
    CHECK(page.value.next.clickTrackingParams.rfind("CCgQuy8YACIT", 0) == 0);
}

TEST_CASE("the recorded second page: three videos and the next continuation, and no title or count of its own")
{
    const auto page = parsePlaylistResponse(readFixture("playlist_long_continuation.json"));
    REQUIRE(page);
    CHECK(idsOf(page.value.entries) == std::vector<std::string>{"2yHr9DPnSzk", "r4wLXNydzeY", "JN3KPFbWCy8"});
    CHECK(durationsOf(page.value.entries) == std::vector<std::int64_t>{5942, 12402, 8207});
    CHECK(page.value.next.token.rfind("4qmFsgJ_EiRW", 0) == 0);
    CHECK(page.value.next.clickTrackingParams.rfind("CAAQhGciEwj6", 0) == 0);
    // It repeats the metadata and the sidebar beside a stub contents; the
    // title and the count are the first page's business.
    CHECK(page.value.title.empty());
    CHECK(page.value.totalCount == 0);
    CHECK(page.value.visitorData == "FIXTURE");
}

TEST_CASE("the recorded missing playlist is Unavailable, in YouTube's words")
{
    const auto page = parsePlaylistResponse(readFixture("playlist_missing.json"));
    CHECK(page.status.code == Error::Unavailable);
    CHECK(page.status.message == "The playlist does not exist.");
    CHECK(page.value.entries.empty());
}

TEST_CASE("the old layout: entries in a playlistVideoListRenderer, the title and the count in the old header")
{
    const auto page = parsePlaylistResponse(oldLayoutPage());
    REQUIRE(page);
    CHECK(page.value.title == "Old Layout");
    CHECK(page.value.totalCount == 3);
    REQUIRE(page.value.entries.size() == 2); // the unplayable one read past
    CHECK(page.value.entries[0].videoId == "aaaaaaaaaaa");
    CHECK(page.value.entries[0].title == "First");
    CHECK(page.value.entries[0].durationSeconds == 212); // lengthSeconds before lengthText
    CHECK(page.value.entries[1].videoId == "bbbbbbbbbbb");
    CHECK(page.value.entries[1].title == "Second");
    CHECK(page.value.entries[1].durationSeconds == 276); // lengthText when there are no lengthSeconds
    CHECK(page.value.next.token == "OLD");

    // A further page of the old layout holds the same entries directly.
    const auto further = parsePlaylistResponse(
        furtherPage(renderer("eeeeeeeeeee", "Fourth", R"(,"lengthSeconds":30)") + "," + continuationRenderer("OLDER")));
    REQUIRE(further);
    REQUIRE(further.value.entries.size() == 1);
    CHECK(further.value.entries[0].videoId == "eeeeeeeeeee");
    CHECK(further.value.entries[0].durationSeconds == 30);
    CHECK(further.value.next.token == "OLDER");
}

TEST_CASE("what is not a playable video with a valid id is read past, in either layout")
{
    const std::string lockups = lockup("aaaaaaaaaaa") + "," + lockup("bbbbbbbbbbb", "[Private video]") + ","
                              + lockup("ccccccccccc", "[Deleted video]") + "," + lockup("ddddddddddd", "") + ","
                              + lockup("eeeeeeeeeee", "A playlist", "1:00", "LOCKUP_CONTENT_TYPE_PLAYLIST") + ","
                              + lockup("short") + "," + R"({"lockupViewModel":"not an object"},42,null,{"messageRenderer":{}},)"
                              + lockup("fffffffffff");
    const auto fresh = parsePlaylistResponse(firstPage(lockups));
    REQUIRE(fresh);
    CHECK(idsOf(fresh.value.entries) == std::vector<std::string>{"aaaaaaaaaaa", "fffffffffff"});

    const std::string renderers = renderer("aaaaaaaaaaa", "Kept") + "," + renderer("bbbbbbbbbbb", "[Private video]") + ","
                                + renderer("ccccccccccc", "[Deleted video]") + "," + renderer("ddddddddddd", "") + ","
                                + renderer("eeeeeeeeeee", "Unplayable", R"(,"isPlayable":false)") + ","
                                + renderer("fffffffffff", "Playable", R"(,"isPlayable":true)") + ","
                                + renderer("ggggggggggg", "Says nothing a boolean would", R"(,"isPlayable":"no")") + ","
                                + renderer("short", "Short id");
    const auto old = parsePlaylistResponse(furtherPage(renderers));
    REQUIRE(old);
    CHECK(idsOf(old.value.entries) == std::vector<std::string>{"aaaaaaaaaaa", "fffffffffff", "ggggggggggg"});
}

TEST_CASE("a lockup's length is its first badge's, under either overlay yt-dlp reads")
{
    const std::string otherOverlay =
        R"({"lockupViewModel":{"contentId":"bbbbbbbbbbb","contentType":"LOCKUP_CONTENT_TYPE_VIDEO",)"
        R"("metadata":{"lockupMetadataViewModel":{"title":{"content":"B"}}},"contentImage":{"thumbnailViewModel":{"overlays":[)"
        R"({"thumbnailHoverOverlayToggleActionsViewModel":{}},{"thumbnailOverlayBadgeViewModel":{"thumbnailBadges":[)"
        R"({"thumbnailBadgeViewModel":{"text":"12:34"}},{"thumbnailBadgeViewModel":{"text":"9:59"}}]}}]}}}})";
    const std::string noBadge = R"({"lockupViewModel":{"contentId":"ccccccccccc","contentType":"LOCKUP_CONTENT_TYPE_VIDEO",)"
                                R"("metadata":{"lockupMetadataViewModel":{"title":{"content":"C"}}}}})";
    const auto page = parsePlaylistResponse(firstPage(lockup("aaaaaaaaaaa", "A", "2:31:48") + "," + otherOverlay + ","
                                                      + noBadge + "," + lockup("ddddddddddd", "D", "LIVE")));
    REQUIRE(page);
    CHECK(durationsOf(page.value.entries) == std::vector<std::int64_t>{9108, 754, 0, 0});
}

TEST_CASE("the continuation to follow is the first among the entries, never the one beside the item section")
{
    // firstPage() puts one beside the item section, as YouTube does.
    const auto among =
        parsePlaylistResponse(firstPage(lockup("aaaaaaaaaaa") + "," + continuationItem("AMONG") + "," + continuationItem("LATER")));
    REQUIRE(among);
    CHECK(among.value.next.token == "AMONG");

    const auto beside = parsePlaylistResponse(firstPage(lockup("aaaaaaaaaaa")));
    REQUIRE(beside);
    CHECK(beside.value.next.token.empty());

    // A further page's continuation among its entries, under either key.
    const auto endpoints = parsePlaylistResponse(
        R"({"onResponseReceivedEndpoints":[{"appendContinuationItemsAction":{"continuationItems":[)" + lockup("aaaaaaaaaaa")
        + "," + continuationItem("NEXT") + "]}}]}");
    REQUIRE(endpoints);
    CHECK(idsOf(endpoints.value.entries) == std::vector<std::string>{"aaaaaaaaaaa"});
    CHECK(endpoints.value.next.token == "NEXT");
}

TEST_CASE("an alert beside real contents is no failure, and an ERROR alert in place of them is Unavailable")
{
    // YouTube says beside a playlist that it hides the unavailable videos.
    const std::string page = firstPage(lockup("aaaaaaaaaaa"));
    const auto shown = parsePlaylistResponse(
        R"({"alerts":[{"alertWithButtonRenderer":{"type":"INFO","text":{"simpleText":"Unavailable videos are hidden"}}}],)"
        + page.substr(1));
    REQUIRE(shown);
    CHECK(idsOf(shown.value.entries) == std::vector<std::string>{"aaaaaaaaaaa"});

    // Hand-written: the first ERROR alert's text, whichever renderer holds it.
    const auto refused = parsePlaylistResponse(
        R"({"alerts":[{"alertRenderer":{"type":"INFO","text":{"simpleText":"Not this one"}}},)"
        R"({"alertWithButtonRenderer":{"type":"ERROR","text":{"runs":[{"text":"Hand-written "},{"text":"reason."}]}}}]})");
    CHECK(refused.status.code == Error::Unavailable);
    CHECK(refused.status.message == "Hand-written reason.");

    // An alert of another kind in place of a playlist explains nothing.
    CHECK(parsePlaylistResponse(R"({"alerts":[{"alertRenderer":{"type":"WARNING","text":{"simpleText":"x"}}}]})").status.code
          == Error::Parse);
}

TEST_CASE("an ERROR alert beside a section list that holds no entry is Unavailable, whatever renderer holds it")
{
    // Hand-written: a renderer name YouTube has not used, a count of 0, and
    // alerts that are no objects or hold none to read past first.
    const std::string empty = firstPage("", "No videos");
    const auto refused = parsePlaylistResponse(
        R"({"alerts":["x",42,{"plainValue":"y"},{"alertRendererOfTomorrow":)"
        R"({"type":"ERROR","text":{"simpleText":"Hand-written reason."}}}],)"
        + empty.substr(1));
    CHECK(refused.status.code == Error::Unavailable);
    CHECK(refused.status.message == "Hand-written reason.");
    CHECK(refused.value.entries.empty());

    // The same alert beside a count above 0: still YouTube's refusal, not Parse.
    const auto counted = parsePlaylistResponse(
        R"({"alerts":[{"alertRenderer":{"type":"ERROR","text":{"simpleText":"Hand-written reason."}}}],)"
        + firstPage("", "12 videos").substr(1));
    CHECK(counted.status.code == Error::Unavailable);

    // An alert of another kind, in a renderer of any name, beside real entries
    // is still Ok; and beside none, with a count of 0, an empty playlist.
    const auto shown = parsePlaylistResponse(
        R"({"alerts":[{"alertRendererOfTomorrow":{"type":"INFO","text":{"simpleText":"Hidden videos"}}}],)"
        + firstPage(lockup("aaaaaaaaaaa")).substr(1));
    REQUIRE(shown);
    CHECK(idsOf(shown.value.entries) == std::vector<std::string>{"aaaaaaaaaaa"});
    const auto quiet = parsePlaylistResponse(
        R"({"alerts":[{"alertRenderer":{"type":"INFO","text":{"simpleText":"Hidden videos"}}}],)" + empty.substr(1));
    REQUIRE(quiet);
    CHECK(quiet.value.entries.empty());
}

TEST_CASE("a first page that counts videos but holds none the library can read is Parse")
{
    // A playlist as it would look had YouTube moved its entries into a view
    // model the reader does not know, as it moved them into lockupViewModel.
    const std::string unknownItem = R"({"videoCardViewModel":{"videoId":"aaaaaaaaaaa"}})";
    const auto moved = parsePlaylistResponse(firstPage(unknownItem, "12 videos"));
    CHECK(moved.status.code == Error::Parse);
    CHECK(moved.status.message == "The playlist counts videos but holds none the library can read");
    CHECK(moved.value.entries.empty());

    // Nothing counted and nothing there is an empty playlist.
    const auto empty = parsePlaylistResponse(firstPage(unknownItem, "No videos"));
    REQUIRE(empty);
    CHECK(empty.value.entries.empty());
    CHECK(empty.value.totalCount == 0);
    CHECK(parsePlaylistResponse(firstPage(unknownItem)));
    CHECK(parsePlaylistResponse(firstPage("")));

    // A further page with nothing readable is the end of the list, not a failure.
    const auto further = parsePlaylistResponse(furtherPage(unknownItem));
    REQUIRE(further);
    CHECK(further.value.entries.empty());
    CHECK(further.value.next.token.empty());

    // Through the Resolver: a failure the caller falls back on, not an empty playlist.
    TestResolver counted;
    counted.http->apiBodies["browse"] = {firstPage(unknownItem, "12 videos")};
    CHECK(counted.resolver.playlist(LONG_PLAYLIST, 5).status.code == Error::Parse);

    TestResolver uncounted;
    uncounted.http->apiBodies["browse"] = {firstPage(unknownItem, "No videos")};
    const auto listed = uncounted.resolver.playlist(LONG_PLAYLIST, 5);
    REQUIRE(listed);
    CHECK(listed.value.entries.empty());
}

TEST_CASE("a playlist page names the visitor YouTube took the caller for, when it may go out as a header")
{
    const std::string page = firstPage(lockup("aaaaaaaaaaa"));
    const auto without = parsePlaylistResponse(page);
    REQUIRE(without);
    CHECK(without.value.visitorData.empty());

    const auto fit = parsePlaylistResponse(R"({"responseContext":{"visitorData":"CgtGSVhUVVJF%3D%3D"},)" + page.substr(1));
    REQUIRE(fit);
    CHECK(fit.value.visitorData == "CgtGSVhUVVJF%3D%3D");

    // The watch page's rule: never a value that could not travel as a header.
    const auto refused =
        parsePlaylistResponse(R"({"responseContext":{"visitorData":"CgtG\r\nX-Injected: 1"},)" + page.substr(1));
    REQUIRE(refused);
    CHECK(refused.value.visitorData.empty());
    CHECK(refused.value.entries.size() == 1);
}

TEST_CASE("an answer that is no playlist response is a Parse failure")
{
    const char *const bodies[] = {
        "",
        "not json",
        "[]",
        "{}",
        R"({"responseContext":{}})",
        R"({"contents":{}})",
        R"({"contents":{"twoColumnBrowseResultsRenderer":{"tabs":[]}}})",
        R"({"contents":{"twoColumnBrowseResultsRenderer":{"tabs":"x"}}})",
        R"({"contents":{"twoColumnBrowseResultsRenderer":{"tabs":[{"tabRenderer":{"selected":true}}]}}})", // a further page's stub
        R"({"onResponseReceivedActions":[{"appendContinuationItemsAction":{}}]})",
        R"({"onResponseReceivedActions":"x"})",
        R"({"alerts":"x"})",
        R"({"alerts":["x",42,{"alertRenderer":"ERROR"},{"alertRenderer":{"type":"INFO"}}]})",
    };
    for (const char *body : bodies) {
        const std::string text = body;
        CAPTURE(text);
        const auto page = parsePlaylistResponse(body);
        CHECK(page.status.code == Error::Parse);
        CHECK(page.value.entries.empty());
    }
}

// ---- through the Resolver ----

TEST_CASE("a playlist the first page satisfies is one request, as the web client whatever the ladder says")
{
    TestResolver test; // its ladder is visionos alone
    test.http->apiBodies["browse"] = {readFixture("playlist_long_first.json")};
    const auto list = test.resolver.playlist("https://www.youtube.com/playlist?list=" + std::string(LONG_PLAYLIST), 2);
    REQUIRE(list);
    CHECK(list.value.playlistId == LONG_PLAYLIST);
    CHECK(list.value.title == "Lex Fridman Podcast");
    CHECK(list.value.totalCount == 447);
    CHECK(idsOf(list.value.entries) == std::vector<std::string>{"s7d2d8FhevU", "NYFGCESmikA"});

    // What the glue sends, not only what the builder builds; no watch page.
    REQUIRE(test.http->requests.size() == 1);
    const ytres::HttpRequest &sent = test.http->requests[0];
    const ytres::HttpRequest built = playlistRequest(webClient(), LONG_PLAYLIST, "en", "");
    CHECK(sent.method == "POST");
    CHECK(sent.url == BROWSE_URL);
    CHECK(sent.headers == built.headers);
    CHECK(sent.body == built.body);
    CHECK(headerValue(sent, "X-YouTube-Client-Name") == "1");
    CHECK(test.logLine(ytres::LogLevel::Debug, "playlist page 1: 3 videos") != nullptr);
}

TEST_CASE("a playlist past the first page sends the continuation among the entries, and stops once it holds max")
{
    TestResolver test;
    test.http->apiBodies["browse"] = {readFixture("playlist_long_first.json"), readFixture("playlist_long_continuation.json")};
    const auto list = test.resolver.playlist(LONG_PLAYLIST, 5);
    REQUIRE(list);
    CHECK(idsOf(list.value.entries)
          == std::vector<std::string>{"s7d2d8FhevU", "NYFGCESmikA", "l6USUAIKJls", "2yHr9DPnSzk", "r4wLXNydzeY"});
    CHECK(list.value.title == "Lex Fridman Podcast"); // the first page's, not taken from the second
    CHECK(list.value.totalCount == 447);
    REQUIRE(test.http->requests.size() == 2);

    const auto first = parsePlaylistResponse(readFixture("playlist_long_first.json"));
    REQUIRE(first);
    const ytres::HttpRequest &second = test.http->requests[1];
    CHECK(second.url == BROWSE_URL);
    // With nothing cached, page one's visitor data goes along too.
    CHECK(second.body == playlistRequest(webClient(), LONG_PLAYLIST, "en", first.value.visitorData, first.value.next).body);
    json body = json::parse(second.body);
    CHECK(body.count("browseId") == 0);
    CHECK(body["continuation"].get<std::string>().rfind("4qmFsgKBARIk", 0) == 0);
    CHECK(body["clickTracking"]["clickTrackingParams"].get<std::string>() == first.value.next.clickTrackingParams);
    CHECK(test.logLine(ytres::LogLevel::Debug, "playlist page 2: 3 videos") != nullptr);
}

TEST_CASE("a playlist with no continuation among its entries is one request, whatever max asks for")
{
    TestResolver test;
    test.http->apiBodies["browse"] = {readFixture("playlist_small.json")};
    const auto list = test.resolver.playlist(SMALL_PLAYLIST, 50);
    REQUIRE(list);
    CHECK(list.value.playlistId == SMALL_PLAYLIST);
    CHECK(list.value.totalCount == 7);
    CHECK(list.value.entries.size() == 7);
    CHECK(test.http->requests.size() == 1);
}

TEST_CASE("a page that brought no entry still leads on, unlike a search's")
{
    // Nothing on page two anyone may watch, should YouTube ever list such videos.
    TestResolver test;
    test.http->apiBodies["browse"] = {
        firstPage(lockup("aaaaaaaaaaa") + "," + continuationItem("T1")),
        furtherPage(lockup("bbbbbbbbbbb", "[Private video]") + "," + continuationItem("T2")),
        furtherPage(lockup("ccccccccccc")),
    };
    const auto list = test.resolver.playlist(LONG_PLAYLIST, 50);
    REQUIRE(list);
    CHECK(idsOf(list.value.entries) == std::vector<std::string>{"aaaaaaaaaaa", "ccccccccccc"});
    CHECK(test.http->requests.size() == 3);
}

TEST_CASE("three pages in a row that bring no video end the list, each with a fresh token")
{
    TestResolver test;
    test.http->apiBodies["browse"] = {
        firstPage(lockup("aaaaaaaaaaa") + "," + continuationItem("T1")),
        furtherPage(continuationItem("T2")),
        furtherPage(lockup("bbbbbbbbbbb", "[Private video]") + "," + continuationItem("T3")),
        furtherPage(continuationItem("T4")),
        furtherPage(lockup("ccccccccccc")), // never asked for
    };
    const auto list = test.resolver.playlist(LONG_PLAYLIST, 50);
    REQUIRE(list);
    CHECK(idsOf(list.value.entries) == std::vector<std::string>{"aaaaaaaaaaa"});
    CHECK(test.http->requests.size() == 4);
    CHECK(test.logLine(ytres::LogLevel::Warning, "brought no video on 3 pages in a row; stopping after page 4") != nullptr);
}

TEST_CASE("a page with a video between empty ones starts the count of empty pages again")
{
    TestResolver test;
    test.http->apiBodies["browse"] = {
        firstPage(lockup("aaaaaaaaaaa") + "," + continuationItem("T1")),
        furtherPage(continuationItem("T2")),
        furtherPage(continuationItem("T3")),
        furtherPage(lockup("bbbbbbbbbbb") + "," + continuationItem("T4")),
        furtherPage(continuationItem("T5")),
        furtherPage(continuationItem("T6")),
        furtherPage(lockup("ccccccccccc")),
    };
    const auto list = test.resolver.playlist(LONG_PLAYLIST, 50);
    REQUIRE(list);
    CHECK(idsOf(list.value.entries) == std::vector<std::string>{"aaaaaaaaaaa", "bbbbbbbbbbb", "ccccccccccc"});
    CHECK(test.http->requests.size() == 7);
    CHECK_FALSE(test.logged(ytres::LogLevel::Warning));
}

TEST_CASE("a playlist whose continuation repeats stops there, with what it read")
{
    // The second page's token is sent; the third page hands it out again.
    TestResolver test;
    test.http->apiBodies["browse"] = {readFixture("playlist_long_first.json"), readFixture("playlist_long_continuation.json"),
                                      readFixture("playlist_long_continuation.json")};
    const auto list = test.resolver.playlist(LONG_PLAYLIST, 50);
    REQUIRE(list);
    CHECK(test.http->requests.size() == 3);
    CHECK(list.value.entries.size() == 9);
    CHECK(test.logLine(ytres::LogLevel::Warning, "leads back to a page already read, after page 3") != nullptr);

    // A page that leads back to the first page's token stops as well.
    TestResolver back;
    back.http->apiBodies["browse"] = {firstPage(lockup("aaaaaaaaaaa") + "," + continuationItem("T1")),
                                      furtherPage(lockup("bbbbbbbbbbb") + "," + continuationItem("T1"))};
    const auto looped = back.resolver.playlist(LONG_PLAYLIST, 50);
    REQUIRE(looped);
    CHECK(idsOf(looped.value.entries) == std::vector<std::string>{"aaaaaaaaaaa", "bbbbbbbbbbb"});
    CHECK(back.http->requests.size() == 2);
}

TEST_CASE("a playlist reads two hundred pages at most, whatever max asks for")
{
    TestResolver test;
    std::deque<std::string> &bodies = test.http->apiBodies["browse"];
    bodies.push_back(firstPage(lockup("aaaaaaaaaaa") + "," + continuationItem("T1")));
    for (int i = 2; i <= 201; ++i) {
        bodies.push_back(furtherPage(lockup("bbbbbbbbbbb") + "," + continuationItem("T" + std::to_string(i))));
    }
    const auto list = test.resolver.playlist(LONG_PLAYLIST, 1000);
    REQUIRE(list);
    CHECK(test.http->requests.size() == 200);
    CHECK(list.value.entries.size() == 200);
    // Cut, not finished, and the log says so.
    CHECK(test.logLine(ytres::LogLevel::Warning, "runs past 200 pages; stopping with 200 videos") != nullptr);
}

TEST_CASE("a playlist that does not exist is Unavailable, in YouTube's words, after one request")
{
    TestResolver test;
    test.http->apiBodies["browse"] = {readFixture("playlist_missing.json")};
    const auto list = test.resolver.playlist("PL" + std::string(32, 'x'), 5); // the id the fixture was scrubbed to
    CHECK(list.status.code == Error::Unavailable);
    CHECK(list.status.message == "The playlist does not exist.");
    CHECK(list.value.entries.empty());
    CHECK(test.http->requests.size() == 1);
}

TEST_CASE("a playlist YouTube refuses over HTTP is Http, with nothing listed")
{
    TestResolver limited;
    limited.http->apiStatus["browse"] = 429;
    limited.http->apiBodies["browse"] = {"<html>Too many requests</html>"};
    const auto refused = limited.resolver.playlist(LONG_PLAYLIST, 5);
    CHECK(refused.status.code == Error::Http);
    CHECK(refused.value.entries.empty());
    CHECK(limited.logLine(ytres::LogLevel::Debug, "HTTP 429 from https://www.youtube.com/youtubei/v1/browse") != nullptr);

    // A redirect CurlHttpClient does not follow, let through by the client.
    TestResolver redirected;
    redirected.http->apiStatus["browse"] = 302;
    redirected.http->apiBodies["browse"] = {""};
    const auto moved = redirected.resolver.playlist(LONG_PLAYLIST, 5);
    CHECK(moved.status.code == Error::Http);
    CHECK(moved.status.message.find("302") != std::string::npos);
}

TEST_CASE("a later playlist page that fails returns its failure with the playlist as read before it")
{
    TestResolver unreadable;
    unreadable.http->apiBodies["browse"] = {readFixture("playlist_long_first.json"), "not json"};
    const auto parse = unreadable.resolver.playlist(LONG_PLAYLIST, 10);
    CHECK(parse.status.code == Error::Parse);
    CHECK(parse.value.playlistId == LONG_PLAYLIST);
    CHECK(parse.value.title == "Lex Fridman Podcast");
    CHECK(parse.value.totalCount == 447);
    CHECK(idsOf(parse.value.entries) == std::vector<std::string>{"s7d2d8FhevU", "NYFGCESmikA", "l6USUAIKJls"});
    CHECK(unreadable.http->requests.size() == 2);

    // Cancelled before page two: page one's entries are still there to use.
    TestResolver cancelled;
    cancelled.http->apiBodies["browse"] = {readFixture("playlist_long_first.json"), readFixture("playlist_long_continuation.json")};
    ytres::Request request;
    int checks = 0;
    request.cancelled = [&checks] { return ++checks > 1; };
    const auto stopped = cancelled.resolver.playlist(LONG_PLAYLIST, 10, request);
    CHECK(stopped.status.code == Error::Cancelled);
    CHECK(stopped.value.title == "Lex Fridman Podcast");
    CHECK(stopped.value.entries.size() == 3);
    CHECK(cancelled.http->requests.size() == 1);
}

TEST_CASE("a playlist for no videos, a mix, or no playlist at all is BadInput and sends nothing")
{
    TestResolver test;
    CHECK(test.resolver.playlist(LONG_PLAYLIST, 0).status.code == Error::BadInput);
    const auto mix = test.resolver.playlist("https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=RDdQw4w9WgXcQ", 5);
    CHECK(mix.status.code == Error::BadInput);
    CHECK(mix.status.message.find("signed-in viewer") != std::string::npos);
    CHECK(test.resolver.playlist("https://www.youtube.com/watch?v=dQw4w9WgXcQ", 5).status.code == Error::BadInput);
    CHECK(test.resolver.playlist("", 5).status.code == Error::BadInput);
    CHECK(test.http->requests.empty());
}

TEST_CASE("with no visitor data cached, a later playlist page carries what the page before it named")
{
    TestResolver test;
    test.http->apiBodies["browse"] = {readFixture("playlist_long_first.json"), readFixture("playlist_long_continuation.json")};
    REQUIRE(test.resolver.playlist(LONG_PLAYLIST, 5));
    REQUIRE(test.http->requests.size() == 2);
    CHECK(headerValue(test.http->requests[0], "X-Goog-Visitor-Id").empty());
    CHECK(json::parse(test.http->requests[0].body)["context"]["client"].count("visitorData") == 0);
    CHECK(headerValue(test.http->requests[1], "X-Goog-Visitor-Id") == "FIXTURE");
    CHECK(json::parse(test.http->requests[1].body)["context"]["client"]["visitorData"] == "FIXTURE");

    // It stays out of the cache: the next resolve still fetches a watch page.
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 4);
    CHECK(test.http->requests[2].method == "GET");
    CHECK(headerValue(test.http->requests[3], "X-Goog-Visitor-Id") == VISITOR_DATA);
}

TEST_CASE("with visitor data cached, every playlist page carries the cached one")
{
    TestResolver test;
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ")); // the watch page, the player
    test.http->apiBodies["browse"] = {readFixture("playlist_long_first.json"), readFixture("playlist_long_continuation.json")};
    REQUIRE(test.resolver.playlist(LONG_PLAYLIST, 5));
    REQUIRE(test.http->requests.size() == 4); // no watch page of its own
    for (std::size_t i = 2; i < 4; ++i) {
        CAPTURE(i);
        CHECK(test.http->requests[i].url == BROWSE_URL);
        CHECK(headerValue(test.http->requests[i], "X-Goog-Visitor-Id") == VISITOR_DATA);
        CHECK(json::parse(test.http->requests[i].body)["context"]["client"]["visitorData"] == VISITOR_DATA);
    }
}

TEST_CASE("a playlist cancelled up front sends nothing, and a cancel check that throws is Internal")
{
    TestResolver cancelled;
    ytres::Request request;
    request.cancelled = [] { return true; };
    CHECK(cancelled.resolver.playlist(LONG_PLAYLIST, 5, request).status.code == Error::Cancelled);
    CHECK(cancelled.http->requests.empty());

    TestResolver broken;
    request.cancelled = []() -> bool { throw std::runtime_error("the check broke"); };
    CHECK(broken.resolver.playlist(LONG_PLAYLIST, 5, request).status.code == Error::Internal);
    CHECK(broken.http->requests.empty());
}

TEST_CASE("a playlist keeps resolve()'s rules for the request timeout and the deadline")
{
    TestResolver zero(WATCH_PAGE, 0ms);
    const auto refused = zero.resolver.playlist(LONG_PLAYLIST, 5);
    CHECK(refused.status.code == Error::BadInput);
    CHECK(refused.status.message == "Options::requestTimeout must be positive");
    CHECK(zero.http->requests.empty());

    TestResolver spent;
    ytres::Request request;
    request.deadline = 0ms;
    CHECK(spent.resolver.playlist(LONG_PLAYLIST, 5, request).status.code == Error::Timeout);
    CHECK(spent.http->requests.empty());

    // Every page gets the request timeout, never more than the deadline leaves.
    TestResolver tight(WATCH_PAGE, 5s);
    tight.http->apiBodies["browse"] = {readFixture("playlist_long_first.json"), readFixture("playlist_long_continuation.json")};
    request.deadline = 2s;
    CHECK(tight.resolver.playlist(LONG_PLAYLIST, 5, request));
    REQUIRE(tight.http->requests.size() == 2);
    for (const ytres::HttpRequest &sent : tight.http->requests) {
        CHECK(sent.timeout > 0ms);
        CHECK(sent.timeout <= 2000ms);
    }
}

TEST_CASE("a moved-from Resolver refuses a playlist")
{
    TestResolver test;
    ytres::Resolver taken = std::move(test.resolver);
    const auto moved = test.resolver.playlist(LONG_PLAYLIST, 5); // NOLINT(bugprone-use-after-move): that is the point
    CHECK(moved.status.code == Error::BadInput);
    CHECK(moved.status.message == "This Resolver has been moved from");
    CHECK(taken.playlist(LONG_PLAYLIST, 0).status.code == Error::BadInput); // the one it moved to still checks its input
    CHECK(test.http->requests.empty());
}
