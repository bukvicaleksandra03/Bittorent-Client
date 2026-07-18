// tests/dht/test_krpc.cpp
// Unit tests for KRPC message builders and the parse_krpc parser

#include <gtest/gtest.h>

#include "bencode/bencode_encoder.h"
#include "bencode/bencode_parser.h"
#include "bencode/bencode_types.h"
#include "dht/krpc.h"
#include "dht/node_id.h"

// ===========================================================================
// KRPC message builders + parser
// ===========================================================================

static dht::NodeId make_id(const std::string& s)
{
    return dht::NodeId::from_string(s);
}

// Helper: parse a bencode string and return the root BDict* (asserts non-null).
static BDict* parse_dict(const std::string& encoded,
                          bencode::Parser& out_parser,
                          std::shared_ptr<BType>& out_root)
{
    out_parser = bencode::Parser(encoded, true);
    out_root   = out_parser.parse_value();
    return dynamic_cast<BDict*>(out_root.get());
}

TEST(Krpc, Ping)
{
    dht::NodeId self = make_id("abcdefghij0123456789");
    std::string msg  = dht::make_ping("aa", self);

    bencode::Parser parser(msg, true);
    auto root_ptr = parser.parse_value();
    ASSERT_NE(root_ptr, nullptr);
    auto* d = dynamic_cast<BDict*>(root_ptr.get());
    ASSERT_NE(d, nullptr);

    auto* y = dynamic_cast<BString*>(d->content.at("y").get());
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->content, "q");

    auto* q = dynamic_cast<BString*>(d->content.at("q").get());
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->content, "ping");

    auto* t = dynamic_cast<BString*>(d->content.at("t").get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->content, "aa");

    auto* a = dynamic_cast<BDict*>(d->content.at("a").get());
    ASSERT_NE(a, nullptr);
    auto* id_node = dynamic_cast<BString*>(a->content.at("id").get());
    ASSERT_NE(id_node, nullptr);
    EXPECT_EQ(id_node->content, self.to_string());
}

TEST(Krpc, FindNode)
{
    dht::NodeId self   = make_id("abcdefghij0123456789");
    dht::NodeId target = make_id("01234567890123456789");
    std::string msg    = dht::make_find_node("bb", self, target);

    bencode::Parser parser(msg, true);
    auto root_ptr = parser.parse_value();
    ASSERT_NE(root_ptr, nullptr);
    auto* d = dynamic_cast<BDict*>(root_ptr.get());
    ASSERT_NE(d, nullptr);

    auto* q = dynamic_cast<BString*>(d->content.at("q").get());
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->content, "find_node");

    auto* a = dynamic_cast<BDict*>(d->content.at("a").get());
    ASSERT_NE(a, nullptr);
    auto* tgt = dynamic_cast<BString*>(a->content.at("target").get());
    ASSERT_NE(tgt, nullptr);
    EXPECT_EQ(tgt->content, target.to_string());
}

TEST(Krpc, GetPeers)
{
    dht::NodeId  self      = make_id("abcdefghij0123456789");
    std::string  info_hash = "ihihihihihihihihihih";   // 20 bytes
    std::string  msg       = dht::make_get_peers("cc", self, info_hash);

    bencode::Parser parser(msg, true);
    auto root_ptr = parser.parse_value();
    ASSERT_NE(root_ptr, nullptr);
    auto* d = dynamic_cast<BDict*>(root_ptr.get());
    ASSERT_NE(d, nullptr);

    auto* q = dynamic_cast<BString*>(d->content.at("q").get());
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->content, "get_peers");

    auto* a = dynamic_cast<BDict*>(d->content.at("a").get());
    ASSERT_NE(a, nullptr);
    auto* ih = dynamic_cast<BString*>(a->content.at("info_hash").get());
    ASSERT_NE(ih, nullptr);
    EXPECT_EQ(ih->content, info_hash);
}

TEST(Krpc, AnnounceResponse)
{
    // Build a minimal "r" (response) message and parse it back.
    dht::NodeId self = make_id("abcdefghij0123456789");
    std::string msg = bencode::dict({
        {"t", bencode::string("dd")},
        {"y", bencode::string("r")},
        {"r", bencode::dict({{"id", bencode::string(self.to_string())}})},
    });

    auto opt = dht::parse_krpc(msg);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->type, dht::KrpcType::Response);
    EXPECT_EQ(opt->txn,  "dd");
    EXPECT_EQ(opt->sender_id.to_string(), self.to_string());
}

TEST(Krpc, ErrorMessage)
{
    std::string msg = bencode::dict({
        {"t", bencode::string("ee")},
        {"y", bencode::string("e")},
        {"e", bencode::list({bencode::integer(201), bencode::string("Generic Error")})},
    });

    auto opt = dht::parse_krpc(msg);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->type, dht::KrpcType::Error);
    EXPECT_EQ(opt->txn,  "ee");
    EXPECT_EQ(opt->error_code, 201);
    EXPECT_EQ(opt->error_msg,  "Generic Error");
}

TEST(Krpc, ParseInvalid)
{
    // parse_krpc returns std::nullopt on bad input – it does NOT throw.
    EXPECT_FALSE(dht::parse_krpc("not bencoded").has_value());
    EXPECT_FALSE(dht::parse_krpc("i42e").has_value());  // integer, not dict
    EXPECT_FALSE(dht::parse_krpc("").has_value());
}
