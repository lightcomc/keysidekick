#include "../src/http_security.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

const unsigned short kPort = 8765;
const std::size_t kMaxHeaderBytes = 16 * 1024;
const std::size_t kMaxBodyBytes = 64 * 1024;
const char kTokenHeader[] = "X-KeySidekick-Token";

keysidekick::SecurityPolicy Policy(const std::string& token) {
    keysidekick::SecurityPolicy policy;
    policy.port = kPort;
    policy.allow_ipv6_loopback = true;
    policy.token_header_name = kTokenHeader;
    policy.token = token;
    policy.max_header_bytes = kMaxHeaderBytes;
    policy.max_body_bytes = kMaxBodyBytes;
    return policy;
}

keysidekick::RequestMetadata BaseRequest(const std::string& method,
                                         const std::string& path) {
    keysidekick::RequestMetadata request;
    request.method = method;
    request.path = path;
    request.host = "localhost:8765";
    request.header_bytes = 512;
    request.body_bytes = 0;
    return request;
}

void TestSameOriginGetAllowed() {
    const std::string token = keysidekick::GenerateSecurityToken();
    assert(!token.empty());

    keysidekick::RequestMetadata request = BaseRequest("GET", "/api/status");
    request.origin = "http://localhost:8765";
    request.sec_fetch_site = "same-origin";

    const keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(decision.allowed);
    assert(decision.error == keysidekick::SECURITY_OK);
}

void TestValidMutationAllowed() {
    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata request = BaseRequest("POST", "/api/profile");
    request.origin = "http://localhost:8765";
    request.sec_fetch_site = "same-origin";
    request.content_type = "application/json; charset=utf-8";
    request.token_header_name = kTokenHeader;
    request.token_header = token;
    request.body_bytes = 32;

    const keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(decision.allowed);
}

void TestEvilOriginRejected() {
    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata request = BaseRequest("GET", "/api/status");
    request.origin = "http://evil.example";
    request.sec_fetch_site = "cross-site";

    const keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_ORIGIN_FORBIDDEN);
    assert(std::string(decision.message) == "request origin is not allowed");
}

void TestDirect127CrossOriginAttemptRejected() {
    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata request = BaseRequest("GET", "/api/status");
    request.host = "127.0.0.1:8765";
    request.origin = "http://attacker.example";
    request.sec_fetch_site = "cross-site";

    const keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_ORIGIN_FORBIDDEN);
}

void TestMissingTokenRejected() {
    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata request = BaseRequest("POST", "/api/reload");
    request.origin = "http://localhost:8765";
    request.sec_fetch_site = "same-origin";
    request.content_type = "application/json";

    const keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_TOKEN_REQUIRED);
}

void TestWrongPortRejected() {
    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata request = BaseRequest("GET", "/api/status");
    request.host = "localhost:9999";

    const keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_HOST_FORBIDDEN);
}

void TestOversizedInputRejected() {
    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata headers = BaseRequest("GET", "/api/status");
    headers.header_bytes = kMaxHeaderBytes + 1;
    keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(headers, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_HEADERS_TOO_LARGE);

    keysidekick::RequestMetadata body = BaseRequest("POST", "/api/profile");
    body.origin = "http://localhost:8765";
    body.sec_fetch_site = "same-origin";
    body.content_type = "application/json";
    body.token_header_name = kTokenHeader;
    body.token_header = token;
    body.body_bytes = kMaxBodyBytes + 1;
    decision = keysidekick::ValidateDashboardRequest(body, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_BODY_TOO_LARGE);
}

void TestMutatingGetRejected() {
    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata request =
        BaseRequest("GET", "/api/profile/activate?name=basic");
    request.origin = "http://localhost:8765";
    request.sec_fetch_site = "same-origin";

    const keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_MUTATING_GET_FORBIDDEN);
}

void TestHelpersAndAdditionalPolicyEdges() {
    std::string normalized;
    assert(keysidekick::NormalizeAndValidateHost(" LOCALHOST:8765 ", kPort,
                                                  true, &normalized));
    assert(normalized == "localhost:8765");
    assert(keysidekick::NormalizeAndValidateHost("[::1]:8765", kPort,
                                                  true, &normalized));
    assert(!keysidekick::NormalizeAndValidateHost("localhost", kPort,
                                                   true, &normalized));
    assert(!keysidekick::NormalizeAndValidateHost("localhost.:8765", kPort,
                                                   true, &normalized));
    assert(!keysidekick::NormalizeAndValidateHost("user@localhost:8765", kPort,
                                                   true, &normalized));

    assert(keysidekick::IsMutationMethod("post"));
    assert(keysidekick::IsMutationPath("/api/capture/start"));
    assert(!keysidekick::IsMutationPath("/api/status"));
    assert(keysidekick::ConstantTimeTokenEquals("abcdef", "abcdef"));
    assert(!keysidekick::ConstantTimeTokenEquals("abcdef", "abcdeg"));
    assert(!keysidekick::ConstantTimeTokenEquals("abcdef", "abcdef0"));

    const std::string token = keysidekick::GenerateSecurityToken();
    keysidekick::RequestMetadata request = BaseRequest("POST", "/api/key");
    request.sec_fetch_site = "same-origin";
    request.content_type = "text/plain";
    request.token_header_name = kTokenHeader;
    request.token_header = token;
    keysidekick::SecurityDecision decision =
        keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_CONTENT_TYPE_REQUIRED);

    request.content_type = "application/json";
    request.sec_fetch_site = "cross-site";
    decision = keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(!decision.allowed);
    assert(decision.error == keysidekick::SECURITY_FETCH_SITE_FORBIDDEN);

    request.sec_fetch_site = "none";
    request.origin.clear();
    decision = keysidekick::ValidateDashboardRequest(request, Policy(token));
    assert(decision.allowed);
}

}  // namespace

int main() {
    TestSameOriginGetAllowed();
    TestValidMutationAllowed();
    TestEvilOriginRejected();
    TestDirect127CrossOriginAttemptRejected();
    TestMissingTokenRejected();
    TestWrongPortRejected();
    TestOversizedInputRejected();
    TestMutatingGetRejected();
    TestHelpersAndAdditionalPolicyEdges();
    std::cout << "http_security_tests: all assertions passed\n";
    return 0;
}
