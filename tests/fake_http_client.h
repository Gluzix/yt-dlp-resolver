#pragma once

#include "ytres/http.h"

#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The value of the header called name, or empty.
inline std::string headerValue(const ytres::HttpRequest &request, const std::string &name)
{
    for (const auto &[key, value] : request.headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

// Stands in for the network: the InnerTube POST gets the next of
// playerBodyQueue while it lasts, else the asking client's entry in
// playerBodyByClient, else playerBody; any other request (the watch page)
// gets watchPage; and every request is kept for the test to look at.
class FakeHttpClient : public ytres::HttpClient
{
public:
    explicit FakeHttpClient(std::string playerBody_, std::string watchPage_ = {})
        : playerBody(std::move(playerBody_)), watchPage(std::move(watchPage_))
    {
    }

    ytres::Result<ytres::HttpResponse> send(const ytres::HttpRequest &request) override
    {
        requests.push_back(request);
        const bool player = request.url.find("/youtubei/v1/player") != std::string::npos;
        const ytres::Status &failure = player ? playerFailure : pageFailure;
        if (failure.code != ytres::Error::Ok) {
            return {failure, {}};
        }
        std::string body = player ? playerBody : watchPage;
        const auto byClient = playerBodyByClient.find(headerValue(request, "X-YouTube-Client-Name"));
        if (player && !playerBodyQueue.empty()) {
            body = std::move(playerBodyQueue.front());
            playerBodyQueue.pop_front();
        } else if (player && byClient != playerBodyByClient.end()) {
            body = byClient->second;
        }
        ytres::Result<ytres::HttpResponse> result{{}, {player ? playerStatus : pageStatus, std::move(body)}};
        if (result.value.status >= 400) {
            // what the HttpClient contract asks of every client
            result.status = {ytres::Error::Http, "HTTP " + std::to_string(result.value.status)};
        }
        return result;
    }

    std::string playerBody;
    std::deque<std::string> playerBodyQueue; // answered in turn, ahead of the rest
    std::map<std::string, std::string> playerBodyByClient; // by X-YouTube-Client-Name, ahead of playerBody
    std::string watchPage;
    long playerStatus{200};
    long pageStatus{200};
    ytres::Status playerFailure; // not Ok: returned instead of a response
    ytres::Status pageFailure;
    std::vector<ytres::HttpRequest> requests;
};

// A recorded response from tests/fixtures, byte for byte.
inline std::string readFixture(const std::string &name)
{
    std::ifstream file(std::string(YTRES_FIXTURES) + "/" + name, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
