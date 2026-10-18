// SPDX-License-Identifier: GPL-3.0-or-later
#include "hls-playlist.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace hls {
namespace {

bool starts_with(const std::string& value, const char* prefix)
{
    const size_t length = std::strlen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
}

bool is_http_url(const std::string& value)
{
    auto has_scheme = [&](const char* scheme) {
        const size_t length = std::strlen(scheme);
        if (value.size() < length) return false;
        for (size_t i = 0; i < length; ++i) {
            if (std::tolower(static_cast<unsigned char>(value[i])) != scheme[i]) return false;
        }
        return true;
    };
    return has_scheme("http://") || has_scheme("https://");
}

std::string trim_line(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    const size_t first = line.find_first_not_of(" \t");
    return first == std::string::npos ? std::string() : line.substr(first);
}

std::string hls_attribute(const std::string& attributes, const char* name)
{
    const std::string needle = std::string(name) + "=";
    size_t start = attributes.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    start += needle.size();
    if (start < attributes.size() && attributes[start] == '"') {
        const size_t end = attributes.find('"', start + 1);
        return end == std::string::npos ? std::string()
            : attributes.substr(start + 1, end - start - 1);
    }
    const size_t end = attributes.find(',', start);
    return attributes.substr(start, end == std::string::npos
        ? std::string::npos : end - start);
}

std::string normalize_url_path(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const size_t path_start = url.find('/', scheme + 3);
    if (path_start == std::string::npos) {
        return url;
    }
    const size_t suffix_start = url.find_first_of("?#", path_start);
    const std::string origin = url.substr(0, path_start);
    const std::string path = url.substr(path_start,
        suffix_start == std::string::npos ? std::string::npos : suffix_start - path_start);
    const std::string suffix = suffix_start == std::string::npos
        ? std::string() : url.substr(suffix_start);

    std::vector<std::string> parts;
    size_t cursor = 1;
    while (cursor <= path.size()) {
        const size_t slash = path.find('/', cursor);
        const std::string part = path.substr(cursor,
            slash == std::string::npos ? std::string::npos : slash - cursor);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }

    std::string normalized = origin + "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) normalized += "/";
        normalized += parts[i];
    }
    if (!path.empty() && path.back() == '/' && normalized.back() != '/') {
        normalized += "/";
    }
    return normalized + suffix;
}

} // namespace

std::string resolveUrl(const std::string& base, const std::string& reference)
{
    if (is_http_url(reference)) {
        return reference;
    }
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    if (starts_with(reference, "//")) {
        return base.substr(0, scheme) + ":" + reference;
    }
    const size_t authority_end = base.find('/', scheme + 3);
    const std::string origin = authority_end == std::string::npos
        ? base : base.substr(0, authority_end);
    if (!reference.empty() && reference.front() == '/') {
        return normalize_url_path(origin + reference);
    }

    std::string clean_base = base.substr(0, base.find_first_of("?#"));
    const size_t slash = clean_base.rfind('/');
    if (slash == std::string::npos || slash < scheme + 3) {
        return {};
    }
    return normalize_url_path(clean_base.substr(0, slash + 1) + reference);
}

std::vector<Variant> parseVariants(const std::string& playlist)
{
    std::vector<Variant> variants;
    std::string pending_attributes;
    size_t cursor = 0;
    while (cursor <= playlist.size()) {
        const size_t end = playlist.find('\n', cursor);
        const std::string line = trim_line(playlist.substr(cursor,
            end == std::string::npos ? std::string::npos : end - cursor));
        if (starts_with(line, "#EXT-X-STREAM-INF:")) {
            pending_attributes = line.substr(std::strlen("#EXT-X-STREAM-INF:"));
        } else if (!pending_attributes.empty() && !line.empty() && line.front() != '#') {
            Variant variant;
            variant.uri = line;
            const std::string bandwidth = hls_attribute(pending_attributes, "BANDWIDTH");
            variant.bandwidth = bandwidth.empty() ? 0
                : std::strtoll(bandwidth.c_str(), nullptr, 10);
            const std::string codecs = hls_attribute(pending_attributes, "CODECS");
            variant.supported_video = codecs.empty() ||
                codecs.find("avc1") != std::string::npos ||
                codecs.find("avc3") != std::string::npos ||
                codecs.find("hvc1") != std::string::npos ||
                codecs.find("hev1") != std::string::npos;
            variants.push_back(std::move(variant));
            pending_attributes.clear();
        }
        if (end == std::string::npos) break;
        cursor = end + 1;
    }
    return variants;
}

const Variant* chooseVariant(const std::vector<Variant>& variants)
{
    const Variant* best_under = nullptr;
    const Variant* lowest_over = nullptr;
    for (const Variant& variant : variants) {
        if (!variant.supported_video) continue;
        if (variant.bandwidth > 0 && variant.bandwidth <= kTargetBitrate &&
            (!best_under || variant.bandwidth > best_under->bandwidth)) {
            best_under = &variant;
        } else if (variant.bandwidth > kTargetBitrate &&
                   (!lowest_over || variant.bandwidth < lowest_over->bandwidth)) {
            lowest_over = &variant;
        } else if (variant.bandwidth == 0 && !best_under) {
            best_under = &variant;
        }
    }
    return best_under ? best_under : lowest_over;
}


} // namespace hls
