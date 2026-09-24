# J — MCP server robustness

## Outcome

`megascope serve` survives any input a client or a stray process can put on
stdin, stays responsive, keeps bounded memory over a long session, and is
ready to talk before its bake finishes.

## Evidence and starting points

- Framing: `readMessageBody` (`src/mcp/McpProtocol.cpp:120-146`) treats any
  first byte other than `{` as the start of a `Content-Length` header block.
  A UTF-8 BOM, a JSON-RPC batch `[...]`, or a stray log line switches the
  reader into header mode, where every following newline-framed request is
  read as a header line until a blank line that newline clients never send.
  The comment at `:157-159` says both framings stay synchronized; they don't.
- Size: `parseContentLength` uses `std::atoi` (`:84`; undefined on overflow)
  and `body.assign(contentLength, '\0')` (`:141`) allocates up to 2 GB before
  reading, so a hostile length can throw `bad_alloc` and terminate.
- `reindexTU` (`src/mcp/McpServer.cpp:60-78`) removes the TU from all three
  indexes before re-parsing, ignores the parse outcome, and reports success.
  A crash or missing compile command leaves the TU gone. The path is not
  canonicalized (`:204`), so a relative path removes nothing and then adds a
  second TU key.
- `QueryCache` (`include/vycor/query/Tools.h:47-58`) never evicts; dead-code
  results are cached per pagination/filter/entry-point combination
  (`src/query/DeadCodeTools.cpp:108-121`, `:145`).
- `notifications/cancelled` is ignored, and the loop is single-threaded, so
  one long query blocks every other request.
- `serve` bakes before `server.run()` (`src/main.cpp:1596-1601`);
  `docs/mcp-usage.md:156-163` tells clients to wait. MCP clients time out on
  a cold bake.
- Protocol surface: any `protocolVersion` is accepted and echoed
  (`McpServer.cpp:138-143`); `tools/list` has no `annotations` (e.g.
  `readOnlyHint`), and `initialize` returns no `instructions`.

## Work

1. Reproduce first: feed a BOM-prefixed request, a batch array, a log line
   followed by valid requests, and a `Content-Length: 99999999999` header;
   show the hang or abort.
2. Framing: switch to header mode only on a line that begins
   `Content-Length:` (case-insensitive); answer any other unparseable line
   with a JSON-RPC parse error and continue. Strip a leading BOM. Answer
   batches with an explicit "batches unsupported" error (or support them).
   Parse lengths with `StringRef::getAsInteger` and cap the body
   (`--max-message-bytes`, default 64 MB); over the cap, skip the body and
   return an error.
3. `reindex_tu`: canonicalize the path the way `SourceSelection` does; parse
   into scratch indexes first (via I's crash-safe path) and swap in only on
   an `Indexed` outcome; return the outcome, and on failure keep the old data
   and return a typed error through the result contract.
4. Bound `QueryCache` with an LRU on estimated bytes (flag-configurable); key
   dead-code caching on liveness inputs only, not on pagination.
5. Cancellation: honor `notifications/cancelled` with a token checked in the
   path search, impact search, and dead-code loops. Consider running tool
   calls on a worker thread so `ping` and cancellation stay live; if not,
   document the single-request model.
6. Start-up: answer `initialize`, `tools/list`, and `ping` immediately; run
   the bake on a background thread; answer tool calls with
   `status: "unavailable"` and a progress hint until it finishes. Send MCP
   progress notifications during the bake if the client supplied a token.
7. Protocol: negotiate `protocolVersion` against the supported list; add
   `annotations.readOnlyHint: true` to every tool except `reindex_tu`; add an
   `instructions` string describing the search → usr → query workflow.

## Ownership and boundaries

Own `McpProtocol.cpp`, `McpServer.cpp`, and the cache policy. I owns the
crash-safe single-TU parse; K owns payload paging; O owns the client-config
snippets in the docs.

## Acceptance

- The step-1 inputs produce errors and the server keeps answering the next
  valid request; `scripts/mcp-smoke.py` extended with these cases.
- A failed `reindex_tu` leaves the previous answers unchanged
  (before/after tool output compared).
- A scripted long session with varied dead-code pagination stays under the
  cache cap.
- `initialize` answers within 1 s on a cold start of the largest available
  fixture.
- Supported LLVM matrix passes.

## Deliverables

Hardened framing, atomic `reindex_tu`, bounded cache, cancellation, early
`initialize`, protocol annotations, extended smoke tests, and updated
`docs/mcp-usage.md`.
