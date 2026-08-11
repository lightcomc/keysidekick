#ifndef KEYSIDEKICK_HTTP_SECURITY_H
#define KEYSIDEKICK_HTTP_SECURITY_H

#include <cstddef>
#include <string>

namespace keysidekick {

const std::size_t kDefaultMaxHeaderBytes = 16 * 1024;
const std::size_t kDefaultMaxBodyBytes = 64 * 1024;

struct RequestMetadata {
    std::string method;
    std::string path;
    std::string host;
    std::string origin;
    std::string sec_fetch_site;
    std::string content_type;
    std::string token_header_name;
    std::string token_header;
    std::size_t header_bytes;
    std::size_t body_bytes;

    RequestMetadata();
};

struct SecurityPolicy {
    unsigned short port;
    bool allow_ipv6_loopback;
    std::string token_header_name;
    std::string token;
    std::size_t max_header_bytes;
    std::size_t max_body_bytes;

    SecurityPolicy();
};

enum SecurityError {
    SECURITY_OK = 0,
    SECURITY_BAD_POLICY,
    SECURITY_HEADERS_TOO_LARGE,
    SECURITY_BODY_TOO_LARGE,
    SECURITY_HOST_FORBIDDEN,
    SECURITY_ORIGIN_FORBIDDEN,
    SECURITY_FETCH_SITE_FORBIDDEN,
    SECURITY_MUTATING_GET_FORBIDDEN,
    SECURITY_TOKEN_REQUIRED,
    SECURITY_TOKEN_INVALID,
    SECURITY_CONTENT_TYPE_REQUIRED
};

struct SecurityDecision {
    bool allowed;
    SecurityError error;
    int http_status;
    const char* code;
    const char* message;
};

bool NormalizeAndValidateHost(const std::string& host,
                              unsigned short configured_port,
                              bool allow_ipv6_loopback,
                              std::string* normalized_host);

bool IsAllowedOrigin(const std::string& origin,
                     unsigned short configured_port,
                     bool allow_ipv6_loopback);

bool IsSecFetchSiteAllowed(const std::string& sec_fetch_site);

bool IsMutationMethod(const std::string& method);

bool IsMutationPath(const std::string& path);

bool IsJsonContentType(const std::string& content_type);

bool ConstantTimeTokenEquals(const std::string& provided,
                             const std::string& expected);

SecurityDecision ValidateDashboardRequest(const RequestMetadata& request,
                                          const SecurityPolicy& policy);

std::string GenerateSecurityToken();

}  // namespace keysidekick

#endif
