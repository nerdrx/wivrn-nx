# Measurements

Live captures, parsed into records that can be quoted.

**Why this directory exists.** The operating point of this fork — the paced frame rate, the
Adreno milliseconds per stereo frame, the pose age, the Pass B split, the GPU duty — has been
quoted in ADRs, in `docs/`, and in a paper draft, and every one of those quotes was read by eye
out of a log in `nx-scratch/` that is not in this repository and will not survive the disk it
sits on. A number without a record is an anecdote.

**House rules.** One record per capture, named for what it is and not for the file it came from.
Each record is a `.json` (every field, machine readable) and a `.md` (the same numbers as a
table, with the headline ones called out). Both carry the provenance: the log paths, their sizes,
their mtimes and their SHA-256, and the window used. Records are **generated, never edited** —
`tools/capture-record.py` writes them and nothing else may. Append; do not rewrite. A capture
that failed is still a record, and is kept.

**The parser counts its own misses.** A line that looks like a periodic report and does not
parse is silent data loss — the record would simply hold a smaller sample and would still look
complete. Both sides therefore count near-misses and put the count in the record. Every record
in this directory currently reports **zero**, and the report totals match `grep -c` on the raw
logs exactly. Two report shapes were found this way and would otherwise have been dropped:
`client decode not reported yet` before the first feedback arrives, and the `at fixed QP N`
form that `"rc": "fixed"` prints instead of a target and a controller.

The index table below is regenerated from the `.json` files, so it cannot drift from them.

**Regenerate everything:**

```sh
tools/capture-record.py --name <name> --server <server.log> [--client <logcat>] \
    [--from HH:MM:SS --to HH:MM:SS] [--server-from N --server-to M] \
    --verdict "..." --note "..." --out docs/measurements/<date>
```

**One limitation, stated once.** Client captures are logcat and every line carries a wall clock,
so `--from/--to` select on it exactly. Server logs carry no timestamps at all, so the wall-clock
window cannot be applied to them; the server side is selected by report index instead, and every
record says which of the two it got. Where a server log is an excerpt cut to the window, the
once-per-session header lines (build, GPU, backend, geometry, negotiated tools) are not in it,
and the record says `not carried by this log excerpt` rather than inventing them.

**Wi-Fi band and RSSI** are parsed when present. No capture in this fork carries them today; the
field is in the schema so that a capture which does will record it rather than drop it.

---

## 2026-09-06

| record | what it is | paced fps | B/frame | client GPU/pair | pose age | loop | reports |
|---|---|---|---|---|---|---|---|
| [`cap76-operating-point`](2026-09-06/cap76-operating-point.md) | The shipped operating point: 16.69 ms of Adreno per stereo frame, 92 ms of pose age, and a GPU already at 92 %. | 38.46 | 17148 | 16.69 ms | 91.9 ms | 43.64/s | 45 |
| [`dec-off-coupled-display`](2026-09-06/dec-off-coupled-display.md) | The B leg of the flat A/B; see dec-on-decoupled-display for what that means. | 38.24 | 17150 | 16.73 ms | 92.7 ms | 43.45/s | 30 |
| [`dec-on-decoupled-display`](2026-09-06/dec-on-decoupled-display.md) | An A/B that came out flat: against dec-off-coupled-display nothing moves by more than the run-to-run spread. | 38.03 | 17151 | 16.74 ms | 93.1 ms | 43.23/s | 30 |
| [`server74-encode-only`](2026-09-06/server74-encode-only.md) | Server side only: one session, 57 encode reports, no client capture was taken. | 39.88 | 17520 | — | — | — | 57 |
| [`server75-encode-only`](2026-09-06/server75-encode-only.md) | Server side only: two client connections inside one server lifetime, 113 encode reports. | 38.72 | 17411 | — | — | — | 113 |
| [`server76-long-session`](2026-09-06/server76-long-session.md) | The long one: 3406 encode reports over roughly two hours of a single connection. | 43.56 | 30118 | — | — | — | 3406 |
| [`server86-client-died`](2026-09-06/server86-client-died.md) | NEGATIVE RECORD: the client connected, exactly one encode report went out, and the network thread saw a socket shutdown. | 33.00 | 32055 | — | — | — | 1 |

### What each one is for

* **`cap76-operating-point`** is the canonical one. Every operating-point figure quoted
  elsewhere in this tree — `16.69 ms` of Adreno per stereo frame, `passA 2.97 + passB 13.72`,
  `skip 8.25` (60 % of Pass B), `91.9 ms` of pose age, the `6.37 ms` display pass, `38.5 fps`
  paced — is in this record, and this is the file to cite instead of a log path.
* **`dec-on-decoupled-display` / `dec-off-coupled-display`** are an A/B that came out **flat**.
  They are kept because a flat A/B is a result, and because neither capture carries a line
  naming which hand-over the session took, which is a gap in the client's logging that the
  record makes visible.
* **`server74-encode-only` / `server75-encode-only`** are server-side only: no logcat was taken,
  so every client field is absent by construction. `server75` spans two client connections in
  one server lifetime.
* **`server76-long-session`** is the long horizon — 3406 reports — and is the one to read for
  controller and not-reconstructed behaviour over hours. It is **not** the operating-point
  record: its aggregate spans far more than the 90 seconds the quoted numbers come from.
* **`server86-client-died`** is a negative record: one encode report, then
  `Exception in network thread: Socket shutdown`. Kept deliberately. A record set that holds
  only sessions that worked cannot be used to tell whether a change broke anything.

### What is not in these records

The **client build hash**. The server writes its own version on its first line, but the client
logs no build identifier at all, so a record cannot say which APK produced it. Attributing a
capture to a commit is therefore done by hand, outside the record, and is not evidence. Making
the client log its `GIT_DESC` on start-up would close that gap and would make every record here
self-identifying.
