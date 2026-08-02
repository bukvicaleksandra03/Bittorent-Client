#pragma once

#include <cassert>
#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>

namespace utils
{

// Fixed-capacity key/value cache with least-recently-used eviction.
//
// Backed by a doubly-linked list (recency order, MRU at the front) plus a
// hash map from Key to a list iterator, so get()/put()/erase() are all O(1)
// average case -- no linear scan is needed to find the eviction candidate.
//
// get() and put() both count as a "use" and move the entry to the front of
// the recency list. Once size() == capacity(), the next put() for a new key
// evicts the entry at the back of the list (the LRU one) before inserting.
//
// Not thread-safe; callers that share an LruCache across threads must guard
// it externally (e.g. with the same mutex already protecting the structure
// it replaces).
template <typename Key, typename Value>
class LruCache
{
   public:
    using ListType = std::list<std::pair<Key, Value>>;
    using MapType = std::unordered_map<Key, typename ListType::iterator>;
    using iterator = typename ListType::iterator;
    using const_iterator = typename ListType::const_iterator;

    // capacity must be at least 1.
    explicit LruCache(size_t capacity) : capacity_(capacity)
    {
        assert(capacity_ > 0 && "LruCache capacity must be at least 1");
    }

    size_t size() const
    {
        return index_.size();
    }

    bool empty() const
    {
        return index_.empty();
    }

    size_t capacity() const
    {
        return capacity_;
    }

    bool contains(const Key& key) const
    {
        return index_.find(key) != index_.end();
    }

    // Returns a pointer to the value and promotes it to most-recently-used,
    // or nullptr if key is not present. The pointer is invalidated by any
    // later call that erases key (erase(key), or a put() for a different
    // key that evicts this entry).
    Value* get(const Key& key)
    {
        auto map_it = index_.find(key);
        if (map_it == index_.end())
            return nullptr;

        // Move the node this iterator points to from its current position to
        // the front of items_, in O(1) and without invalidating any other
        // iterator (including map_it->second itself, which still points at
        // the same node -- it's just relinked, not copied or reallocated).
        items_.splice(items_.begin(), items_, map_it->second);
        return &map_it->second->second;
    }

    // Inserts or overwrites the value for key and promotes it to
    // most-recently-used. If key is new and the cache is already at
    // capacity, the current LRU entry is evicted first. Returns a
    // reference to the stored value.
    template <typename V>
    Value& put(const Key& key, V&& value)
    {
        auto map_it = index_.find(key);
        if (map_it != index_.end())
        {
                    map_it->second->second = std::forward<V>(value);
                    // Same as in get(): move the existing node to the front
                    // of items_ in O(1), leaving map_it->second valid and
                    // still pointing at it.
                    items_.splice(items_.begin(), items_, map_it->second);
                    return map_it->second->second;
        }

        if (index_.size() >= capacity_)
            evict_lru();

        items_.emplace_front(key, std::forward<V>(value));
        index_[key] = items_.begin();
        return items_.front().second;
    }

    // Removes key if present. Returns true if an entry was removed.
    bool erase(const Key& key)
    {
        auto map_it = index_.find(key);
        if (map_it == index_.end())
            return false;

        items_.erase(map_it->second);
        index_.erase(map_it);
        return true;
    }

    // Removes the entry at pos. Returns an iterator to the entry that
    // followed it, mirroring std::unordered_map::erase(iterator) -- this
    // lets callers walk the whole cache with begin()/end() and erase
    // entries in place, same as the map this class replaces.
    iterator erase(iterator pos)
    {
        index_.erase(pos->first);
        return items_.erase(pos);
    }

    // Iterates all entries in recency order (front = most-recently-used).
    // Iteration order is otherwise unspecified to callers and must not be
    // relied upon beyond "some order over all entries".
    iterator begin()
    {
        return items_.begin();
    }

    iterator end()
    {
        return items_.end();
    }

    const_iterator begin() const
    {
        return items_.begin();
    }

    const_iterator end() const
    {
        return items_.end();
    }

    void clear()
    {
        items_.clear();
        index_.clear();
    }

    // Key of the current least-recently-used entry, or nullptr if empty.
    // Does not affect recency order.
    const Key* lru_key() const
    {
        return items_.empty() ? nullptr : &items_.back().first;
    }

   private:
    void evict_lru()
    {
        if (items_.empty())
            return;
        index_.erase(items_.back().first);
        items_.pop_back();
    }

    size_t capacity_;
    ListType items_;  // front = most recently used, back = least
    MapType index_;
};

}  // namespace utils
