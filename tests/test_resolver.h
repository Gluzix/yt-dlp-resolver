#pragma once

#include "fake_http_client.h"

#include "ytres/ytres.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A watch page whose ytcfg carries visitorData where YouTube's does.
inline std::string watchPageWith(const std::string &visitorData)
{
    return R"(<script>ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":")" + visitorData + R"("}}});</script>)";
}

inline const char *const VISITOR_DATA = "CgtGSVhUVVJFAAAA%3D%3D";
inline const std::string WATCH_PAGE = watchPageWith(VISITOR_DATA);

// A Resolver on a FakeHttpClient that answers with the recorded
// dQw4w9WgXcQ response, keeping whatever it logs.
class TestResolver
{
public:
    explicit TestResolver(const std::string &watchPage = WATCH_PAGE,
                          std::chrono::milliseconds requestTimeout = std::chrono::seconds(10))
        : http(std::make_shared<FakeHttpClient>(readFixture("player_dQw4w9WgXcQ.json"), watchPage))
        , resolver(options(requestTimeout))
    {
    }

    bool logged(ytres::LogLevel level) const
    {
        return std::any_of(logs.begin(), logs.end(), [level](const auto &entry) { return entry.first == level; });
    }

    // The first line logged at level that contains text; null if none.
    const std::string *logLine(ytres::LogLevel level, const std::string &text) const
    {
        for (const auto &[loggedLevel, line] : logs) {
            if (loggedLevel == level && line.find(text) != std::string::npos) {
                return &line;
            }
        }
        return nullptr;
    }

    std::shared_ptr<FakeHttpClient> http;
    std::vector<std::pair<ytres::LogLevel, std::string>> logs;
    ytres::Resolver resolver;

private:
    ytres::Resolver::Options options(std::chrono::milliseconds requestTimeout)
    {
        ytres::Resolver::Options made;
        made.http = http;
        made.log = [this](ytres::LogLevel level, std::string_view text) { logs.emplace_back(level, std::string(text)); };
        made.requestTimeout = requestTimeout;
        return made;
    }
};

// Whether request went to the player API rather than the watch page.
inline bool isPlayerRequest(const ytres::HttpRequest &request)
{
    return request.url.find("/youtubei/v1/player") != std::string::npos;
}
