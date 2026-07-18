// tests/bencode/test_encoder.cpp
// Unit tests for the bencode encoder (bencode::integer / string / list / dict)
// and a round-trip test that also exercises the parser.

#include <gtest/gtest.h>

#include "bencode/bencode_encoder.h"
#include "bencode/bencode_parser.h"
#include "bencode/bencode_types.h"

// ===========================================================================
// Bencode encoder
// ===========================================================================

TEST(BencodeEncoder, Integer)
{
    EXPECT_EQ(bencode::integer(0),   "i0e");
    EXPECT_EQ(bencode::integer(42),  "i42e");
    EXPECT_EQ(bencode::integer(-7),  "i-7e");
}

TEST(BencodeEncoder, String)
{
    EXPECT_EQ(bencode::string(""),      "0:");
    EXPECT_EQ(bencode::string("spam"),  "4:spam");
    EXPECT_EQ(bencode::string("hello"), "5:hello");
}

TEST(BencodeEncoder, List)
{
    // empty list
    EXPECT_EQ(bencode::list({}), "le");

    // single element
    EXPECT_EQ(bencode::list({bencode::integer(1)}), "li1ee");

    // multiple elements
    std::string got = bencode::list({bencode::string("a"), bencode::integer(2)});
    EXPECT_EQ(got, "l1:ai2ee");
}

TEST(BencodeEncoder, Dict)
{
    // empty dict
    EXPECT_EQ(bencode::dict({}), "de");

    // single key – value
    EXPECT_EQ(bencode::dict({{"k", bencode::string("v")}}), "d1:k1:ve");

    // keys must be sorted lexicographically
    std::string got = bencode::dict({
        {"z", bencode::integer(2)},
        {"a", bencode::integer(1)},
    });
    EXPECT_EQ(got, "d1:ai1e1:zi2ee");
}

TEST(BencodeEncoder, RoundTrip)
{
    // Build a small KRPC-like dict, then parse it back with bencode::Parser
    // and verify the values survive the round-trip.
    std::string encoded = bencode::dict({
        {"t", bencode::string("aa")},
        {"y", bencode::string("q")},
        {"q", bencode::string("ping")},
        {"a", bencode::dict({{"id", bencode::string("abcdefghij0123456789")}})},
    });

    bencode::Parser parser(encoded, true);
    auto root_ptr = parser.parse_value();
    ASSERT_NE(root_ptr, nullptr);

    auto* d = dynamic_cast<BDict*>(root_ptr.get());
    ASSERT_NE(d, nullptr);

    auto* y_node = dynamic_cast<BString*>(d->content.at("y").get());
    ASSERT_NE(y_node, nullptr);
    EXPECT_EQ(y_node->content, "q");

    auto* q_node = dynamic_cast<BString*>(d->content.at("q").get());
    ASSERT_NE(q_node, nullptr);
    EXPECT_EQ(q_node->content, "ping");

    auto* t_node = dynamic_cast<BString*>(d->content.at("t").get());
    ASSERT_NE(t_node, nullptr);
    EXPECT_EQ(t_node->content, "aa");

    auto* a_node = dynamic_cast<BDict*>(d->content.at("a").get());
    ASSERT_NE(a_node, nullptr);

    auto* id_node = dynamic_cast<BString*>(a_node->content.at("id").get());
    ASSERT_NE(id_node, nullptr);
    EXPECT_EQ(id_node->content, "abcdefghij0123456789");
}
