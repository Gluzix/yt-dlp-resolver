#include "continuation.h"

#include "json_read.h"

#include <nlohmann/json.hpp>

namespace ytres::innertube {

namespace {

using nlohmann::json;

using jsonread::child;
using jsonread::childArray;
using jsonread::readString;

// The token one command holds in continuationCommand.token, with the
// clickTrackingParams beside it; empty when it holds none.
Continuation tokenIn(const json &command)
{
    const json *continuationCommand = child(command, "continuationCommand");
    const std::string token = continuationCommand ? readString(*continuationCommand, "token") : std::string{};
    if (token.empty()) {
        return {};
    }
    return {token, readString(command, "clickTrackingParams")};
}

// What a command leads to. Like yt-dlp, look through the commands[] of a
// commandExecutorCommand first, then at the command itself, and take the
// first token found.
Continuation fromCommand(const json &command)
{
    const json *executor = child(command, "commandExecutorCommand");
    const json *commands = executor ? childArray(*executor, "commands") : nullptr;
    if (commands) {
        for (const json &inner : *commands) {
            Continuation found = tokenIn(inner);
            if (!found.token.empty()) {
                return found;
            }
        }
    }
    return tokenIn(command);
}

}

Continuation continuationOf(const json &item)
{
    // The renderer form, which search uses: its endpoint, else its button's command.
    if (const json *renderer = child(item, "continuationItemRenderer")) {
        const json *endpoint = child(*renderer, "continuationEndpoint");
        Continuation found = endpoint ? fromCommand(*endpoint) : Continuation{};
        if (!found.token.empty()) {
            return found;
        }
        const json *button = child(*renderer, "button");
        const json *buttonRenderer = button ? child(*button, "buttonRenderer") : nullptr;
        const json *command = buttonRenderer ? child(*buttonRenderer, "command") : nullptr;
        return command ? fromCommand(*command) : Continuation{};
    }
    // The view model form, which playlists moved to.
    const json *viewModel = child(item, "continuationItemViewModel");
    const json *continuationCommand = viewModel ? child(*viewModel, "continuationCommand") : nullptr;
    const json *innertubeCommand = continuationCommand ? child(*continuationCommand, "innertubeCommand") : nullptr;
    return innertubeCommand ? fromCommand(*innertubeCommand) : Continuation{};
}

void addContinuation(nlohmann::ordered_json &fields, const Continuation &continuation)
{
    fields["continuation"] = continuation.token;
    if (!continuation.clickTrackingParams.empty()) {
        fields["clickTracking"]["clickTrackingParams"] = continuation.clickTrackingParams;
    }
}

}
