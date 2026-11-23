#include "utf8.h"

#include <doctest/doctest.h>

#include <string>

using ytres::utf8::isValid;

// The bytes are spelled as escapes, so this file stays ASCII. Mind that a hex
// escape runs on through any hex digit after it.

TEST_CASE("well-formed UTF-8 of one to four bytes a character is valid")
{
    const std::string valid[] = {
        "",
        "plain ASCII, with a tab\t and a newline\n",
        std::string("NUL\0inside", 10),
        "Dawid Podsiad\xC5\x82o",                       // U+0142, two bytes
        "\xC2\x80",                                     // U+0080, the first two-byte character
        "\xDF\xBF",                                     // U+07FF, the last
        "\xE0\xA0\x80",                                 // U+0800, the first three-byte character
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",         // three CJK characters
        "\xED\x9F\xBF",                                 // U+D7FF, just below the surrogates
        "\xEE\x80\x80",                                 // U+E000, just above them
        "\xEF\xBF\xBD",                                 // U+FFFD itself
        "\xF0\x90\x80\x80",                             // U+10000, the first four-byte character
        "\xF0\x9F\x8E\xB5",                             // a musical note emoji
        "\xF4\x8F\xBF\xBF",                             // U+10FFFF, the last code point
    };
    for (const std::string &text : valid) {
        CAPTURE(text);
        CHECK(isValid(text));
    }
}

TEST_CASE("overlongs, surrogates, code points past U+10FFFF, cut-off tails and stray bytes are not")
{
    const std::string invalid[] = {
        "Dawid Podsiad\xB3o",       // the Polish l in code page 1250: a lone continuation byte
        "\x80",                     // a continuation byte with no lead
        "a\xBFz",
        "\xC0\x80",                 // an overlong NUL
        "\xC1\xBF",                 // an overlong DEL
        "\xE0\x80\x80",             // an overlong three-byte form
        "\xE0\x9F\xBF",
        "\xF0\x80\x80\x80",         // an overlong four-byte form
        "\xF0\x8F\xBF\xBF",
        "\xED\xA0\x80",             // U+D800, a surrogate
        "\xED\xBF\xBF",             // U+DFFF
        "\xF4\x90\x80\x80",         // U+110000
        "\xF5\x80\x80\x80",         // a lead byte that cannot occur
        "\xFF",
        "\xFE\xFF",
        "\xC5",                     // a two-byte character cut off
        "Podsiad\xC5",
        "\xE6\x97",                 // a three-byte character cut off
        "\xF0\x9F\x8E",             // a four-byte character cut off
        "\xC5o",                    // a lead byte followed by ASCII
        "\xE6\x97o",
        "\xC5\x82\x82",             // one continuation too many
    };
    for (const std::string &text : invalid) {
        CAPTURE(text);
        CHECK_FALSE(isValid(text));
    }
}
