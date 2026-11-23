#pragma once

#include "ytres/ytres.h"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// ytres_cli's command line, read into Arguments. A header of its own so that
// ytres_tests can pin it; it is harness code, not the library's.
namespace ytres_cli {

// What --client takes: yt-dlp's names for the clients, as the library's
// table keeps them.
inline const std::pair<std::string_view, ytres::ClientId> CLIENT_NAMES[] = {
    {"visionos", ytres::ClientId::VisionOS},
    {"web", ytres::ClientId::Web},
};

struct Arguments
{
    // Each is empty when not given; given, even as "", it counts, so that an
    // empty target is the library's BadInput, not a target nobody passed.
    std::optional<std::string> target;
    bool formats{false};
    std::optional<std::string> dumpPath;
    std::optional<ytres::ClientId> client; // the library's own ladder when empty
    std::optional<std::string> query;      // --search: a search instead of a resolve
    std::optional<std::string> playlist;   // --playlist: a playlist instead of a resolve
    std::optional<std::size_t> max;        // for --search or --playlist; their default when empty
};

inline std::optional<ytres::ClientId> clientNamed(std::string_view name)
{
    for (const auto &[clientName, id] : CLIENT_NAMES) {
        if (clientName == name) {
            return id;
        }
    }
    return std::nullopt;
}

// --max's count: decimal digits and nothing else. 0 goes through, for the
// library to answer.
inline std::optional<std::size_t> countIn(std::string_view text)
{
    std::size_t count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return count;
}

// arguments are the command line after the program's name. False for a line
// to answer with the usage: an unknown option, an option without its value,
// any option given twice (rather than the last one silently winning), more
// or fewer than one of a target, --search and --playlist, or an option that
// belongs to another mode.
inline bool parseArguments(const std::vector<std::string> &arguments, Arguments &args)
{
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string &arg = arguments[i];
        // The value after an option, if there is one.
        const std::string *value = i + 1 < arguments.size() ? &arguments[i + 1] : nullptr;
        if (arg == "--formats") {
            if (args.formats) {
                return false;
            }
            args.formats = true;
            continue;
        }
        if (arg.rfind("--", 0) != 0) {
            if (args.target) {
                return false;
            }
            args.target = arg;
            continue;
        }
        if (!value) {
            return false;
        }
        ++i;
        if (arg == "--dump" && !args.dumpPath) {
            args.dumpPath = *value;
        } else if (arg == "--client" && !args.client) {
            args.client = clientNamed(*value);
            if (!args.client) {
                return false;
            }
        } else if (arg == "--search" && !args.query) {
            args.query = *value;
        } else if (arg == "--playlist" && !args.playlist) {
            args.playlist = *value;
        } else if (arg == "--max" && !args.max) {
            args.max = countIn(*value);
            if (!args.max) {
                return false;
            }
        } else {
            return false; // unknown, or given twice
        }
    }
    // Exactly one of a target, a search and a playlist. --formats and
    // --client are about a resolve, --max about a search or a playlist.
    const int modes = (args.target ? 1 : 0) + (args.query ? 1 : 0) + (args.playlist ? 1 : 0);
    if (modes != 1) {
        return false;
    }
    if (args.target) {
        return !args.max;
    }
    return !args.formats && !args.client;
}

}
