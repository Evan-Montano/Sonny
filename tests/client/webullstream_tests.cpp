#include "client/webullstream.hpp"
#include "common/jsonutil.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

TEST(WebullEnvironmentTests, EndpointsAndSecretScopeStayPaired) {
    const auto& live = Client::GetWebullEnvironmentConfig(Client::WebullEnvironment::Live);
    const auto& sandbox = Client::GetWebullEnvironmentConfig(Client::WebullEnvironment::Sandbox);
    EXPECT_STREQ(live.api_host, "api.webull.com");
    EXPECT_STREQ(live.mqtt_host, "data-api.webull.com");
    EXPECT_STREQ(live.secret_environment, "live");
    EXPECT_STREQ(sandbox.api_host, "api.sandbox.webull.com");
    EXPECT_STREQ(sandbox.mqtt_host, "data-api.sandbox.webull.com");
    EXPECT_STREQ(sandbox.secret_environment, "sandbox");
}

TEST(WebullSubscriptionTests, BuildsQuoteSubscriptionWithSharedSessionId) {
    const std::string serialized = Client::BuildWebullSubscriptionBody("session-unique");
    EXPECT_EQ(serialized, R"({"category":"US_ETF","grab":"true","overnight_required":false,"session_id":"session-unique","sub_types":["QUOTE"],"symbols":["SPY"]})");
    const Common::JsonUtility body(serialized);
    EXPECT_EQ(body.Get<std::string>("session_id"), "session-unique");
    EXPECT_EQ(body.Get<std::vector<std::string>>("symbols"), std::vector<std::string>{"SPY"});
    EXPECT_EQ(body.Get<std::string>("category"), "US_ETF");
    EXPECT_EQ(body.Get<std::vector<std::string>>("sub_types"), std::vector<std::string>{"QUOTE"});
    EXPECT_EQ(body.Get<std::string>("grab"), "true");
    EXPECT_FALSE(body.Get<bool>("overnight_required"));
}

TEST(WebullSigningTests, MatchesOfficialSignatureExample) {
    const std::string body = R"({"k1":123,"k2":"this is the api request body","k3":true,"k4":{"foo":[1,2]}})";
    EXPECT_EQ(Client::GenerateWebullSignature(
        "/trade/place_order", body,
        "776da210ab4a452795d74e726ebd74b6", "0f50a2e853334a9aae1a783bee120c1f",
        "api.webull.com", "2022-01-04T03:55:31Z", "48ef5afed43d4d91ae514aaeafbc29ba",
        {{"a1", "webull"}, {"a2", "123"}, {"a3", "xxx"}, {"q1", "yyy"}}
    ), "kvlS6opdZDhEBo5jq40nHYXaLvM=");
}
