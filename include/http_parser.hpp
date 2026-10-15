// =============================================================================
// GatePulse — High-Performance L7 API Gateway & Reverse Proxy
// File: include/http_parser.hpp
// Milestone: FR5 — Incremental L7 HTTP/1.1 Parser & Serializer
// =============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <sstream>
#include <algorithm>
#include "middleware.hpp"

namespace gatepulse {

class HttpParser {
public:
    // Parse raw HTTP/1.1 request wire bytes into an HttpRequest struct
    static std::optional<HttpRequest> parse_request(const std::string& raw) {
        if (raw.empty()) return std::nullopt;

        // Find boundary between headers and body: \r\n\r\n or \n\n
        size_t header_end = raw.find("\r\n\r\n");
        size_t delim_len = 4;
        if (header_end == std::string::npos) {
            header_end = raw.find("\n\n");
            delim_len = 2;
            if (header_end == std::string::npos) {
                return std::nullopt; // Incomplete HTTP headers
            }
        }

        std::string header_part = raw.substr(0, header_end);
        std::string body_part   = raw.substr(header_end + delim_len);

        std::istringstream stream(header_part);
        std::string request_line;
        if (!std::getline(stream, request_line)) {
            return std::nullopt;
        }

        // Clean trailing \r
        if (!request_line.empty() && request_line.back() == '\r') {
            request_line.pop_back();
        }

        // Parse Request Line: "METHOD PATH HTTP/1.1"
        std::istringstream line_stream(request_line);
        std::string method, path, version;
        if (!(line_stream >> method >> path >> version)) {
            return std::nullopt; // Malformed request line
        }

        HttpRequest req;
        req.method = method;
        req.path   = path;

        // Parse Header lines: "Key: Value"
        std::string header_line;
        while (std::getline(stream, header_line)) {
            if (!header_line.empty() && header_line.back() == '\r') {
                header_line.pop_back();
            }
            if (header_line.empty()) continue;

            size_t colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = header_line.substr(0, colon_pos);
                std::string val = header_line.substr(colon_pos + 1);

                // Trim leading whitespace
                size_t first = val.find_first_not_of(" \t");
                if (first != std::string::npos) {
                    val = val.substr(first);
                } else {
                    val = "";
                }
                req.set_header(key, val);
            }
        }

        req.body = body_part;
        return req;
    }

    // Parse raw HTTP/1.1 response wire bytes into an HttpResponse struct
    static std::optional<HttpResponse> parse_response(const std::string& raw) {
        if (raw.empty()) return std::nullopt;

        size_t header_end = raw.find("\r\n\r\n");
        size_t delim_len = 4;
        if (header_end == std::string::npos) {
            header_end = raw.find("\n\n");
            delim_len = 2;
            if (header_end == std::string::npos) {
                return std::nullopt;
            }
        }

        std::string header_part = raw.substr(0, header_end);
        std::string body_part   = raw.substr(header_end + delim_len);

        std::istringstream stream(header_part);
        std::string status_line;
        if (!std::getline(stream, status_line)) {
            return std::nullopt;
        }

        if (!status_line.empty() && status_line.back() == '\r') {
            status_line.pop_back();
        }

        // Parse Status Line: "HTTP/1.1 200 OK"
        std::istringstream line_stream(status_line);
        std::string version;
        int status_code = 0;
        if (!(line_stream >> version >> status_code)) {
            return std::nullopt;
        }

        std::string status_message;
        std::getline(line_stream, status_message);
        if (!status_message.empty() && status_message.front() == ' ') {
            status_message = status_message.substr(1);
        }

        HttpResponse res;
        res.set_status(status_code, status_message.empty() ? "OK" : status_message);

        std::string header_line;
        while (std::getline(stream, header_line)) {
            if (!header_line.empty() && header_line.back() == '\r') {
                header_line.pop_back();
            }
            if (header_line.empty()) continue;

            size_t colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = header_line.substr(0, colon_pos);
                std::string val = header_line.substr(colon_pos + 1);
                size_t first = val.find_first_not_of(" \t");
                if (first != std::string::npos) val = val.substr(first);
                else val = "";
                res.set_header(key, val);
            }
        }

        res.body = body_part;
        return res;
    }

    // Serialize HttpRequest into wire format bytes
    static std::string serialize_request(const HttpRequest& req) {
        std::ostringstream ss;
        ss << req.method << " " << req.path << " HTTP/1.1\r\n";
        for (const auto& [k, v] : req.headers) {
            ss << k << ": " << v << "\r\n";
        }
        if (!req.body.empty() && req.get_header("Content-Length").empty()) {
            ss << "Content-Length: " << req.body.length() << "\r\n";
        }
        ss << "\r\n" << req.body;
        return ss.str();
    }

    // Serialize HttpResponse into wire format bytes
    static std::string serialize_response(const HttpResponse& res) {
        std::ostringstream ss;
        ss << "HTTP/1.1 " << res.status_code << " " << res.status_message << "\r\n";
        for (const auto& [k, v] : res.headers) {
            ss << k << ": " << v << "\r\n";
        }
        if (!res.body.empty() && res.get_header("Content-Length").empty()) {
            ss << "Content-Length: " << res.body.length() << "\r\n";
        }
        ss << "\r\n" << res.body;
        return ss.str();
    }
};

}  // namespace gatepulse
