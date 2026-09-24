#include "innertube.h"
#include "watch_page.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using ytres::watchpage::visitorData;

TEST_CASE("visitor data comes from the ytcfg object, past the string-form calls")
{
    const std::string html =
        R"(<script>ytcfg.set('EXPERIMENT_FLAGS', {});</script>)"
        R"(<script>ytcfg.set({"CLIENT_CANARY_STATE":"none","INNERTUBE_CONTEXT":{"client":{"hl":"en",)"
        R"("visitorData":"CgtGSVhUVVJFAAAA%3D%3D"}}}); window.ytcfg.obfuscatedData_ = [];</script>)";
    CHECK(visitorData(html) == "CgtGSVhUVVJFAAAA%3D%3D");
}

TEST_CASE("braces and quotes inside strings do not end the object")
{
    const std::string html = R"(ytcfg.set({"A":"} { \" }","INNERTUBE_CONTEXT":{"client":{"visitorData":"v1"}}});)";
    CHECK(visitorData(html) == "v1");
}

TEST_CASE("a config without visitor data is passed over for a later one")
{
    const std::string html = R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{}}}); )"
                             R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":"v2"}}});)";
    CHECK(visitorData(html) == "v2");
}

TEST_CASE("no ytcfg, a broken one or a strange value gives no visitor data")
{
    CHECK(visitorData("").empty());
    CHECK(visitorData("<html>Before you continue to YouTube</html>").empty());
    CHECK(visitorData(R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":"v3"})").empty()); // never closes
    CHECK(visitorData(R"(ytcfg.set({not json});)").empty());
    CHECK(visitorData(R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":42}}});)").empty());
}

TEST_CASE("the watch page request carries the client's user agent and nothing else")
{
    const ytres::innertube::ClientDef *client = ytres::innertube::findClient(ytres::ClientId::VisionOS);
    REQUIRE(client != nullptr);
    const ytres::HttpRequest request = ytres::watchpage::request(*client, "dQw4w9WgXcQ");
    CHECK(request.method == "GET");
    CHECK(request.url == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    REQUIRE(request.headers.size() == 1);
    CHECK(request.headers[0].first == "User-Agent");
    CHECK(request.headers[0].second == client->userAgent);
    CHECK(request.body.empty());
}

TEST_CASE("visitor data that could not travel safely as a header counts as none")
{
    const auto page = [](const std::string &value) {
        nlohmann::json config;
        config["INNERTUBE_CONTEXT"]["client"]["visitorData"] = value;
        return "ytcfg.set(" + config.dump() + ");";
    };
    CHECK(visitorData(page("CgtGSVhUVVJFAAAA%3D%3D_-=")) == "CgtGSVhUVVJFAAAA%3D%3D_-=");
    CHECK(visitorData(page(std::string(1024, 'A'))) == std::string(1024, 'A'));
    CHECK(visitorData(page(std::string(1025, 'A'))).empty());
    CHECK(visitorData(page("CgtGSVhUVVJF\r\nX-Injected: 1")).empty());
    CHECK(visitorData(page("CgtGSVhU VVJF")).empty());
    CHECK(visitorData(page("")).empty());
}
