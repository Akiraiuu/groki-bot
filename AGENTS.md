# Paid API testing

The user requires advance, explicit confirmation for each test that can incur API
charges, including text-only requests, voice/audio, generated replies, searches,
and automatic tool follow-ups. Credentials, a prior deployment authorization, or
a request to continue coding are not authorization for a new paid test.

Before a live test, state the provider, purpose, input, maximum rounds, duration,
and possible tool calls, then wait for confirmation. Never start it silently in
the background. Stop capture/upload and close the session on completion, cancel,
timeout or failure. Never auto-reconnect or start paid sessions on boot, OTA,
settings save, or Wi-Fi restoration. Offline tests/builds may proceed.

The three previously authorized ephemeral-02.wav uploads are fully consumed;
further audio uploads or text-only live checks need fresh confirmation.

When live user speech is required, use a dialog with choices. Before connecting,
wait for an explicit choice confirming readiness and this test's paid scope.
After listening is ready, also present the speech/done/cancel instructions in a
dialog. Commentary alone is not a readiness confirmation. A late reply to an
expired test never authorizes a restart or extra round.
