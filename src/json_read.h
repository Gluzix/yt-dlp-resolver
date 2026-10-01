#pragma once

#include <nlohmann/json.hpp>

#include <charconv>
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

}
