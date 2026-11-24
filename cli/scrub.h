#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <string>

// What ytres_cli takes out of a --dump before it is written, so that a dump
// can be committed as a fixture. A header of its own so that ytres_tests can
// pin it; it is harness code, not the library's.
//
// Scanned by hand, not with std::regex: MSVC's matcher recurses once per
// repetition of a group, so a pattern that reads a JSON string or an address
// of unbounded length threw regex_error on a value of 1000 characters, and
// overflowed the stack on longer ones. Every scan below is a loop over the
// text, and none of it throws but for running out of memory.
namespace ytres_cli {

namespace scrub_detail {

const std::size_t NO_MATCH = std::string::npos;

// JSON's whitespace, as \s matched it: space, tab, the line breaks, \v, \f.
inline bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// What an address is written with: hex digits, dots and colons.
inline bool isAddressChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
}

// The position after literal when text holds it at at, else NO_MATCH. With
// ignoreCase, letters match either case: percent-encoding writes its hex
// digits in both, and literal is then given in lower case.
inline std::size_t literal(const std::string &text, std::size_t at, const char *expected, bool ignoreCase = false)
{
    if (at == NO_MATCH) {
        return NO_MATCH;
    }
    const std::size_t length = std::strlen(expected);
    if (at > text.size() || text.size() - at < length) {
        return NO_MATCH;
    }
    for (std::size_t i = 0; i < length; ++i) {
        char c = text[at + i];
        if (ignoreCase) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (c != expected[i]) {
            return NO_MATCH;
        }
    }
    return at + length;
}

// The position after the first of the alternatives text holds at at.
inline std::size_t oneOf(const std::string &text, std::size_t at, std::initializer_list<const char *> alternatives,
                         bool ignoreCase = false)
{
    for (const char *alternative : alternatives) {
        const std::size_t end = literal(text, at, alternative, ignoreCase);
        if (end != NO_MATCH) {
            return end;
        }
    }
    return NO_MATCH;
}

inline std::size_t skipSpace(const std::string &text, std::size_t at)
{
    if (at == NO_MATCH) {
        return NO_MATCH;
    }
    while (at < text.size() && isSpace(text[at])) {
        ++at;
    }
    return at;
}

// The position after separator, whitespace allowed around it.
inline std::size_t separator(const std::string &text, std::size_t at, const char *expected)
{
    return skipSpace(text, literal(text, skipSpace(text, at), expected));
}

// The position after the JSON string that opens at at: its quotes, and a
// backslash taking the byte that follows it, an escaped quote included.
// NO_MATCH when no string opens there or it never closes. A backslash
// before a line break is no escape - JSON has none such, and the pattern
// this replaced did not take one either.
inline std::size_t jsonString(const std::string &text, std::size_t at)
{
    if (at >= text.size() || text[at] != '"') {
        return NO_MATCH;
    }
    for (std::size_t i = at + 1; i < text.size(); ++i) {
        if (text[i] == '"') {
            return i + 1;
        }
        if (text[i] == '\\') {
            if (i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r')) {
                return NO_MATCH;
            }
            ++i;
        }
    }
    return NO_MATCH;
}

// The position after the address that starts at at, or at itself when none
// does. The address ends at the first character that cannot be part of
// one, so the %26 after an encoded address is left for the next parameter.
inline std::size_t addressEnd(const std::string &text, std::size_t at)
{
    while (at < text.size()) {
        if (isAddressChar(text[at])) {
            ++at;
            continue;
        }
        const std::size_t encoded = oneOf(text, at, {"%3a", "%253a"}, true);
        if (encoded == NO_MATCH) {
            break;
        }
        at = encoded;
    }
    return at;
}

// A rule: where its match that starts at at ends, or NO_MATCH; replacement
// is set to what the match is written as.
using Rule = std::size_t (*)(const std::string &text, std::size_t at, std::string &replacement);

// text with every match of rule replaced, read from left to right, the
// search going on after each match. A match can only start at one of
// starts, which spares the rule most positions.
inline std::string applied(const std::string &text, Rule rule, const char *starts)
{
    std::string out;
    std::string replacement;
    std::size_t copied = 0;
    std::size_t at = text.find_first_of(starts);
    while (at != std::string::npos) {
        const std::size_t end = rule(text, at, replacement);
        if (end == NO_MATCH) {
            at = text.find_first_of(starts, at + 1);
            continue;
        }
        out.append(text, copied, at - copied);
        out += replacement;
        copied = end;
        at = text.find_first_of(starts, end);
    }
    out.append(text, copied, std::string::npos);
    return out;
}

const char *const FAKE_ADDRESS = "203.0.113.7";

// ?ip=, &ip=, %26ip%3D, %3Fip%3D, &ip= and the address after it.
inline std::size_t queryAddress(const std::string &text, std::size_t at, std::string &replacement)
{
    std::size_t end = oneOf(text, at, {"?", "&", "\\u0026"});
    if (end == NO_MATCH) {
        end = oneOf(text, at, {"%26", "%3f"}, true);
    }
    const std::size_t name = literal(text, end, "ip");
    end = literal(text, name, "=");
    if (end == NO_MATCH) {
        end = literal(text, name, "%3d", true);
    }
    if (end == NO_MATCH) {
        return NO_MATCH;
    }
    const std::size_t address = addressEnd(text, end);
    if (address == end) {
        return NO_MATCH;
    }
    replacement = text.substr(at, end - at) + FAKE_ADDRESS;
    return address;
}

// /ip/<address>/ in a path, each slash possibly %2F.
inline std::size_t pathAddress(const std::string &text, std::size_t at, std::string &replacement)
{
    std::size_t end = oneOf(text, at, {"/", "%2f"}, true);
    end = literal(text, end, "ip");
    end = oneOf(text, end, {"/", "%2f"}, true);
    if (end == NO_MATCH) {
        return NO_MATCH;
    }
    const std::size_t address = addressEnd(text, end);
    const std::size_t after = oneOf(text, address, {"/", "%2f"}, true);
    if (address == end || after == NO_MATCH) {
        return NO_MATCH;
    }
    replacement = text.substr(at, end - at) + FAKE_ADDRESS + text.substr(address, after - address);
    return after;
}

// "key":"visitor_data","value":"<string>"
inline std::size_t visitorDataKeyFirst(const std::string &text, std::size_t at, std::string &replacement)
{
    std::size_t end = literal(text, at, "\"key\"");
    end = separator(text, end, ":");
    end = literal(text, end, "\"visitor_data\"");
    end = separator(text, end, ",");
    end = literal(text, end, "\"value\"");
    end = jsonString(text, separator(text, end, ":"));
    if (end != NO_MATCH) {
        replacement = R"("key":"visitor_data","value":"FIXTURE")";
    }
    return end;
}

// "value":"<string>","key":"visitor_data"
inline std::size_t visitorDataValueFirst(const std::string &text, std::size_t at, std::string &replacement)
{
    std::size_t end = literal(text, at, "\"value\"");
    end = jsonString(text, separator(text, end, ":"));
    end = separator(text, end, ",");
    end = literal(text, end, "\"key\"");
    end = separator(text, end, ":");
    end = literal(text, end, "\"visitor_data\"");
    if (end != NO_MATCH) {
        replacement = R"("value":"FIXTURE","key":"visitor_data")";
    }
    return end;
}

// "visitorData":"<string>"
inline std::size_t visitorData(const std::string &text, std::size_t at, std::string &replacement)
{
    std::size_t end = literal(text, at, "\"visitorData\"");
    end = jsonString(text, separator(text, end, ":"));
    if (end != NO_MATCH) {
        replacement = R"("visitorData":"FIXTURE")";
    }
    return end;
}

}

// Every stream url names the requesting address: ?ip=/&ip= in queries,
// /ip/<addr>/ in hlsManifestUrl's path, IPv6 percent-encoded. A url inside
// signatureCipher is percent-encoded whole (%26ip%3D, %2Fip%2F), and a page
// may escape its ampersands as &, so the separators come in those
// forms too. The address itself is hex digits, dots and colons, a colon
// possibly written %3A or, encoded twice, %253A - and nothing else, so that
// the %26 after an encoded address is left for the next parameter.
// Visitor data identifies the visitor to YouTube, and carries the country,
// so it goes too, under both names it travels by: "visitorData", and the web
// client's tracking parameter "visitor_data", whose key and value may come
// in either order. A JSON string is read whole, escaped quotes and all, and
// whitespace may sit around the colons. The rules apply one after another,
// each over the whole text; text that is not JSON, or breaks off in the
// middle of a string, is left as it is where no rule matches it whole.
inline std::string scrubbed(std::string text)
{
    namespace detail = scrub_detail;
    text = detail::applied(text, detail::queryAddress, "?&%\\");
    text = detail::applied(text, detail::pathAddress, "/%");
    text = detail::applied(text, detail::visitorDataKeyFirst, "\"");
    text = detail::applied(text, detail::visitorDataValueFirst, "\"");
    return detail::applied(text, detail::visitorData, "\"");
}

}
