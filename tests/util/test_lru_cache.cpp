// tests/util/test_lru_cache.cpp
// Unit tests for the generic utils::LruCache template.

#include <gtest/gtest.h>

#include <string>

#include "lru_cache.h"

TEST(LruCache, PutAndGetRoundTrip)
{
    utils::LruCache<std::string, int> cache(3);

    cache.put("a", 1);
    cache.put("b", 2);

    ASSERT_TRUE(cache.contains("a"));
    ASSERT_NE(cache.get("a"), nullptr);
    EXPECT_EQ(*cache.get("a"), 1);
    EXPECT_EQ(*cache.get("b"), 2);
    EXPECT_EQ(cache.size(), 2u);
}

TEST(LruCache, GetOnMissingKeyReturnsNullptr)
{
    utils::LruCache<std::string, int> cache(3);
    cache.put("a", 1);

    EXPECT_EQ(cache.get("missing"), nullptr);
}

TEST(LruCache, PutOverwritesExistingValueWithoutGrowingSize)
{
    utils::LruCache<std::string, int> cache(3);

    cache.put("a", 1);
    cache.put("a", 42);

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(*cache.get("a"), 42);
}

TEST(LruCache, EvictsLeastRecentlyUsedEntryWhenAtCapacity)
{
    utils::LruCache<std::string, int> cache(2);

    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);  // capacity 2 -> evicts "a" (least recently used).

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_EQ(cache.size(), 2u);
}

TEST(LruCache, GetPromotesEntryToMostRecentlyUsed)
{
    utils::LruCache<std::string, int> cache(2);

    cache.put("a", 1);
    cache.put("b", 2);
    cache.get("a");     // "a" is now MRU, "b" becomes LRU.
    cache.put("c", 3);  // Should evict "b", not "a".

    EXPECT_TRUE(cache.contains("a"));
    EXPECT_FALSE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
}

TEST(LruCache, PutOnExistingKeyPromotesToMostRecentlyUsed)
{
    utils::LruCache<std::string, int> cache(2);

    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("a", 99);  // Refresh "a"; "b" becomes LRU.
    cache.put("c", 3);   // Should evict "b".

    EXPECT_TRUE(cache.contains("a"));
    EXPECT_EQ(*cache.get("a"), 99);
    EXPECT_FALSE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
}

TEST(LruCache, EraseRemovesEntryAndReturnsWhetherItExisted)
{
    utils::LruCache<std::string, int> cache(3);
    cache.put("a", 1);

    EXPECT_TRUE(cache.erase("a"));
    EXPECT_FALSE(cache.contains("a"));
    EXPECT_FALSE(cache.erase("a"));
}

TEST(LruCache, ClearRemovesAllEntries)
{
    utils::LruCache<std::string, int> cache(3);
    cache.put("a", 1);
    cache.put("b", 2);

    cache.clear();

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.contains("a"));
}

TEST(LruCache, LruKeyReflectsCurrentEvictionCandidateWithoutMutating)
{
    utils::LruCache<std::string, int> cache(3);
    cache.put("a", 1);
    cache.put("b", 2);

    ASSERT_NE(cache.lru_key(), nullptr);
    EXPECT_EQ(*cache.lru_key(), "a");

    // Peeking at the LRU key must not itself change recency order.
    ASSERT_NE(cache.lru_key(), nullptr);
    EXPECT_EQ(*cache.lru_key(), "a");
}

TEST(LruCache, CapacityOneKeepsOnlyMostRecentlyUsedEntry)
{
    utils::LruCache<std::string, int> cache(1);

    cache.put("a", 1);
    cache.put("b", 2);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_EQ(cache.size(), 1u);
}
