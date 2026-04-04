#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "bencode/bencode_parser.h"
#include "bencode/bencode_types.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: parse a raw bencode string into a single BType value
// ---------------------------------------------------------------------------
static std::shared_ptr<BType> parse_raw(const std::string& input)
{
    BencodeParser parser(input, true);
    return parser.parse_value();
}

// ===========================================================================
// Integer parsing
// ===========================================================================

TEST(BencodeInteger, PositiveNumber)
{
    auto val = parse_raw("i42e");
    ASSERT_EQ(val->type(), BType::Type::Integer);
    EXPECT_EQ(as<BInteger>(val.get())->value, 42);
}

TEST(BencodeInteger, NegativeNumber)
{
    auto val = parse_raw("i-7e");
    ASSERT_EQ(val->type(), BType::Type::Integer);
    EXPECT_EQ(as<BInteger>(val.get())->value, -7);
}

TEST(BencodeInteger, Zero)
{
    auto val = parse_raw("i0e");
    ASSERT_EQ(val->type(), BType::Type::Integer);
    EXPECT_EQ(as<BInteger>(val.get())->value, 0);
}

TEST(BencodeInteger, LargeNumber)
{
    auto val = parse_raw("i9999999999e");
    ASSERT_EQ(val->type(), BType::Type::Integer);
    EXPECT_EQ(as<BInteger>(val.get())->value, 9999999999LL);
}

// ===========================================================================
// String parsing
// ===========================================================================

TEST(BencodeString, SimpleString)
{
    auto val = parse_raw("5:hello");
    ASSERT_EQ(val->type(), BType::Type::String);
    EXPECT_EQ(as<BString>(val.get())->content, "hello");
}

TEST(BencodeString, EmptyString)
{
    auto val = parse_raw("0:");
    ASSERT_EQ(val->type(), BType::Type::String);
    EXPECT_EQ(as<BString>(val.get())->content, "");
}

TEST(BencodeString, StringWithSpaces)
{
    auto val = parse_raw("11:hello world");
    ASSERT_EQ(val->type(), BType::Type::String);
    EXPECT_EQ(as<BString>(val.get())->content, "hello world");
}

TEST(BencodeString, SingleCharacter)
{
    auto val = parse_raw("1:x");
    ASSERT_EQ(val->type(), BType::Type::String);
    EXPECT_EQ(as<BString>(val.get())->content, "x");
}

// ===========================================================================
// List parsing
// ===========================================================================

TEST(BencodeList, EmptyList)
{
    auto val = parse_raw("le");
    ASSERT_EQ(val->type(), BType::Type::List);
    EXPECT_TRUE(as<BList>(val.get())->content.empty());
}

TEST(BencodeList, ListOfIntegers)
{
    auto val = parse_raw("li1ei2ei3ee");
    ASSERT_EQ(val->type(), BType::Type::List);

    auto& items = as<BList>(val.get())->content;
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(as<BInteger>(items[0].get())->value, 1);
    EXPECT_EQ(as<BInteger>(items[1].get())->value, 2);
    EXPECT_EQ(as<BInteger>(items[2].get())->value, 3);
}

TEST(BencodeList, ListOfStrings)
{
    auto val = parse_raw("l3:foo3:bare");
    ASSERT_EQ(val->type(), BType::Type::List);

    auto& items = as<BList>(val.get())->content;
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(as<BString>(items[0].get())->content, "foo");
    EXPECT_EQ(as<BString>(items[1].get())->content, "bar");
}

TEST(BencodeList, MixedTypeList)
{
    auto val = parse_raw("li42e4:spam3:egge");
    ASSERT_EQ(val->type(), BType::Type::List);

    auto& items = as<BList>(val.get())->content;
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0]->type(), BType::Type::Integer);
    EXPECT_EQ(items[1]->type(), BType::Type::String);
    EXPECT_EQ(items[2]->type(), BType::Type::String);
}

TEST(BencodeList, NestedList)
{
    auto val = parse_raw("lli1ei2eeli3eee");
    ASSERT_EQ(val->type(), BType::Type::List);

    auto& outer = as<BList>(val.get())->content;
    ASSERT_EQ(outer.size(), 2u);

    auto& inner1 = as<BList>(outer[0].get())->content;
    ASSERT_EQ(inner1.size(), 2u);
    EXPECT_EQ(as<BInteger>(inner1[0].get())->value, 1);
    EXPECT_EQ(as<BInteger>(inner1[1].get())->value, 2);

    auto& inner2 = as<BList>(outer[1].get())->content;
    ASSERT_EQ(inner2.size(), 1u);
    EXPECT_EQ(as<BInteger>(inner2[0].get())->value, 3);
}

// ===========================================================================
// Dictionary parsing
// ===========================================================================

TEST(BencodeDict, EmptyDict)
{
    auto val = parse_raw("de");
    ASSERT_EQ(val->type(), BType::Type::Dictionary);
    EXPECT_TRUE(as<BDict>(val.get())->content.empty());
}

TEST(BencodeDict, SimpleDict)
{
    auto val = parse_raw("d3:cow3:moo4:spam4:eggse");
    ASSERT_EQ(val->type(), BType::Type::Dictionary);

    auto* dict = as<BDict>(val.get());
    EXPECT_EQ(dict->get_val<BString>("cow")->content, "moo");
    EXPECT_EQ(dict->get_val<BString>("spam")->content, "eggs");
}

TEST(BencodeDict, DictWithInteger)
{
    auto val = parse_raw("d3:agei25e4:name5:Alicee");
    ASSERT_EQ(val->type(), BType::Type::Dictionary);

    auto* dict = as<BDict>(val.get());
    EXPECT_EQ(dict->get_val<BInteger>("age")->value, 25);
    EXPECT_EQ(dict->get_val<BString>("name")->content, "Alice");
}

TEST(BencodeDict, NestedDict)
{
    auto val = parse_raw("d5:innerd3:foo3:baree");
    ASSERT_EQ(val->type(), BType::Type::Dictionary);

    auto* outer = as<BDict>(val.get());
    auto* inner = outer->get_val<BDict>("inner");
    EXPECT_EQ(inner->get_val<BString>("foo")->content, "bar");
}

TEST(BencodeDict, DictWithList)
{
    auto val = parse_raw("d5:itemsli1ei2ei3eee");
    ASSERT_EQ(val->type(), BType::Type::Dictionary);

    auto* dict = as<BDict>(val.get());
    auto* list = dict->get_val<BList>("items");
    ASSERT_EQ(list->content.size(), 3u);
    EXPECT_EQ(as<BInteger>(list->content[0].get())->value, 1);
}

TEST(BencodeDict, HasKeyCheck)
{
    auto val = parse_raw("d3:foo3:bare");
    auto* dict = as<BDict>(val.get());
    EXPECT_TRUE(dict->has_key("foo"));
    EXPECT_FALSE(dict->has_key("missing"));
}

TEST(BencodeDict, MissingKeyThrows)
{
    auto val = parse_raw("d3:foo3:bare");
    auto* dict = as<BDict>(val.get());
    EXPECT_THROW(dict->get_val<BString>("missing"), std::runtime_error);
}

TEST(BencodeDict, WrongTypeThrows)
{
    auto val = parse_raw("d3:foo3:bare");
    auto* dict = as<BDict>(val.get());
    EXPECT_THROW(dict->get_val<BInteger>("foo"), std::runtime_error);
}

TEST(BencodeDict, SubscriptOperator)
{
    auto val = parse_raw("d3:fooi99ee");
    auto* dict = as<BDict>(val.get());
    BType* item = (*dict)["foo"];
    ASSERT_EQ(item->type(), BType::Type::Integer);
    EXPECT_EQ(as<BInteger>(item)->value, 99);
}

TEST(BencodeDict, SubscriptMissingKeyThrows)
{
    auto val = parse_raw("d3:fooi1ee");
    auto* dict = as<BDict>(val.get());
    EXPECT_THROW((*dict)["nope"], std::out_of_range);
}

// ===========================================================================
// Error handling
// ===========================================================================

TEST(BencodeError, TruncatedInteger)
{
    EXPECT_THROW(parse_raw("i42"), std::runtime_error);
}

TEST(BencodeError, TruncatedString)
{
    EXPECT_THROW(parse_raw("5:hi"), std::runtime_error);
}

TEST(BencodeError, InvalidStartByte)
{
    EXPECT_THROW(parse_raw("x"), std::runtime_error);
}

TEST(BencodeError, TruncatedDict)
{
    EXPECT_THROW(parse_raw("d3:foo"), std::runtime_error);
}

// ===========================================================================
// Print / serialization
// ===========================================================================

TEST(BencodePrint, IntegerPrint)
{
    auto val = parse_raw("i42e");
    std::ostringstream oss;
    val->print(oss);
    EXPECT_EQ(oss.str(), "42");
}

TEST(BencodePrint, StringPrint)
{
    auto val = parse_raw("5:hello");
    std::ostringstream oss;
    val->print(oss);
    EXPECT_EQ(oss.str(), "\"hello\"");
}

// ===========================================================================
// File-based parsing (.torrent)
// ===========================================================================

static const char* TORRENT_FILE =
    "torrent_files/unparsed_torrents/"
    "annakarenina_mas_1202_librivox_archive.torrent";

TEST(BencodeTorrent, ParsesTorrentFile)
{
    if (!std::filesystem::exists(TORRENT_FILE))
        GTEST_SKIP() << "Torrent file not found: " << TORRENT_FILE;

    std::string path{TORRENT_FILE};
    BencodeParser parser(path);
    std::unique_ptr<TorrentFile> tf;
    ASSERT_NO_THROW(tf = parser.parse());
    ASSERT_NE(tf, nullptr);

    EXPECT_FALSE(tf->get_name().empty());
    EXPECT_GT(tf->get_total_size(), 0u);
    EXPECT_GT(tf->get_piece_size(), 0u);
    EXPECT_GT(tf->get_piece_count(), 0u);
    EXPECT_FALSE(tf->get_announce().empty());
    EXPECT_EQ(tf->get_info_hash_hex().size(), 40u);
}

TEST(BencodeTorrent, PrintDoesNotThrow)
{
    if (!std::filesystem::exists(TORRENT_FILE))
        GTEST_SKIP() << "Torrent file not found: " << TORRENT_FILE;

    std::string path{TORRENT_FILE};
    BencodeParser parser(path);
    auto tf = parser.parse();

    std::ostringstream oss;
    EXPECT_NO_THROW(tf->print(oss));
    EXPECT_FALSE(oss.str().empty());
}

// ===========================================================================
// Constructor from raw bytes
// ===========================================================================

TEST(BencodeRawBytes, ParseFromUint8Pointer)
{
    std::string input = "i100e";
    BencodeParser parser(reinterpret_cast<const uint8_t*>(input.data()),
                         input.size());
    auto val = parser.parse_value();
    ASSERT_EQ(val->type(), BType::Type::Integer);
    EXPECT_EQ(as<BInteger>(val.get())->value, 100);
}

// ===========================================================================
// Batch parsing: parse every .torrent and write printed output to disk
// ===========================================================================

static const fs::path UNPARSED_DIR = "torrent_files/unparsed_torrents";
static const fs::path PARSED_DIR = "torrent_files/parsed_torrents";

TEST(BencodeBatch, ParseAllTorrentsToFiles)
{
    ASSERT_TRUE(fs::exists(UNPARSED_DIR))
        << "Missing directory: " << UNPARSED_DIR;

    if (fs::exists(PARSED_DIR))
    {
        for (auto& entry : fs::directory_iterator(PARSED_DIR))
            fs::remove_all(entry.path());
    }
    fs::create_directories(PARSED_DIR);

    int parsed_count = 0;

    for (const auto& entry : fs::directory_iterator(UNPARSED_DIR))
    {
        if (!entry.is_regular_file())
            continue;

        const fs::path& torrent_path = entry.path();
        std::string path_str = torrent_path.string();

        BencodeParser parser(path_str);
        try
        {
            parser.parse();
        }
        catch (const std::exception& e)
        {
            FAIL() << "Failed to parse " << path_str << ": " << e.what();
        }

        fs::path output_file = PARSED_DIR / torrent_path.filename();

        std::ofstream ofs(output_file);
        ASSERT_TRUE(ofs.is_open())
            << "Could not open output file: " << output_file;

        parser.print(ofs);
        ofs.close();

        EXPECT_TRUE(fs::exists(output_file));
        EXPECT_GT(fs::file_size(output_file), 0u)
            << "Output file is empty: " << output_file;

        ++parsed_count;
    }

    EXPECT_GT(parsed_count, 0) << "No .torrent files found in " << UNPARSED_DIR;

    std::cout << "[  INFO  ] Parsed " << parsed_count << " torrent files into "
              << PARSED_DIR << std::endl;
}
