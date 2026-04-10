#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "logger.h"

// Type trait mapping
template <typename T>
struct BTypeToEnum;

struct BType
{
    enum class Type
    {
        Integer,
        String,
        List,
        Dictionary
    };

    virtual ~BType() = default;
    virtual Type type() const = 0;
    virtual void print(std::ostream& os, int indent = 0) const = 0;
};

struct BInteger : public BType
{
    int64_t value;

    virtual Type type() const override
    {
        return Type::Integer;
    }
    void print(std::ostream& os, int indent = 0) const override
    {
        os << std::string(indent, ' ') << value;
    }
};

struct BString : public BType
{
    std::string content;

    Type type() const override
    {
        return Type::String;
    }
    void print(std::ostream& os, int indent = 0) const override
    {
        os << std::string(indent, ' ') << '"' << content << '"';
    }
};

struct BList : public BType
{
    std::vector<std::shared_ptr<BType>> content;

    Type type() const override
    {
        return Type::List;
    }
    void print(std::ostream& os, int indent = 0) const override
    {
        os << std::string(indent, ' ') << "[\n";
        for (const auto& item : content)
        {
            item->print(os, indent + 2);
            os << "\n";
        }
        os << std::string(indent, ' ') << "]";
    }
};

struct BDict : public BType
{
    std::map<std::string, std::shared_ptr<BType>> content;

    Type type() const override
    {
        return Type::Dictionary;
    }
    void print(std::ostream& os, int indent = 0) const override
    {
        os << std::string(indent, ' ') << "{\n";
        for (const auto& [key, value] : content)
        {
            os << std::string(indent + 2, ' ') << '"' << key << "\": ";
            os << std::endl;
            value->print(os, indent + 4);
            os << "\n";
        }
        os << std::string(indent, ' ') << "}";
    }

    template <typename T>
    T* get_val(const std::string& key) const
    {
        auto it = content.find(key);
        if (it == content.end())
            LOG_AND_THROW("The dictionary doesn't contain the key: " + key);
        if (it->second->type() != BTypeToEnum<T>::value)
            LOG_AND_THROW("Wrong type of the value for key: " + key);
        return dynamic_cast<T*>(it->second.get());
    }

    BType* operator[](const std::string& key) const
    {
        auto it = content.find(key);
        if (it == content.end())
            throw std::out_of_range("Key \"" + key +
                                    "\" not found in the dictionary.");
        return it->second.get();
    }

    bool has_key(std::string field)
    {
        return content.find(field) != content.end();
    }
};

template <typename T>
T* as(BType* obj)
{
    if (!obj)
        LOG_AND_THROW("Null pointer encountered");
    if (obj->type() != BTypeToEnum<T>::value)
    {
        LOG_AND_THROW("Unexpected BType");
    }
    return dynamic_cast<T*>(obj);
};

// BTypeToEnum specializations - map each type to its enum value
template <>
struct BTypeToEnum<BString>
{
    static constexpr BType::Type value = BType::Type::String;
};

template <>
struct BTypeToEnum<BInteger>
{
    static constexpr BType::Type value = BType::Type::Integer;
};

template <>
struct BTypeToEnum<BList>
{
    static constexpr BType::Type value = BType::Type::List;
};

template <>
struct BTypeToEnum<BDict>
{
    static constexpr BType::Type value = BType::Type::Dictionary;
};
