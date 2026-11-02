// webull_proto_tests.cpp

#include <gtest/gtest.h>

#include <string>

#include "webull/marketdata.pb.h"

using namespace webull::marketdata;

TEST(WebullProtoTests, QuoteSerializationRoundTrip)
{
    // Construct a quote.
    Quote original;

    auto* basic = original.mutable_basic();
    basic->set_symbol("SPY");
    basic->set_instrument_id("913243251");
    basic->set_timestamp("1793647497627");
    basic->set_session("RTH");

    auto* ask = original.add_asks();
    ask->set_price("754.93");
    ask->set_size("80");

    auto* bid = original.add_bids();
    bid->set_price("754.91");
    bid->set_size("226");

    // Serialize the quote into a binary string.
    std::string payload;

    ASSERT_TRUE(original.SerializeToString(&payload));
    ASSERT_FALSE(payload.empty());

    // Deserialize the binary string.
    Quote decoded;

    ASSERT_TRUE(decoded.ParseFromString(payload));

    // Verify metadata.
    ASSERT_TRUE(decoded.has_basic());

    EXPECT_EQ(decoded.basic().symbol(), "SPY");
    EXPECT_EQ(decoded.basic().instrument_id(), "913243251");
    EXPECT_EQ(decoded.basic().timestamp(), "1793647497627");
    EXPECT_EQ(decoded.basic().session(), "RTH");

    // Verify ask levels.
    ASSERT_EQ(decoded.asks_size(), 1);
    EXPECT_EQ(decoded.asks(0).price(), "754.93");
    EXPECT_EQ(decoded.asks(0).size(), "80");

    // Verify bid levels.
    ASSERT_EQ(decoded.bids_size(), 1);
    EXPECT_EQ(decoded.bids(0).price(), "754.91");
    EXPECT_EQ(decoded.bids(0).size(), "226");
}
