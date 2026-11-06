# Explicit SSL transport for a bounded HTTPS ticket request

The ticket request configuration explicitly selects `HTTP_TRANSPORT_OVER_SSL`
before `esp_http_client_init`. The endpoint already requires HTTPS and retains
certificate-bundle verification. Authorization, the one-request scope, finite
deadlines, disabled redirects/auth retries, and controlled cleanup are unchanged.

In the pinned SDK overlay, a zero-initialized transport selects the non-SSL
defaults in `_set_config`, including port 80. Parsing an HTTPS URL then changes
the port to 443 and calls `esp_http_client_close`. For a bounded client, close
cancels and retires its scope even when no socket exists. Initialization can
still return a client, while its subsequent perform admission rejects the
cancelled scope with `ESP_ERR_INVALID_STATE`. Selecting SSL initially avoids
this initialization-time port change without weakening scope cancellation.

The regression now includes the exact URL-setting and close methods and the
exact transport-default branch from the frozen SDK input, alongside the existing
HTTP/SSL/TLS admission methods and real scope ledger. It demonstrates both the
default transport's early rejection and explicit SSL reaching a compatible
lower TLS boundary. Existing duplicate-operation, permission-revocation,
deadline and cleanup checks remain. The production ticket test also checks
that its HTTP configuration actually selects SSL.

URL parsing, string utilities, allocation and lower TLS/crypto/socket interfaces
remain explicit test doubles. The added defaults code is a source fragment;
the fixture does not execute the full SDK initializer, HTTP perform/response
parser, real DNS, certificates, or network. A host reproduction establishes this
code path, while complete firmware compilation and hardware acceptance remain
separate validations. No additional request or automatic retry is introduced.
