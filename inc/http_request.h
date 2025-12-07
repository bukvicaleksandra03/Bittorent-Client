#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

class HttpRequest
{
   public:
    enum class Method
    {
        GET,
        POST
    };

   private:
    Method _method = Method::GET;
    std::string _path = "/";
    std::string _host;
    std::vector<std::pair<std::string, std::string>> _query_params;
    std::vector<std::pair<std::string, std::string>> _headers;
    std::string _body;

   public:
    HttpRequest& method(Method m)
    {
        _method = m;
        return *this;
    }

    HttpRequest& path(const std::string& p)
    {
        _path = p;
        return *this;
    }

    HttpRequest& host(const std::string& h)
    {
        _host = h;
        return *this;
    }

    // Add a query parameter (will be URL-encoded)
    HttpRequest& query(const std::string& key, const std::string& value)
    {
        _query_params.emplace_back(key, value);
        return *this;
    }

    // Add a raw query parameter (already URL-encoded, won't be encoded again)
    HttpRequest& query_raw(const std::string& key, const std::string& value)
    {
        _query_params.emplace_back(key, "\x00" + value);  // Prefix to mark raw
        return *this;
    }

    HttpRequest& header(const std::string& key, const std::string& value)
    {
        _headers.emplace_back(key, value);
        return *this;
    }

    HttpRequest& body(const std::string& b)
    {
        _body = b;
        return *this;
    }

    // URL-encode a string (for query params)
    static std::string url_encode(const std::string& str)
    {
        std::ostringstream encoded;
        for (unsigned char c : str)
        {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                encoded << c;
            }
            else
            {
                encoded << '%' << std::uppercase << std::hex << std::setw(2)
                        << std::setfill('0') << static_cast<int>(c);
            }
        }
        return encoded.str();
    }

    // Build the query string: ?key1=val1&key2=val2
    std::string build_query_string() const
    {
        if (_query_params.empty())
        {
            return "";
        }

        std::ostringstream qs;
        qs << "?";
        for (size_t i = 0; i < _query_params.size(); ++i)
        {
            if (i > 0)
            {
                qs << "&";
            }

            const auto& key = _query_params[i].first;
            const auto& value = _query_params[i].second;

            // Check if value is marked as raw (prefixed with \x00)
            if (!value.empty() && value[0] == '\x00')
            {
                qs << url_encode(key) << "=" << value.substr(1);
            }
            else
            {
                qs << url_encode(key) << "=" << url_encode(value);
            }
        }
        return qs.str();
    }

    // Build the full HTTP request string
    std::string build() const
    {
        std::ostringstream req;

        // Request line: GET /path?query HTTP/1.1
        req << (_method == Method::GET ? "GET" : "POST") << " " << _path
            << build_query_string() << " HTTP/1.1\r\n";

        // Host header (required for HTTP/1.1)
        req << "Host: " << _host << "\r\n";

        // Custom headers
        for (const auto& h : _headers)
        {
            req << h.first << ": " << h.second << "\r\n";
        }

        // Connection header
        req << "Connection: close\r\n";

        // Content-Length for POST
        if (_method == Method::POST && !_body.empty())
        {
            req << "Content-Length: " << _body.size() << "\r\n";
        }

        // End of headers
        req << "\r\n";

        // Body (if POST)
        if (!_body.empty())
        {
            req << _body;
        }

        return req.str();
    }

    // Get just the request line and headers (for debugging)
    std::string to_string() const
    {
        return build();
    }
};
