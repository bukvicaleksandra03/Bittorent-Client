#include <gtest/gtest.h>

#include <array>
#include <string>

#include "bencode/bencode_encoder.h"
#include "info_hash.h"
#include "trackers/http_tracker_communicator.h"

static InfoHash make_test_info_hash()
{
    InfoHash h;
    for (size_t i = 0; i < h.bytes.size(); ++i)
    {
        h.bytes[i] = static_cast<uint8_t>(i);
    }
    return h;
}

TEST(TrackerScrapePath, ConvertsTrailingAnnounce)
{
    TrackerDetails announce{TrackerProtocol::HTTP, "example.com", 80,
                            "/announce"};
    EXPECT_EQ(scrape_tracker_from_announce(announce).path, "/scrape");

    announce.path = "/foo/announce";
    EXPECT_EQ(scrape_tracker_from_announce(announce).path, "/foo/scrape");
}

TEST(TrackerScrapePath, ConvertsInPathAnnounce)
{
    TrackerDetails announce{TrackerProtocol::HTTP, "example.com", 80,
                            "/tracker/announce/foo"};
    EXPECT_EQ(scrape_tracker_from_announce(announce).path,
              "/tracker/scrape/foo");
}

TEST(TrackerScrapePath, BareAnnounceAndFallback)
{
    TrackerDetails announce{TrackerProtocol::HTTP, "example.com", 80, "announce"};
    EXPECT_EQ(scrape_tracker_from_announce(announce).path, "scrape");

    announce.path = "/tracker";
    EXPECT_EQ(scrape_tracker_from_announce(announce).path, "/scrape");
}

TEST(TrackerScrapePath, PreservesHostAndPort)
{
    TrackerDetails tracker{TrackerProtocol::HTTP, "bt1.archive.org", 6969,
                           "/announce"};
    const TrackerDetails scrape = scrape_tracker_from_announce(tracker);

    EXPECT_EQ(scrape.protocol, TrackerProtocol::HTTP);
    EXPECT_EQ(scrape.hostname, "bt1.archive.org");
    EXPECT_EQ(scrape.port, 6969);
    EXPECT_EQ(scrape.path, "/scrape");
    EXPECT_EQ(scrape.to_string(), "http://bt1.archive.org:6969/scrape");
}

static std::string build_scrape_body(const InfoHash& info_hash,
                                     int64_t complete,
                                     int64_t incomplete,
                                     int64_t downloaded,
                                     const char* downloaded_key = "downloaded")
{
    const std::string hash_key = info_hash.to_raw();
    const std::string file_stats =
        bencode::dict({{downloaded_key, bencode::integer(downloaded)},
                       {"complete", bencode::integer(complete)},
                       {"incomplete", bencode::integer(incomplete)}});
    const std::string files =
        bencode::dict({{hash_key, file_stats}});
    return bencode::dict({{"files", files}});
}

TEST(HttpScrapeParser, ParsesSwarmStats)
{
    const InfoHash info_hash = make_test_info_hash();
    const std::string body = build_scrape_body(info_hash, 42, 7, 100);

    const TrackerScrapeStats stats =
        parse_http_scrape_response(body, info_hash);

    EXPECT_EQ(stats.complete, 42u);
    EXPECT_EQ(stats.incomplete, 7u);
    ASSERT_TRUE(stats.downloaded.has_value());
    EXPECT_EQ(*stats.downloaded, 100u);
}

TEST(HttpScrapeParser, AcceptsDownloadKeyAlias)
{
    const InfoHash info_hash = make_test_info_hash();
    const std::string body =
        build_scrape_body(info_hash, 1, 2, 3, "download");

    const TrackerScrapeStats stats =
        parse_http_scrape_response(body, info_hash);

    ASSERT_TRUE(stats.downloaded.has_value());
    EXPECT_EQ(*stats.downloaded, 3u);
}

TEST(HttpScrapeParser, MissingHashReturnsZeros)
{
    const InfoHash info_hash = make_test_info_hash();
    const InfoHash other = InfoHash::from_sha1(
        crypto::SHA1Hash{});  // different hash, all zero bytes

    const std::string body = build_scrape_body(other, 10, 20, 30);

    const TrackerScrapeStats stats =
        parse_http_scrape_response(body, info_hash);

    EXPECT_EQ(stats.complete, 0u);
    EXPECT_EQ(stats.incomplete, 0u);
    EXPECT_FALSE(stats.downloaded.has_value());
}

TEST(HttpScrapeParser, EmptyFilesReturnsZeros)
{
    const InfoHash info_hash = make_test_info_hash();
    const std::string body = bencode::dict({{"files", bencode::dict({})}});

    const TrackerScrapeStats stats =
        parse_http_scrape_response(body, info_hash);

    EXPECT_EQ(stats.complete, 0u);
    EXPECT_EQ(stats.incomplete, 0u);
}

TEST(HttpScrapeParser, FailureReasonThrows)
{
    const InfoHash info_hash = make_test_info_hash();
    const std::string body =
        bencode::dict({{"failure reason", bencode::string("invalid passkey")}});

    EXPECT_THROW(parse_http_scrape_response(body, info_hash),
                 std::runtime_error);
}

TEST(InfoHash, UrlEncodedPercentEncodesRawBytes)
{
    const InfoHash info_hash = make_test_info_hash();
    const std::string encoded = info_hash.url_encoded();

    // Non-printable bytes (0x00-0x13 here) expand to %XX triplets.
    EXPECT_GT(encoded.size(), info_hash.bytes.size());
    EXPECT_NE(encoded.find('%'), std::string::npos);
    EXPECT_EQ(encoded.compare(0, 3, "%00"), 0);
}
