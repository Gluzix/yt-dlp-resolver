#pragma once

#include "ytres/http.h"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Stands in for the network: the InnerTube POST gets playerBody, any other
// request (the watch page) gets watchPage, and every request is kept for
// the test to look at.
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
        ytres::Result<ytres::HttpResponse> result{{}, {player ? playerStatus : 200, player ? playerBody : watchPage}};
        if (result.value.status >= 400) {
            // what the HttpClient contract asks of every client
            result.status = {ytres::Error::Http, "HTTP " + std::to_string(result.value.status)};
        }
        return result;
    }

    std::string playerBody;
    std::string watchPage;
    long playerStatus{200};
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
