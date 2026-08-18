# dsh Session Link

Client-only dsh plugin for selecting a session from an `openin` deep link:

```text
http://127.0.0.1:3080/?openinSession=<session-id>
```

The plugin waits for the session baseline, calls `ctx.sessions.open()`, then
removes the query parameter with `history.replaceState()`.
