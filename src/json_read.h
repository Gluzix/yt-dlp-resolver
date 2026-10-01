#pragma once

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

// Type-checked reads of the JSON YouTube answers with, shared by every reader
// of an InnerTube answer: the player's, a search's, a playlist's.
// =======================================================
// Rules:
// - Nothing here throws on YouTube's account. A key that is missing, or holds
//   a value of another type, reads as null, empty, false or zero, so that an
//   answer of a surprising shape is a failure to report, never a crash of
//   the program the library runs in.
// - Each function looks up one key of one object; a path is a chain of
//   child() calls, each of which may come back null.
// =======================================================
namespace ytres::jsonread {

using json = nlohmann::json;

inline const json *child(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? &*it : nullptr;
}

inline const json *childArray(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? &*it : nullptr;
}

inline std::string readString(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

inline bool readBool(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

// Python's truth test, which is how yt-dlp asks about a field: present but
// 0, "" or [] counts as absent.
inline bool isTruthy(const json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return false;
    }
    switch (it->type()) {
    case json::value_t::boolean:
        return it->get<bool>();
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
    case json::value_t::number_float:
        return it->get<double>() != 0.0;
    case json::value_t::string:
        return !it->get_ref<const std::string &>().empty();
    case json::value_t::array:
    case json::value_t::object:
        return !it->empty();
    default:
        return false; // null
    }
}

inline std::int64_t toInt64(std::string_view text)
{
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size() ? value : 0;
}

// YouTube sends some numbers as JSON numbers and others as strings
// ("lengthSeconds": "212"). Either reads; missing or malformed is 0.
inline std::int64_t readInt(const json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return 0;
    }
    if (it->is_number_integer()) {
        return it->get<std::int64_t>();
    }
    if (it->is_string()) {
        return toInt64(it->get_ref<const std::string &>());
    }
    return 0;
}

// InnerTube writes a label three ways: {"simpleText": ...}, {"runs":
// [{"text": ...}, ...]} to be joined, or a view model's {"content": ...}.
// The text of label itself, which may stand alone in an array, as a
// playlist's stats do; empty when it is none of the three.
inline std::string labelText(const json &label)
{
    if (!label.is_object()) {
        return {};
    }
    std::string text = readString(label, "simpleText");
    const json *runs = childArray(label, "runs");
    if (text.empty() && runs) {
        for (const json &run : *runs) {
            if (run.is_object()) {
                text += readString(run, "text");
            }
        }
    }
    return text.empty() ? readString(label, "content") : text;
}

// The text of the label object holds under key, read as labelText() reads
// one. Empty when object has no such key or the value is none of the three.
inline std::string textOf(const json &object, const char *key)
{
    const json *label = child(object, key);
    return label ? labelText(*label) : std::string{};
}

// A duration as YouTube writes it under a thumbnail, m:ss or h:mm:ss -
// "4:36" is 276, "2:31:48" is 9108 - in seconds. Anything that is not two or
// three groups of digits separated by colons is 0: "LIVE", "Upcoming", "".
inline std::int64_t durationSeconds(std::string_view text)
{
    // Nine digits a group keeps the sum far inside 64 bits whatever comes.
    const std::size_t MAX_GROUP_DIGITS = 9;
    std::int64_t total = 0;
    int groups = 0;
    for (std::size_t start = 0;;) {
        const std::size_t colon = text.find(':', start);
        const std::string_view group =
            text.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start);
        if (group.empty() || group.size() > MAX_GROUP_DIGITS || ++groups > 3) {
            return 0;
        }
        std::int64_t value = 0;
        for (const char c : group) {
            if (c < '0' || c > '9') {
                return 0;
            }
            value = value * 10 + (c - '0');
        }
        total = total * 60 + value;
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    return groups >= 2 ? total : 0;
}

}
