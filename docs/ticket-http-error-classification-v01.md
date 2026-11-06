# Finite ticket HTTP failure categories

Ticket HTTP failures retain the existing `connection` JSON shape. New fixed
`code` values distinguish HTTP connect, write, headers, read timeout, closed,
incomplete data, unavailable transport, pending, invalid argument, unknown HTTP
failure, DNS, TLS verification, TLS library, and selected scope ledger failures.
No native error number, verification flags, address, credential, ticket, error
text, transcript or audio is stored in these diagnostics.

Classification occurs after the single synchronous `esp_http_client_perform`
returns and before cancellation/cleanup alters the error cache. An observed
scope clock/invariant/capacity fault or exceeded scope deadline has priority.
Otherwise the HTTP return code is mapped. Only `ESP_ERR_HTTP_CONNECT`, whose
production path has selected a transport, reads and clears the SDK TLS error
cache once. DNS is identified from its specific SDK error; nonzero verification
flags map to TLS verification, and a nonzero native TLS code maps to the broader
TLS library category. Unlisted conditions remain generic. These categories are
observations, not a complete causal diagnosis or evidence of server receipt.

The first recorded failure still wins. A header setup failure, body size limit,
HTTP status failure, or request failure cannot be replaced by a later cleanup
failure. The error cache is not read on success, on early HTTP errors before a
transport is assigned, or after cleanup. No request, retry, reconnect, permission,
deadline or close policy is added or relaxed.

Host tests link the production ticket code and execute the coordinator connect
body and status handler with explicit admission/network/RTOS doubles. They test
the finite HTTP and TLS categories, bounded JSON output, first failure retention,
null-client handling, and the unchanged success path. A separate test joins nine
exact HTTP connect, SSL connect and TLS admission method bodies plus the real
scope ledger. Its parser, allocation, lower TLS/crypto/socket and time sources
remain controlled substitutes. A compatible synthetic path reaches connected;
injected failures prove rejection and resource retirement behavior.

The joined test does not execute full HTTP perform/response parsing, real DNS,
TCP, certificates, encryption, ESP task scheduling or platform time sources.
It does not reproduce a device failure or establish its cause. Host symbolic TLS
error values are test doubles, while firmware compilation uses the pinned SDK
headers. Full firmware compilation and device acceptance remain independent.

Runtime tools must allow the expanded code whitelist before interpreting this
candidate on hardware. Existing JSON field names, `fault_detail` semantics, call
prefix flags and the generation-bound first-failure policy remain unchanged.
