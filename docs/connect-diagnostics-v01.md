# Finite connection diagnostics

Connection setup can fail before ticket HTTP, during ticket processing, or while
starting the realtime client. The existing `fault_source` and `fault_detail`
fields keep their current meaning. The additive `connection` object reports a
fixed `stage`, fixed `code`, `ticket_http_called`, and `websocket_start_called`.
It contains no raw errors, URLs, tokens, headers, identifiers, text or audio.

The two call flags mean the application entered the corresponding API call.
They do not prove that packets were sent, a handshake succeeded, or usage was
billed. HTTP response statuses map to fixed categories; raw statuses are not
retained in the diagnostic. A stage of `started` means that synchronous client
startup returned successfully, not that the server accepted the session or that
audio input is ready.

`PaidVoiceTest` publishes the diagnostic under its existing mutex. Updates require
the matching granted generation and a running paid scope. The first nonempty
failure code and its call prefix are retained. STOP, finish, stale callbacks and
cleanup cannot replace it. An accepted new explicit grant clears it, alongside
the existing fault fields; reboot starts with empty RAM state. Diagnostics do not
grant permission, extend the deadline, retry requests, reconnect, or change any
shutdown fact.

Ticket request failures are observed before cleanup, so a later cleanup failure
does not hide the original cause. Cleanup still follows the existing ownership
and bounded scope policy. A retained uncertain owner is not reported as cleanly
released by these diagnostics.

Host regression compiles the actual coordinator `connect()` method and actual
voice status handler against explicit operation/SDK/network doubles, links the
production ticket and realtime client implementations, and covers pre-request
rejection, HTTP categories, ticket validation, client startup failures, successful
startup, uncertain cleanup, first cause retention, old generation rejection,
concurrent snapshot access and complete JSON serialization. Existing close,
operation gate, DNS, HTTP and SDK tests remain in the host suite.

Host simulation is not a firmware build or a device acceptance result. Allocation
and TLS platform behavior, system clock initialization, real network conditions,
server acceptance and audio quality still require their appropriate independent
validation. In particular, the clock precondition and some allocation/TLS
initialization branches are instrumented but not forced by the new host fixture.
These fields locate a failed branch; they do not repair its underlying cause.
