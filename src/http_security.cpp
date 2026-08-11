#include "http_security.h"

#ifndef _WIN32
#error http_security token generation requires Windows
#endif

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <vector>

namespace keysidekick {
namespace {

const std::size_t kTokenBytes = 32;

std::string TrimAsciiWhitespace(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() &&
           (value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first &&
           (value[last - 1] == ' ' || value[last - 1] == '\t')) {
        --last;
    }
    return value.substr(first, last - first);
}

std::string ToLowerAscii(const std::string& value) {
    std::string result(value);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(result[index]);
        result[index] = static_cast<char>(std::tolower(character));
    }
    return result;
}

std::string ToUpperAscii(const std::string& value) {
    std::string result(value);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(result[index]);
        result[index] = static_cast<char>(std::toupper(character));
    }
    return result;
}

std::string PortText(unsigned short port) {
    char buffer[6];
    std::snprintf(buffer, sizeof(buffer), "%u",
                  static_cast<unsigned int>(port));
    return std::string(buffer);
}

std::string StripQueryAndFragment(const std::string& path) {
    const std::size_t delimiter = path.find_first_of("?#");
    return path.substr(0, delimiter);
}

bool HeaderNameEquals(const std::string& left, const std::string& right) {
    return ToLowerAscii(TrimAsciiWhitespace(left)) ==
           ToLowerAscii(TrimAsciiWhitespace(right));
}

SecurityDecision Allow() {
    SecurityDecision decision = {
        true, SECURITY_OK, 200, "ok", "request allowed"
    };
    return decision;
}

SecurityDecision Reject(SecurityError error, int http_status,
                        const char* code, const char* message) {
    SecurityDecision decision = {
        false, error, http_status, code, message
    };
    return decision;
}

bool FillRandomWithBCrypt(unsigned char* bytes, std::size_t length) {
    if (length > static_cast<std::size_t>(ULONG_MAX)) {
        return false;
    }
    const NTSTATUS status = BCryptGenRandom(
        NULL, bytes, static_cast<ULONG>(length),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
}

bool FillRandomWithCryptoApi(unsigned char* bytes, std::size_t length) {
    if (length > static_cast<std::size_t>(
                     (std::numeric_limits<DWORD>::max)())) {
        return false;
    }

    HCRYPTPROV provider = 0;
    if (!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return false;
    }
    const BOOL generated = CryptGenRandom(
        provider, static_cast<DWORD>(length), bytes);
    CryptReleaseContext(provider, 0);
    return generated == TRUE;
}

std::string HexEncode(const unsigned char* bytes, std::size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (std::size_t index = 0; index < length; ++index) {
        result[index * 2] = digits[(bytes[index] >> 4) & 0x0f];
        result[index * 2 + 1] = digits[bytes[index] & 0x0f];
    }
    return result;
}

}  // namespace

RequestMetadata::RequestMetadata()
    : header_bytes(0), body_bytes(0) {}

SecurityPolicy::SecurityPolicy()
    : port(0),
      allow_ipv6_loopback(false),
      token_header_name("X-KeySidekick-Token"),
      max_header_bytes(kDefaultMaxHeaderBytes),
      max_body_bytes(kDefaultMaxBodyBytes) {}

bool NormalizeAndValidateHost(const std::string& host,
                              unsigned short configured_port,
                              bool allow_ipv6_loopback,
                              std::string* normalized_host) {
    if (normalized_host != NULL) {
        normalized_host->clear();
    }
    if (configured_port == 0) {
        return false;
    }

    const std::string normalized = ToLowerAscii(TrimAsciiWhitespace(host));
    const std::string port = PortText(configured_port);
    const bool valid_ipv4 = normalized == "127.0.0.1:" + port;
    const bool valid_localhost = normalized == "localhost:" + port;
    const bool valid_ipv6 = allow_ipv6_loopback &&
                            normalized == "[::1]:" + port;
    if (!valid_ipv4 && !valid_localhost && !valid_ipv6) {
        return false;
    }

    if (normalized_host != NULL) {
        *normalized_host = normalized;
    }
    return true;
}

bool IsAllowedOrigin(const std::string& origin,
                     unsigned short configured_port,
                     bool allow_ipv6_loopback) {
    if (origin.empty()) {
        return true;
    }
    if (configured_port == 0) {
        return false;
    }

    const std::string port = PortText(configured_port);
    if (origin == "http://127.0.0.1:" + port ||
        origin == "http://localhost:" + port) {
        return true;
    }
    return allow_ipv6_loopback && origin == "http://[::1]:" + port;
}

bool IsSecFetchSiteAllowed(const std::string& sec_fetch_site) {
    const std::string normalized =
        ToLowerAscii(TrimAsciiWhitespace(sec_fetch_site));
    return normalized.empty() || normalized == "same-origin" ||
           normalized == "none";
}

bool IsMutationMethod(const std::string& method) {
    const std::string normalized = ToUpperAscii(TrimAsciiWhitespace(method));
    return normalized == "POST" || normalized == "PUT" ||
           normalized == "PATCH" || normalized == "DELETE";
}

bool IsMutationPath(const std::string& path) {
    const std::string normalized = StripQueryAndFragment(path);
    return normalized == "/api/profile/activate" ||
           normalized == "/api/profile" ||
           normalized == "/api/key" ||
           normalized == "/api/key/delete" ||
           normalized == "/api/reload" ||
           normalized == "/api/capture/start";
}

bool IsJsonContentType(const std::string& content_type) {
    const std::string normalized =
        ToLowerAscii(TrimAsciiWhitespace(content_type));
    const std::size_t semicolon = normalized.find(';');
    const std::string media_type = TrimAsciiWhitespace(
        normalized.substr(0, semicolon));
    return media_type == "application/json";
}

bool ConstantTimeTokenEquals(const std::string& provided,
                             const std::string& expected) {
    const std::size_t maximum = std::max(provided.size(), expected.size());
    unsigned int difference =
        static_cast<unsigned int>(provided.size() ^ expected.size());
    for (std::size_t index = 0; index < maximum; ++index) {
        const unsigned char left = index < provided.size()
            ? static_cast<unsigned char>(provided[index]) : 0;
        const unsigned char right = index < expected.size()
            ? static_cast<unsigned char>(expected[index]) : 0;
        difference |= static_cast<unsigned int>(left ^ right);
    }
    return difference == 0;
}

SecurityDecision ValidateDashboardRequest(const RequestMetadata& request,
                                          const SecurityPolicy& policy) {
    if (policy.port == 0 || policy.token_header_name.empty() ||
        policy.token.empty() || policy.max_header_bytes == 0 ||
        policy.max_body_bytes == 0) {
        return Reject(SECURITY_BAD_POLICY, 500, "security_configuration_error",
                      "security policy is not configured");
    }
    if (request.header_bytes > policy.max_header_bytes) {
        return Reject(SECURITY_HEADERS_TOO_LARGE, 431, "headers_too_large",
                      "request headers are too large");
    }
    if (request.body_bytes > policy.max_body_bytes) {
        return Reject(SECURITY_BODY_TOO_LARGE, 413, "body_too_large",
                      "request body is too large");
    }
    if (!NormalizeAndValidateHost(request.host, policy.port,
                                  policy.allow_ipv6_loopback, NULL)) {
        return Reject(SECURITY_HOST_FORBIDDEN, 403, "host_forbidden",
                      "request host is not allowed");
    }
    if (!IsAllowedOrigin(request.origin, policy.port,
                         policy.allow_ipv6_loopback)) {
        return Reject(SECURITY_ORIGIN_FORBIDDEN, 403, "origin_forbidden",
                      "request origin is not allowed");
    }
    if (!IsSecFetchSiteAllowed(request.sec_fetch_site)) {
        return Reject(SECURITY_FETCH_SITE_FORBIDDEN, 403,
                      "fetch_site_forbidden",
                      "cross-site requests are not allowed");
    }

    const std::string method = ToUpperAscii(TrimAsciiWhitespace(request.method));
    const bool mutation_path = IsMutationPath(request.path);
    if (method == "GET" && mutation_path) {
        return Reject(SECURITY_MUTATING_GET_FORBIDDEN, 405,
                      "mutating_get_forbidden",
                      "state changes require a mutation method");
    }

    const bool mutation = IsMutationMethod(method) || mutation_path;
    if (!mutation) {
        return Allow();
    }
    if (request.token_header.empty()) {
        return Reject(SECURITY_TOKEN_REQUIRED, 403, "token_required",
                      "request token is required");
    }
    if (!HeaderNameEquals(request.token_header_name,
                          policy.token_header_name) ||
        !ConstantTimeTokenEquals(request.token_header, policy.token)) {
        return Reject(SECURITY_TOKEN_INVALID, 403, "token_invalid",
                      "request token is invalid");
    }
    if (!IsJsonContentType(request.content_type)) {
        return Reject(SECURITY_CONTENT_TYPE_REQUIRED, 415,
                      "json_content_type_required",
                      "application/json content type is required");
    }
    return Allow();
}

std::string GenerateSecurityToken() {
    std::vector<unsigned char> bytes(kTokenBytes, 0);
    bool generated = FillRandomWithBCrypt(&bytes[0], bytes.size());
    if (!generated) {
        generated = FillRandomWithCryptoApi(&bytes[0], bytes.size());
    }
    if (!generated) {
        SecureZeroMemory(&bytes[0], bytes.size());
        return std::string();
    }

    const std::string token = HexEncode(&bytes[0], bytes.size());
    SecureZeroMemory(&bytes[0], bytes.size());
    return token;
}

}  // namespace keysidekick
