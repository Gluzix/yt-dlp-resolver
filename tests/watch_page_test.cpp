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
    CHECK(visitorData(html).value == "CgtGSVhUVVJFAAAA%3D%3D");
}

TEST_CASE("braces and quotes inside strings do not end the object")
{
    const std::string html = R"(ytcfg.set({"A":"} { \" }","INNERTUBE_CONTEXT":{"client":{"visitorData":"v1"}}});)";
    CHECK(visitorData(html).value == "v1");
}

TEST_CASE("a config without visitor data is passed over for a later one")
{
    const std::string html = R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{}}}); )"
                             R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":"v2"}}});)";
    CHECK(visitorData(html).value == "v2");
}

TEST_CASE("no ytcfg, or a broken one, gives no visitor data and refuses none")
{
    const std::string pages[] = {
        "",
        "<html>Before you continue to YouTube</html>",
        R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":"v3"})", // never closes
        R"(ytcfg.set({not json});)",
        R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{}}});)",
    };
    for (const std::string &html : pages) {
        CAPTURE(html);
        const auto found = visitorData(html);
        CHECK(found.value.empty());
        CHECK(found.refused.empty());
    }
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

TEST_CASE("visitor data unfit to travel as a header is refused, saying why")
{
    const auto page = [](const nlohmann::json &value) {
        nlohmann::json config;
        config["INNERTUBE_CONTEXT"]["client"]["visitorData"] = value;
        return "ytcfg.set(" + config.dump() + ");";
    };
    const std::string base64 = "CgtGSVhUVVJFAAAA+/=_-%3D";
    CHECK(visitorData(page(base64)).value == base64);
    CHECK(visitorData(page(std::string(4096, 'A'))).value == std::string(4096, 'A'));

    const auto tooLong = visitorData(page(std::string(4097, 'A')));
    CHECK(tooLong.value.empty());
    CHECK(tooLong.refused.find("4097") != std::string::npos);

    const auto injected = visitorData(page("CgtGSVhUVVJF\r\nX-Injected: 1"));
    CHECK(injected.value.empty());
    CHECK(injected.refused.find("character") != std::string::npos);

    CHECK_FALSE(visitorData(page("CgtGSVhU VVJF")).refused.empty());
    CHECK_FALSE(visitorData(page("")).refused.empty());
    CHECK(visitorData(page(42)).refused == "not a string");
}

TEST_CASE("a refused value does not hide a fit one in a later ytcfg")
{
    const std::string html = R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":"bad value"}}}); )"
                             R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":"v4"}}});)";
    const auto found = visitorData(html);
    CHECK(found.value == "v4");
    CHECK(found.refused.empty());
}
