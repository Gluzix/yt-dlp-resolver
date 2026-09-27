#include "url_parse.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

using ytres::Error;
using ytres::parseVideoId;

TEST_CASE("every link form in the notes gives the video id")
{
    const char *const accepted[] = {
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42s",
        "https://www.youtube.com/watch?feature=share&v=dQw4w9WgXcQ&list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG",
        "https://youtube.com/watch?v=dQw4w9WgXcQ",
        "https://m.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://music.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://music.youtube.com/watch?v=dQw4w9WgXcQ&si=FIXTURE",
        "https://youtu.be/dQw4w9WgXcQ",
        "https://youtu.be/dQw4w9WgXcQ?t=42",
        "https://youtu.be/dQw4w9WgXcQ?si=FIXTURE",
        "https://www.youtube.com/shorts/dQw4w9WgXcQ",
        "https://www.youtube.com/shorts/dQw4w9WgXcQ?feature=share",
        "https://www.youtube.com/live/dQw4w9WgXcQ",
        "https://www.youtube.com/live/dQw4w9WgXcQ?si=FIXTURE",
        "https://www.youtube.com/embed/dQw4w9WgXcQ",
        "https://www.youtube.com/embed/dQw4w9WgXcQ?start=10",
        "https://www.youtube.com/v/dQw4w9WgXcQ",
        "dQw4w9WgXcQ",
    };
    for (const char *input : accepted) {
        const std::string text = input; // a std::string, so a failure prints the text, not the pointer
        CAPTURE(text);
        const auto result = parseVideoId(input);
        REQUIRE(result);
        CHECK(result.value == "dQw4w9WgXcQ");
    }
}

TEST_CASE("the scheme may be http or missing, and case and padding do not matter")
{
    const char *const accepted[] = {
        "http://www.youtube.com/watch?v=dQw4w9WgXcQ",
        "www.youtube.com/watch?v=dQw4w9WgXcQ",
        "youtu.be/dQw4w9WgXcQ",
        "HTTPS://WWW.YouTube.com/watch?v=dQw4w9WgXcQ",
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ#t=30",
        "https://youtu.be/dQw4w9WgXcQ/",
        "  https://youtu.be/dQw4w9WgXcQ\n",
    };
    for (const char *input : accepted) {
        const std::string text = input; // a std::string, so a failure prints the text, not the pointer
        CAPTURE(text);
        const auto result = parseVideoId(input);
        REQUIRE(result);
        CHECK(result.value == "dQw4w9WgXcQ");
    }
}

TEST_CASE("an id keeps its dashes and underscores")
{
    const auto result = parseVideoId("https://youtu.be/_-aB3_-xY9z");
    REQUIRE(result);
    CHECK(result.value == "_-aB3_-xY9z");
}

TEST_CASE("anything that is not a video link or id is BadInput")
{
    const char *const rejected[] = {
        "",
        "   ",
        "dQw4w9WgXc",                                               // 10 characters
        "dQw4w9WgXcQQ",                                             // 12
        "dQw4w9WgX.Q",                                              // outside the alphabet
        "https://vimeo.com/dQw4w9WgXcQ",
        "https://notyoutube.com/watch?v=dQw4w9WgXcQ",
        "https://www.youtube.com.evil.example/watch?v=dQw4w9WgXcQ",
        "https://user@www.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://www.youtube.com:8080/watch?v=dQw4w9WgXcQ",
        "ftp://www.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://www.youtube.com/watch?v=dQw4w9WgXc",               // short v
        "https://www.youtube.com/watch?vv=dQw4w9WgXcQ",
        "https://www.youtube.com/watch?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG",
        "https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG",
        "https://www.youtube.com/@RickAstleyYT",
        "https://www.youtube.com/",
        "https://youtu.be/",
        "https://youtu.be?v=dQw4w9WgXcQ",
    };
    for (const char *input : rejected) {
        const std::string text = input; // a std::string, so a failure prints the text, not the pointer
        CAPTURE(text);
        const auto result = parseVideoId(input);
        CHECK(result.status.code == Error::BadInput);
        CHECK_FALSE(result.status.message.empty());
    }
}

TEST_CASE("the canonical page url")
{
    CHECK(ytres::canonicalWatchUrl("dQw4w9WgXcQ") == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
}

TEST_CASE("queryValue reads one raw query parameter")
{
    // As a string, so doctest can print it; "<none>" when absent.
    const auto valueOf = [](std::string_view url, std::string_view name) {
        const auto value = ytres::queryValue(url, name);
        return value ? std::string(*value) : std::string("<none>");
    };
    const std::string url = "https://rr2---sn-x.googlevideo.com/videoplayback?expire=1790283253&ei=abc&mime=audio%2Fwebm";
    CHECK(valueOf(url, "expire") == "1790283253");
    CHECK(valueOf(url, "mime") == "audio%2Fwebm");
    CHECK(valueOf(url, "pire") == "<none>");
    CHECK(valueOf("https://x/?a=1&expire=2#expire=3", "expire") == "2");
    CHECK(valueOf("https://x/?expire&a=1", "expire").empty());
    CHECK(valueOf("https://x/videoplayback", "expire") == "<none>");
}
