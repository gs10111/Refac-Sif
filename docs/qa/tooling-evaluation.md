# Tooling evaluation — mutation testing on the backend, emulation for the firmware

Question asked: not "is it good", but **what does it catch that we do not catch
today, and what does it cost**.

| Tool | Verdict | Cost | First step |
|---|---|---|---|
| **mutmut** (backend mutation testing) | **ADOPT NOW**, pinned to 2.5.1, `string,fstring` disabled | minutes per run — but the real cost is **two readers for the first triage**, one per run after | build the content-keyed survivor baseline |
| **QEMU (Espressif fork) + pytest-embedded** | **REJECT** | an ESP-IDF install plus a port off the Arduino framework, to answer 1 of 3 questions | none |
| **Wokwi + wokwi-cli** | **REJECT for the three questions asked. Revisit only for the trigger path in CI** | metered cloud service, alpha scenario API, firmware binary leaves the building | none until H6/H7 are done on a bench |

The finding that drives both firmware verdicts: **all three questions lave wants
answered are answerable on one pico32 on a bench in under an hour, at higher
fidelity than any emulator gives.** Emulation is being asked to substitute for
hardware access we already have.

---

## Tool 1 — mutmut

### What was run

Everything below is measured on this branch, not read about.

```
# baseline
cd backend && ./.venv/bin/python -m pytest tests/ -q     # 111 passed in 0.75s (1.557s wall)

# mutmut 3.6.0 — FAILED, see below
# mutmut 2.5.1 — works:
./.venv/bin/mutmut run \
  --paths-to-mutate "app_state.py,config,protocol,server,web" \
  --runner "./.venv/bin/python -m pytest -x -q tests/" \
  --disable-mutation-types=string
```

### Two ways this run lies to you

Both were caught before they produced a wrong answer. Both belong in the invocation
notes, because both produce output that looks entirely plausible.

**1. A red baseline turns every mutant into a kill.** The runner is `pytest -x`, which
stops at the first failure — correct and fast for mutation testing, because one failing
test is all it takes to call a mutant dead. But if the suite is *already* red, every
mutant run dies on the pre-existing failure and **every mutant is scored killed**. The
report comes back showing survivors collapsing to near zero, which reads as spectacular
progress and is in fact the baseline being broken. sama caught this on my command while
four Q3 tests were deliberately red mid-TDD.

**mutmut 2.5.1 refuses on a red baseline — verified, not assumed.** lave ran it against
the red tree before the warning arrived: it checked the baseline, reported
`1 failed, 109 passed`, and bailed. No mutants were scored, no report was produced,
nothing was written. So the collapse-to-near-zero scenario cannot happen with this tool
as invoked. We know that because it happened, not because the docs say so.

Keep the guard below anyway. Not out of distrust of a property we have now watched
fire, but because it is one line, it makes the refusal legible to whoever reads the log
six months from now, and it survives a changed runner or a version bump — the guard
holds even when the assumption it protects stops being true:

```
cd backend && ./.venv/bin/python -m pytest -q tests/ 2>&1 | tail -3
# ONLY if that reports N passed, 0 failed:
rm -f .mutmut-cache && ./.venv/bin/mutmut run ...
```

**2. Mutant IDs are not stable.** mutmut numbers mutants in discovery order over the
source, so any edit above a mutant renumbers everything after it. "Mutant #94 is now
killed" is a claim about a *label*, and the label moves. **Verify survivors by content,
never by id** — `mutmut show all` prints the diff for every surviving mutant, so the
question "is the `saved = False → True` mutation still alive" is answered by searching
the diffs, not by looking up a number.

The same reasoning as the firmware harness's rule about naming which tests must die
rather than predicting a count: an identifier that can silently come to mean something
else is not a self-check.

### Version: 2.5.1, pinned, and this is a real cost

`pip install mutmut` resolves **3.6.0, which does not work for this layout**. 3.x
requires `source_paths` in `setup.cfg`/`pyproject.toml` (neither existed), then
copies the tree into `backend/mutants/` and runs pytest from there — which breaks
every import, because the suite reaches `app_state` and `protocol.packet` through
pytest's rootdir. Observed result: `0 files mutated`, then `ModuleNotFoundError`.

2.5.1 takes `--paths-to-mutate` on the command line, needs no config file inside
sama's area, and ran first time.

**Price this honestly:** 2.5.1 is an old release of a tool whose current major
version we have established does not fit. We are adopting an unmaintained pin. That
is acceptable for a nightly quality job — if it ever breaks we lose a report, not a
build — and it would not be acceptable for anything in the critical path.

### Results

With string mutants disabled:

Two runs, three days of code apart in effect:

| | Baseline run | After the items 1–10 round (`bc4717a`) |
|---|---|---|
| Mutants generated | 187 | 188 |
| Killed | 115 | **132** |
| Timeout | 3 | 2 |
| Survived | 69 | **54** |
| Mutation score | 61.5% | **70.2%** (73.4% counting timeouts) |

**Read the survivor delta before the score. `54` is not the number that matters —
`+0 new survivors` is.**

A round that kills sixteen mutants and quietly introduces three still reports a drop of
thirteen and reads as progress. The totals cannot distinguish that from a round that
introduced nothing, and the score cannot either: both land on the same percentage. Only
a content diff of the two survivor sets separates them.

We ran that diff: **16 killed, 0 appeared.** The score moving 61.5% → 70.2% is the
decoration; the zero is the result.

The same asymmetry applies to every future run, which is why the survivor baseline
below is the recommended artefact and the score is not.

Runtime: a few minutes with a warm cache — and **runtime was never the constraint.**
See [what adoption actually costs](#what-adoption-actually-costs).

### `--disable-mutation-types=string` is not optional

The first run, with string mutants on, produced 92 survivors. Sampling them showed
the population:

```
#102  logging.info(f"Saved {filename}")  ->  f"XXSaved {filename}XX"   noise
#141  addr[0] -> addr[1]  inside a warning message                     noise
#152  same shape                                                       noise
```

Log-string mutants are unkillable by design — we do not assert on log text and we
should not start. Left on, they are 25% of the survivor list and they train the
reader to skim it, which is how the real ones get missed. Disabled, the survivor
count drops 92 → 69 and every remaining row is at least arguable.

### What it caught that we do not catch today

**Survivor #94 — `saved = False` → `saved = True` in `save_data`, survived.**

This is the find, and it is worth the whole exercise. It meant **no test covered "the
local CSV write failed, so do not attempt the Drive copy"** — the `if saved` guard at
`server/tcp_server.py:65,71,80`, from the B6 fix, shipped that day with its failure
path untested. Every other B6 test took the write-succeeded path.

**Closed within the hour.** sama added
`test_save_data_does_not_copy_to_drive_when_the_local_write_fails`
(`tests/test_tcp_server.py:566`), which raises `OSError` from a module-scoped `open`
shadow and asserts `subprocess.run` is never called. Its docstring names the gap
itself: *"This test is the only thing covering that branch — every other B6 test takes
the write-succeeded path."*

That is the whole value cycle in one afternoon: **a tool found a hole in code shipped
the same day, a human wrote the test, and the next run reported the mutant killed.**

**The acceptance test passed.** After the round closing items 1–10 (`bc4717a`):

```
188 mutants   killed 132   timeout 2   survived 54     (was 116 / 2 / 70)
```

All eleven named predictions verified dead **by content, not by id**. Sixteen mutants
died against twelve named ones (`BATTERY_INVALID` was one item but two mutants), so
**four died as side effects**, identified by diffing the two survivor sets rather than
guessed:

| Extra kill | Killed by |
|---|---|
| `BATTERY_INVALID = None` | the item-9 literal test |
| `CLIENT_TIMEOUT_SEC = None` | the item-9 literal test |
| `GDRIVE_PATH = None` | the item-5/6 test asserting the copy destination |
| `reason = None` (whole ternary) | the item-8 `/ota` message test |

All four are siblings of a named target — the good kind of extra. **Zero new survivors
appeared**, which is the check that matters more than the drop: a round that kills
sixteen and quietly introduces three is not progress, and only a content diff of the
two survivor sets can tell you which happened.

That diff is the baseline workflow proposed [below](#where-i-disagree-this-argues-for-running-it-more-often-not-less),
run once by hand. It took two minutes and it answered a question the totals could not.
A mutation tool whose findings do not verifiably close is a report generator; this
one's do.

**Timeout #89 — `break` → `continue` on the `None` sentinel in `save_data`.**

Careful with this one: it **timed out, which is a detection, not a miss.** The mutant
made the writer thread spin forever and the suite hung; mutmut reports timeouts
separately for exactly that reason. The narrower claim that survives is still worth
acting on: *nothing asserts that the sentinel terminates the writer thread*, so the
property is caught only by wall clock — which in CI reads as flakiness rather than as
a failure, and gets retried rather than investigated.

### The 70 survivors, triaged

Run on the green baseline (117 passing): **188 mutants, 116 killed, 2 timeout, 70
survived.** Every survivor's diff was read. The population is not what the first
sample suggested:

| Class | Count | Verdict |
|---|---|---|
| **f-string text** — `f"Saved {x}"` → `f"XXSaved {x}XX"` | **22 (31%)** | noise, and **removable** — see below |
| `addr[0]` → `addr[1]` **inside a log message** | 7 | noise |
| Untested entrypoints — `server_main`, `exit_monitor`, `__main__` | 15 | real code, no tests; binds sockets and reads stdin |
| Operational constants in `settings.py` | 8 | low value; nothing asserts them |
| **Equivalent mutants** | **2 (3%)** | cannot be killed, must not be chased |
| **Real behavioural holes** | **~10** | listed below, worth tests |

Triaged independently by sama and by me, and **the independence is the finding.** I
filed mutant 93 as message noise and sama caught it as a weak assertion; sama filed
mutant 127 as message noise and I caught it as a silent CSV rename. **Each of us was
blind in exactly the direction the other was not, and neither list was right alone.**

Both misses have the same cause: categorising by the shape that keeps recurring rather
than by what the line does. Seven `addr[0] → addr[1]` mutants in a row train you to
sweep the eighth. Twenty-two `XX...XX` mutants train you to sweep an operator mutation
that happens to sit inside an f-string.

It recurred a third time while counting: a `grep '^+.*addr\[1\]'` returns **nine**
hits, but one is an `XX`-form mutant whose *unmutated* text merely contains `addr[1]`.
The real count is eight — seven message-text swaps and mutant 127.

And a fourth time, in this document, by me. I classified `running = True → None` as
equivalent **by analogy to `saved = False → None`** — same shape, both falsy, filed
together. It is not equivalent. `running` is read at exactly one place,
`tcp_server.py:237`, the `while running` of `server_main`'s accept loop; under the
mutant the server binds, listens, exits the loop immediately, closes the socket and
posts the shutdown sentinel. A total behavioural change that survives only because
nothing in the suite runs `server_main`. It is **unreachable glue, not equivalent** —
caught by sama.

The discriminator is falsifiable rather than a matter of taste, and it is worth
keeping: *an equivalent mutant cannot be killed by any test that could ever be
written; an unreachable one is waiting for a test that does not exist yet.*

A fifth followed immediately, and it moved up a level. sama predicted the loopback
integration test would kill `running = None` — by analogy again, but this time an
analogy about **test strategy** rather than code shape: *integration tests cover glue,
this is glue*. The proposed design calls `handle_client` directly and never runs
`server_main`, so the mutant survives it untouched. The claim and the design were in
the same message, written by the same person.

A sixth, in the permanent backend documentation, and it is the sharpest of the set:
the caveat above was first written as *"mutmut flips operators, swaps numbers and nulls
names, and none of those produce a wrong byte count"* — reasoned from the **tool's list
of mutation types** rather than from the function body, which contains an operator on a
byte count (`remaining -= len(chunk)`). Categorising by the shape of the rules instead
of by what the code holds, while writing the caveat about categorising by shape, into
the document that outlives the conversation.

A seventh, mine, in this document: I explained the classification drift below as *"the
suite got slower"* — generalising from a number that had moved and a test that had just
landed, instead of asking **which mutant, and why**. The suite had not got slower.

**Seven instances of one mistake in one afternoon** — the third while measuring the
first two, the fourth while writing the rule down, the fifth reasoning about the fix for
the fourth, the sixth inside the correction itself, the seventh in the paragraph
correcting the sixth. **Every one was caught by the other reader, never by the author.**
Seven for seven, across all three of us.

The framing worth keeping is sama's: this is **not carelessness and not inexperience**,
because all seven happened to people who were holding the rule and writing it down at
the time. It is that **the artefact in front of you is always more available than the
question behind it** — a grep result, a mutation-type table, a number that moved. The
artefact answers immediately; the question requires going and looking. Under any time
pressure at all, the artefact wins.

That is also why the countermeasure is a second reader rather than more care. Care is
what all seven of these already had.

#### The eighth is a different failure, and the second reader does not fix it

Tracing why a test failed to detect the mutation it was written for, I computed
`((0*18+1) % 72) / 18 = 0` — the mutation's head index never advances, so it did not do
what its name and comment claimed. **I saw that and dropped it**, filed as incidental
arithmetic, because it did not serve the conclusion I was already building about the
test. The report I sent said the mutation reached the ring and never mentioned that it
was not the mutation it advertised.

That is not the artefact being more available than the question. The answer *was* in
hand. It is **relevance-filtering against a hypothesis in flight** — a fact gets
discarded for not fitting the argument being assembled.

**The countermeasure is different, and this is the important part.** A second reader
fixes availability, because the second reader has not yet built the conclusion. It does
*not* reliably fix this one: the second reader is usually reading the first reader's
framing, and the discarded fact was already excluded there. Here it was caught only
because the other reader was tracing the same code **without a conclusion to protect** —
and had he not, the round would have shipped with a mutation whose name lied and five
deaths that read as coverage.

The rule that does address it:

> **When you compute something that does not fit the argument you are making, report it
> anyway and say it does not fit.**

Cheap, and it is the only defence that works from inside the head that is doing the
filtering.

#### A third class: measuring a moving target

Late in the same session, three consecutive runs of one unchanged command reported
**73, 62 and 61** test cases — the suite was being rewritten under the compiler. One of
those runs said `1 failed`, which is exactly the shape of a finding.

It happened to me in the same window. A `grep` for a mutation's build env returned
nothing while its `#ifdef` was already in the source, and I was one message from
reporting *"this mutation has no env"* — the same real gap found hours earlier, and
this time an artefact of an editor mid-save. Minutes later the count was consistent.

So there are **three distinct classes, with three distinct countermeasures**, and
conflating them is why the first two kept recurring:

| Class | Failure | Countermeasure |
|---|---|---|
| Availability | The artefact in front of you answers; the question behind it requires going and looking | A second reader, split by angle |
| Relevance-filtering | You compute something that does not fit the argument in flight, and drop it | Report it anyway and say it does not fit — the second reader inherits your framing and cannot catch this |
| **Moving target** | The thing measured changes between the measurement and the claim | **Sequencing.** Neither care nor readers help: both readers measure the same moving tree |

The third is the one nobody defends against, because every measurement *feels*
instantaneous. It caught all three readers within ten minutes, including both who had
just named it.

It has **two** remedies, and the second is the useful one:

**Sequencing**, for whoever measures — a stated quiet window: the tree is untouched,
then the run happens, then the claim. Anything measured outside it is *inadmissible*,
not weak evidence.

**The conditional form**, for whoever reports and cannot wait. State the implication
and the antecedent separately:

> *If* `uploader.cpp` still assigns `outcome.fullyWritten = ok && write_all(...)`,
> *then* with `ok` false the battery is sampled and not written, because C++
> short-circuits `&&` — and *if* the comment above still claims it is written, the
> comment describes behaviour the code does not have.

The implication is stable and reviewable immediately; the antecedent is one `grep`
someone runs later. **A finding stated as a fact about a moving tree is worthless the
moment it is wrong about the tree, even when the reasoning is perfect** — which is
exactly how three careful readers nearly shipped one each.

This is why "to be checked, not believed" is not hedging. It is the only form in which
an observation of a moving subject carries its full weight, because the half that can
be wrong is separated from the half that cannot.

**And the third piece is provenance, which is what makes a number auditable after the
fact:** whoever posts a measurement states **who ran it**, and **against which tree
state — scoped to the paths the measurement depends on.**

> *gomi ran the grep over `lib/ src/ test/`, those paths clean at `a06a3c5`.*

**Not** "tree clean at `a06a3c5`". In a worktree shared by several owners the global
claim is almost always false — one owner is nearly always dirty somewhere — so it is a
statement people learn to write **without checking**, and it is then false on the day it
matters. That is the same degradation as every broken check above: an assertion that can
be false while the thing it protects is fine stops being read.

The scoped form is narrower, usually true, auditable, and does not go stale the moment
another owner touches their own half. It also lets two provenance claims coexist without
contradiction when they cover different paths.

Three investigations in one afternoon went into numbers taken mid-edit, and in every
case the missing information was not who was *allowed* to run it — it was *when*. A
number with an author and a tree state can be re-derived or discarded on inspection; a
number without one is an artefact whoever produced it.

Note what this is **not**: a restriction on who may run things. Reading a tree cannot
corrupt it, and a second party running someone else's prediction is *stronger* evidence
than the author confirming their own claim. Ownership governs editing, not measuring.

Concretely for this tool: **a mutation run and its survivor diff must happen against a
tree nobody is editing.** Given the classification-drift finding above, mutmut is
already sensitive to conditions outside the code; adding an unstable tree to that
makes the output unfalsifiable rather than merely noisy.

#### Every check needs a sanity case, and the equality form needs two

A check that cannot fail is the same defect as a test that cannot fail — and checks get
written in shell, where the failure is far easier to reach. In one afternoon, four
invariant `grep`s were themselves broken: two counted words rather than declarations,
one matched a text shape the target does not have, and one had a backtick inside a
double-quoted `$( )` so the shell ate the pattern. **The README row check was attempted
three times by two people and returned 0, 0, 12 — with two *different* bugs producing
identical output.** Neither became a finding only because both authors reported the
broken pattern instead of the number. That does not scale.

The rule, in three parts:

| Kind | Requirement |
|---|---|
| **Standalone check** | carries its own sanity case inline — it must demonstrate it *can* fail |
| **Suite assertion** | may borrow a positive counterpart from another test |
| **Borrowed safety** | must be written down **at the borrower**, or it is one tidy-up from gone |

The third part is the same remedy as the `DO NOT REMOVE` notes on A27b and on the
`recv_exact` byte-count test, for the same reason: a test whose value depends on
something outside itself is invisible at the point of reading. Three instances of one
fix across two halves of the codebase is a better argument than any of them alone.

**Write those notes to name what breaks, not what to preserve.** A cross-reference —
*"needed by X, do not delete"* — only helps a reader who already knows why X matters. A
note that states the **mechanism** — *every value assertion in this suite passes whether
the ADC is read before or after the upload, so deleting this test reverts the sampling
point silently* — lets a reader who has never heard of the concept derive the risk from
the text in front of them. The first defends against the careless; the second defends
against the uninformed, who are far more common and are usually acting in good faith
while tidying up.

**Distance from the consequence predicts which comments need it.** Audited against the
nine guard comments on this branch, eight already named their failure and the one that
did not is the *adapter* — a note on an interface method stating the rule (*the flag
must not be spent until the page is reachable*) while what actually breaks lives one
file away: an operator joins a network, finds no page, and the arming is gone from both
halves with nothing able to reconstruct it. The reader who would "simplify" a bool that
is always true arrives at the adapter, sees a rule with no consequence attached, and has
no reason to look further.

So the rule is not "spell out the mechanism everywhere". It is: **spell it out wherever
the guard and the failure live in different files.** Where they sit together, the code
below the comment already says it.

**And the equality form fails worse than the empty form — including the one proposed in
this document.** I recommended

```
comm -3 <(grep -rho 'MUTANT_[A-Z][A-Z_]*' lib/ | sort -u) \
        <(grep -o  'MUTANT_[A-Z][A-Z_]*' platformio.ini | sort -u)     # empty = pass
```

If the *pattern* breaks, **both** sides return nothing, `comm` returns nothing, and the
check reports PASS. Worse for a counts version: `flags 0 / envs 0 / rows 0` satisfies
the equality outright. Three checks that share a pattern family and get edited together
degrade to **agreement**, not to mismatch — which silently turns three independent
measurements into one.

So an equality check needs a **positive half as well as a negative one**: assert the
sets agree *and* that the count is the number you expect and non-zero. Agreement alone
cannot distinguish "everything matches" from "nothing was found", and those are opposite
results wearing the same output.

**The positive half is still not sufficient, and the missing piece is a negative
control.** A non-zero count only proves the pattern matched *something* — not that it
matched the thing you meant. Run the check once against a token you know is absent and
confirm it can still return its failure value:

```
grep -cE '^\[env:mutant_THIS_DOES_NOT_EXIST' platformio.ini     # must be 0
```

Without that, `12` and *"the pattern is broken in a way that happens to count something
else"* are the same observation. This is the mutation harness's own rule — *the first
run of a new mutation must FAIL* — applied to the checks instead of to the code, and it
is the half that was missing from every one of the four broken `grep`s.

A check verified with all three halves — agrees, counts what it should, and can still
fail — is the shell equivalent of a red test before a green one.

Two things follow. The mistake is not about carelessness with lines of code: it is
categorising by resemblance, and it operates on strategies and predictions as readily
as on `grep` patterns. And it is the strongest evidence in this document for the
two-reader policy — evidence against the people who wrote the policy, while writing
it.

**Budget two independent readers for a mutation triage, or accept a one-directional
miss.** That is a real line item in the adoption cost, and it is the number nobody
quotes.

The table above is the reconciled version; the corrections are called out below.

**Answering the triage-cost question directly: the equivalent-mutant class is small —
2 of 70.** `saved = False → None`, falsy under a truthiness test, and one I originally
filed as a real hole and which is not:
`if n <= 0` → `if n < 0` in `recv_exact`. With `n = 0` the guard is skipped, but
`remaining = 0` so `while remaining > 0` never runs and the function returns `b''`
having touched no socket — identical observable behaviour, including the zero
`recv` calls that `test_recv_exact_zero_bytes_returns_empty_without_calling_recv`
asserts. Verified by reading `server/tcp_server.py:109-115`, not taken on trust.
Either way the class is small: it is not what inflates triage cost.

**The class that does is f-strings, and it is a configuration mistake of mine.**
`--disable-mutation-types=string` does not remove them, because mutmut treats
`fstring` as a **separate mutation type** — verified in the installed source, not
inferred: `mutmut/__init__.py:451-465` lists `'string'` and `'fstring'` as distinct
keys. The correct flag is:

```
--disable-mutation-types=string,fstring
```

Expected effect: survivors drop from **70 to ~48**, and what remains is almost
entirely behavioural. Worth re-running once to confirm, and worth fixing before anyone
triages this list by hand.

**Disable `fstring` and `string`. Never disable `name`, `number` or `operator` to
chase the same noise.** The tempting next step is to kill the `addr[0] → addr[1]`
family the same way — and it would suppress two of the best findings in this run.
Mutant 127 is `addr[0] → addr[1]` in `data_queue.put`, which silently renames every
CSV; mutant 93 is an **operator** mutation *inside* an f-string, and it is the one
that exposed a test asserting on a substring. `fstring` only suppresses the
replace-the-whole-text form, so both survive the filter — but a broader filter would
hide them.

The general rule, which is the same one the firmware harness already follows: a
mutation type is safe to disable when **no test could legitimately assert on it**.
Log wording qualifies. Values and operators inside a message do not, because the
message can carry a computed number that something downstream reads.

#### The real holes, ranked

| Mutants | Behaviour not pinned | Why it matters |
|---|---|---|
| **93** | `f'peer closed after {n - remaining}/{n} bytes'` → `{n + remaining}` | **A weak assertion, not missing coverage — the most valuable row here.** `test_recv_exact_error_reports_partial_progress` asserts `'3/8' in str(e)`. The mutant produces `13/8`, and `'3/8'` **is a substring of** `'13/8'`, so the test passes on a wrong message. Found by sama; I had filed it as message noise and was wrong. Fix is `assert 'after 3/8 bytes'`. |
| 86 | `if n <= 0` → `if n <= 1` in `recv_exact` | `recv_exact(conn, 1)` would return `b''` without reading. No test calls it with n=1, and 1 is a byte count a short header produces. |
| 102 | `expected > MAX_PAYLOAD_BYTES` → `>=` | The inclusive boundary of the corrupt-length guard. Cheap to pin without a 1.4 MB allocation: monkeypatch the constant to 36 and assert 36 accepted, 37 refused. |
| 26, 27, 22 | `BATTERY_INVALID = -1` and `CLIENT_TIMEOUT_SEC = 6.0` survive mutation | The tests **import the constants**, so the test and the constant agree and can be wrong together — the same class as the CSV header, and DEC-0 applies for the same reason: both values are production-fidelity (`-1` is the original's battery sentinel, `6.0` its socket timeout). Pin them as **literals**. sama's point; I had rated these low value and that was the wrong call. |
| 154 | `if value <= 0` → `if value <= 1` in `validate_config_form` | **`max_acq = 1` is a legitimate setting** and this mutation would reject it. The lower bound is unpinned in the direction that breaks real use. |
| 149, 152 | `continue` → `break` in the validation loop | The form is supposed to report **all** invalid fields; with `break` it reports only the first. No test submits two bad fields at once. |
| 127 | `data_queue.put((addr[0], ...))` → `addr[1]` | The CSV filename would become `<port>_<timestamp>.csv` instead of `<ip>_...`. A silent corpus-naming change — the exact class DEC-3 protected the header from. |
| 171, 173 | `if armed is None` → `is not None` in the OTA route | Inverts which error reason is reported for a bad `/ota` post. |
| 134, 138 | `continue` → `break` on `socket.timeout`; `running = False` → `True` | 134 stops the accept loop after the first 1-second timeout — the server silently stops accepting connections. Both live in `server_main`/`exit_monitor`, which have no tests at all. |
| 82 | `subprocess.run(..., check=True)` → `check=False` | `check=True` is what converts a non-zero `gio` exit into the exception the B6 logic branches on. The existing B6 tests monkeypatch `run` to raise, so they never exercise it. |

#### Four survivors stay alive — two impossible, two chosen

The distinction matters and lumping them together as "survivors we accept" hides it.

**Impossible — equivalent, unkillable by any test that could ever be written:**

| Mutant | Why |
|---|---|
| `saved = False → None` | Both falsy under `if saved:`. No behaviour distinguishes them. |
| `if n <= 0 → if n < 0` | `n = 0` falls through to a loop that does not execute. Same `b''`, same zero `recv` calls. |

Closing either would mean restructuring working code to satisfy a tool.

**Chosen — killable, deliberately not killed:**

| Mutant | Why not |
|---|---|
| `MAX_PAYLOAD_BYTES = 1400000 → 1400001` | Killing it means pinning the literal, which would **make the comment calling it sanity headroom false**. Note mutant 102 — the `>` vs `>=` *boundary behaviour* — is a different thing and **is** worth pinning. Pinning a number the design calls arbitrary is optimising the score against the documentation. |
| `running = True → None` | Only a test that drives `server_main` reaches `tcp_server.py:237`, and buying that costs the accept loop, sleeps and retries. A bad trade for one glue mutant. |

**Unreachable does not imply we owe it a test.** That is the sentence worth keeping:
the discriminator tells you whether a mutant *can* be killed, not whether it *should*
be. Two of these are impossible and two are decisions, and a report that does not say
which is which leaves the next reader to re-derive it.

A survivor deliberately left alive is indistinguishable from one nobody looked at,
unless someone wrote down which it is.

#### The glue: ~15 survivors nothing can reach

`server_main`, `exit_monitor` and the `__main__` block account for roughly 15
survivors — `srv.listen(6)`, `max_workers=11`, `settimeout(2.0)`, `while False`,
`srv`/`state`/`threads = None`. All unkillable by unit tests, because **nothing in the
suite ever binds a socket**. The one that would actually hurt is `continue → break` on
`socket.timeout`: the accept loop exits after the first 1-second timeout and the
server silently stops accepting connections.

sama proposes one loopback integration test — bind `server_main` on port 0, connect a
real client, send header + payload + battery, assert the 10-byte response. **From a
coverage standpoint I support it, and killing mutants is the lesser reason.** Every
`handle_client` test today drives a `MagicMock` that returns whatever it was queued
regardless of the byte count it was asked for, which is why a wrong-size `recv_exact`
could pass all 117 tests. A real socket cannot ignore a byte count. That is a
documented single point of failure in the suite, and one test removes it.

Whether it is in scope for a plant server is lave's call, not QA's.

**`protocol/packet.py` has exactly one survivor and it is message text.** The module
carrying the wire contract is fully pinned. Combined with `app_state.py`'s 18/18, the
two modules where a defect would be worst are the two the suite covers best. That is
the strongest signal in this run and it is worth saying plainly, because a mutation
report is otherwise all bad news.

### Does it agree with our hand-made mutation?

**Yes on the behaviour, no on the mutant, and the disagreement is instructive.**

mutmut has **no statement-deletion operator**, so it never generates our hand mutation
("delete `self.ota_armed = False` from `take_config_for_send`"). What it generates at
that line is the keyword flip `False → True`, which is operationally the same defect —
the flag is never cleared, so every device gets `update=1`.

Run against `app_state.py` alone, cache dropped:

```
18/18   killed 18   timeout 0   suspicious 0   survived 0
```

**Eighteen mutants, zero survivors.** The only file in the backend where nothing at
all gets past the suite is the file we hand-mutated. Both methods agree, and the claim
now has a denominator — "zero survivors out of eighteen" is a result; "zero survivors"
alone is not.

The lesson generalises to the firmware harness: an automated mutation tool and a
hand-authored one are not substitutes. Ours can express "delete the line that clears
the flag" and mutmut cannot; mutmut generates 187 mutants across five modules and we
would never hand-write 187. **Different instruments, same dial.**

### What adoption actually costs

Runtime was never the binding constraint, and framing it as one — as an earlier
revision of this document did — was the wrong frame. **The binding constraint is
reader attention**, and it has a price:

| Cost | Amount |
|---|---|
| Runtime | minutes, warm cache. Not the constraint. |
| Dependency | an unmaintained 2.5.1 pin |
| Correct invocation | `--disable-mutation-types=string,fstring`, and never broader |
| **Bulk triage** | **two independent readers, or a known one-directional miss** |
| Delta triage thereafter | one reader, if a content-keyed baseline exists |

**Policy, adopted by lave:** a mutation triage gets two independent readers, or it
ships with a known one-directional miss. The evidence is five instances in one
afternoon, and the later ones are what settle it — the misses happened with the rule
articulated and in hand, while applying it. That is what makes "one careful reader
plus a good rule" insufficient rather than merely weaker. Pattern-matching does this
to whoever is holding the list, not to careless people.

**Scope it to judgement, not to arithmetic.** The survivor-set diff was done twice
independently and the two results matched exactly — the first time all day two readers
*agreed*. That agreement is information: the diff is mechanical, so one reader would
have sufficed. The policy earns its cost on judgement calls — *is this noise, is this
equivalent, will that test kill this mutant* — and wastes it on set operations. Spending
two readers on everything would discredit the rule at the point where it matters.

**When two readers are worth it, split them by angle rather than both reading top to
bottom** — otherwise the second reader repeats the first one's sweep and inherits the
same blind spot. And judge the split by what the lists *contain*, not by how much they
overlap: heavy overlap means the split failed, but disjoint lists are not automatically
success either — an angle can be disjoint because it was empty. **The test is whether
each list holds something the other angle had no way to reach.**

**Split by angle, never by file.** In a review where this was tried, the single best
outcome came from three lines both readers reached from opposite sides — one found "a
config can be applied after a failed write", the other found "the battery is written
after a failed write, and that is only safe because TCP stays broken". Same three
lines, two consequences, and the correct fix came from neither report but from putting
them together. **A file split would have handed that seam to exactly one reader, and
the fix would have been whichever half that reader could see.** Adjacent beats
disjoint.

**And silence from the other angle is not evidence unless they examined the same
question.** The rule "if the other reader didn't raise it, weight it down" is right only
when you know they looked. In the same review a fidelity finding was nearly downgraded
for being solo — but the second reader's silence came from having checked the *battery
conversion* (which a decision list enumerated) and never asking what else about the
measurement could differ. That is absence of evidence read as evidence of absence, and
it would have priced one reader's miss into the other's finding. **The reason they
missed it made the finding more likely to be real, not less.**

**Also trace before reading each other's notes**, not merely in parallel. The angle
split protects against duplicated sweeps; it does nothing against *inherited framing*.
A second reader who starts from the first's write-up has already had the discarded fact
excluded for them — which is precisely the gap that the second-reader rule cannot close
(see [the eighth](#the-eighth-is-a-different-failure-and-the-second-reader-does-not-fix-it)).

#### The survivor count is an upper bound on tests needed, not an estimate

Items 5 and 6 were never two items. `subprocess.run([...], check=True)` yields two
mutants — `destino = None` and `check=False` — because the call has two mutable parts,
but it is **one behaviour**, so it takes **one test** asserting the call it makes.
Splitting them would have meant two tests building identical fixtures to assert two
kwargs of the same call. That is why 12 planned tests became 11.

The cause is worth stating because it inflates any estimate built the same way: the
work list was derived **from the mutants rather than from the behaviours**, so a
one-call, two-property site got counted twice. **A survivor count bounds the work above;
it does not estimate it, and the gap is exactly the sites with several mutable parts.**
For an adoption estimate, "~10 real gaps" should be read as "at most 10 tests, probably
fewer".

#### Where I disagree: this argues for running it *more* often, not less

lave's reading is that a tool needing two careful readers is not a fast feedback loop,
so it belongs at milestones. **The data points the other way, and the mechanism is the
reason.**

Every miss today was **volume-induced**. Twenty-two near-identical `XX` mutants train
you to sweep the twenty-third; seven `addr` swaps train you to sweep the eighth. The
failure needs a long list of look-alikes to happen at all. A delta of two or three
mutants has no pattern to match against.

So the cost is not per-run, it is per-*bulk*-triage, and the two modes are different
work:

| Mode | When | Cost |
|---|---|---|
| **Bulk triage** — classify the whole survivor set from scratch | once, to establish a baseline | two readers, an afternoon |
| **Delta triage** — which survivors are *new*, and did any known-killed mutant come back to life | every run after that | one reader, minutes |

Milestone cadence guarantees a large unfamiliar list every time, which guarantees the
expensive mode every time. **Frequent runs are what keep the delta small, and a small
delta is what makes one reader safe.** The costs invert from what the milestone
argument assumes.

The gate is therefore not cadence but **whether a classified baseline exists**. Before
one: expensive, two readers, once — which is the work this document did. After one:
cheap, and worth running per round.

**One requirement makes or breaks this, and it falls straight out of the id-stability
finding:** the baseline must be keyed by **content**, never by mutant id. IDs renumber
on any edit above them, so an id-keyed baseline would report spurious new survivors
after every commit and be abandoned within a week. Store the `mutmut show all` output
and diff normalised `(file, removed line, added line)` triples against it.

**Status: demonstrated once, not automated.** The delta workflow was a design proposal
when first written here; it has since been run by hand — diffing `survivors.txt`
against `survivors2.txt` on normalised `(file, removed, added)` triples — and it
produced the `16 killed / 0 appeared` result that the totals could not. Content-keying
survived the renumbering, which was the part I was least sure of.

What still does not exist is the script. Two minutes by hand for one comparison is
fine; per-round it wants automating, and until it is automated the delta half of this
recommendation depends on someone remembering to do it.

#### A standing regression check that costs nothing

`config/settings.py` went from 9 survivors to 3, and all three are accounted for:

| Survivor | Status |
|---|---|
| `MAX_PAYLOAD_BYTES = 1400001` | alive on purpose — see above |
| `SERVER_IP = None` | unreachable: read only by `server_main`'s bind |
| `SERVER_PORT = None` | unreachable: read only by `server_main`'s bind |

So `settings.py` is fully triaged and **should sit at 3 indefinitely**. Notably it
should stay at 3 through the loopback integration round too: that design binds port 0
directly and never calls `server_main`, so neither `SERVER_IP` nor `SERVER_PORT` is
ever read. If the number moves, something changed that nobody intended.

A file whose survivor count is fully explained becomes a one-line regression check
rather than a number to admire. That is the useful end state for a mutation report, it
is reachable file by file, and it is a better goal than a score — **a score cannot tell
you which survivors you decided to keep.**

#### The score is not stable under unrelated changes

Measured, and neither sama nor I anticipated it. Re-running after the loopback test
landed:

| | Before the integration test | After | After `SHUT_WR` |
|---|---|---|---|
| Killed | 132 | **130** | **132** |
| Suspicious | 0 | **2** | **0** |
| **Survived** | **54** | **54** | **54** |

The survivor set never moved across all three runs. The score fell by two and came back,
and neither movement had a cause in the code or in the tests.

**Nothing about those two mutants changed**, and — correcting my own first reading of
this — **the suite did not get slower either.** The integration test runs in 0.03 s. The
cause is narrower and more interesting: *those two mutants specifically* became slow,
and only under a real socket.

Trace `remaining += len(chunk)` against the loopback test: `recv(4)` returns 4,
`remaining` grows to 8, `recv(8)` drains what is left, `remaining` grows again, and the
next `recv` has nothing to read — so it **blocks until `handle_client`'s own
`conn.settimeout(CLIENT_TIMEOUT_SEC)` fires at six seconds.** Against a `MagicMock` the
identical mutant dies in microseconds when the `side_effect` list runs out. Same mutant,
same suite, ~200× the wall clock, purely because one test talks to a kernel that is
willing to wait.

mutmut classifies by wall clock — roughly ten times baseline is a timeout,
slow-but-not-fatal is *suspicious* — so both were reclassified. Note what *suspicious*
means here: **killed, but not confidently.** The test does fail; it just fails too
slowly for the tool to attribute the failure to the test rather than to the clock.

The generalisation is therefore not "a slow test degrades classification elsewhere",
which is what I first wrote and is wrong. It is: **mutation timing is a property of the
mutant and the harness together, not of the code.** Introducing real I/O changes how
*mutants* behave, not merely how tests behave — a mutant that spins or blocks against a
real resource inherits that resource's timeouts, and the score moves as a result.

This is the strongest argument yet for the recommendation already made above — **watch
the survivor set, not the score.** Across this run the set was stable at 54 and
content-identical; the score was not. A team tracking the percentage would have opened
an investigation into a regression that did not exist, and a team tracking the set would
have correctly seen nothing happen.

And the mechanism sharpens *why*. The score moved for a cause located **neither in the
subject nor in the tests' correctness, nor even in the suite's runtime** — it was one
mutant's interaction with one socket timeout. A number that moves for reasons in none of
those three places is not a regression signal. The survivor set did not flinch.

It is the same shape as everything else in this document: a number moved, and the cause
was not in the thing being measured.

**The fix is one line and is better test design regardless of mutmut.** After its
60-byte write the client calls `shutdown(socket.SHUT_WR)`: it has nothing further to
send, so any read past the payload gets `b''` immediately instead of blocking for six
seconds. A framing regression then fails at once, and the failure stays attributable to
the code rather than to a timeout. `SHUT_WR` closes only client→server, so the response
direction and the happy path are byte-identical.

*Prediction, recorded before the run with its falsification condition attached, and
**confirmed**:* the two suspicious mutants returned to **killed** and survivors stayed
at **54**. The diagnosis was therefore right in mechanism as well as in remedy — had
they stayed suspicious, the slowness would have been somewhere other than the blocking
read.

**The rule this generalises to, for every real-I/O test after this one: remove the
reason to wait, do not shorten the wait.** The distinction is load-bearing.

| | |
|---|---|
| **Shortening the wait** | means reaching into production — `handle_client`'s `conn.settimeout(6.0)` is *the device's* timeout, and it is not ours to tune for a test's convenience |
| **Removing the reason** | means the resource has nothing left to give, so a read past the payload returns immediately. Needs no cooperation from the code under test and changes nothing on the happy path. |

Anything holding a real socket, file or subprocess should be built that way from the
start. Otherwise every mutant that runs off the end of the data pays the production
timeout, and the tool reports it as a scoring problem rather than as what it is.

#### Open predictions — unverified at time of writing

The loopback integration test landed in `541fc37` (129 passing; run alone three times,
0.03 s, identical). It closes L6: no `handle_client` test other than this one exercises
a real socket, so a wrong-size `recv_exact` could previously pass the whole suite.

Two predictions were recorded before the fact so they could fail visibly. **Both
confirmed**, by a content diff of the survivor sets rather than by the totals:

| Prediction | Result |
|---|---|
| The integration test kills **none** of the ~16 glue survivors | **Confirmed** — it never calls `server_main` |
| `config/settings.py` stays at exactly **3** survivors | **Confirmed** |

sama's stronger form — that it would kill **nothing at all** — also held: 54 survivors
before, 54 after, content-identical apart from the two reclassified mutants above.

So the integration test bought no score and removed a failure mode the score cannot see.
That was the argument for it, and it is now measured rather than argued. **Judged by
mutants killed it would have been cut** — which is the practical lesson: a tool that
scores tests will always undervalue the test that fixes the tools.

### What the score does not measure

**Mutation score measures whether your assertions are load-bearing. It does not measure
whether your test doubles are faithful. A suite of perfect assertions against a mock
that lies scores 100%.**

L6 is the worked example, and it is the reason the loopback test exists. Every
`handle_client` test but that one drives a `MagicMock` returning whatever it was queued,
**regardless of the byte count it was asked for**. So `conn.recv(remaining)` written as
`conn.recv(n)` — an ordinary human slip that reads fine and breaks every multi-chunk
read — passes the entire mock-based suite. mutmut does not generate that swap: its
operators flip comparisons, adjust numbers and null names; they do not substitute one
local for another.

The general form matters more than the example. **Mutation testing mutates production
code, and an unfaithful double is a defect in the test, not in the code.** No mutation
of the source can surface it, so the score is structurally blind to the entire class.

One correction to how this was first put to me, and then a correction to the
correction. It is **not** true that mutmut generates no wrong-byte-count mutation:
`remaining -= len(chunk)` at `tcp_server.py:95` yields both `+=` (never terminates) and
`= len(chunk)` (asks for the wrong count on the next pass). Corrupting the loop
arithmetic produces a wrong byte count without touching the argument at all, which is
the class the strong claim said was inexpressible.

**The defensible claim is narrow and specific:** mutmut cannot express *"asked the
socket for a count that is wrong from the first call"* — `conn.recv(remaining)` written
as `conn.recv(n)`. That is the shape L6 guards, and it is not generated because mutmut
does not substitute one local for another.

**The general claim is the one about doubles, and it stands unchanged:** the tool cannot
see **a mock that ignores its arguments**, because that is a property of the harness
rather than of the subject. No mutation of production code can surface a defect that
lives in the test.

Consequence for how to read a verdict here: had the loopback test been judged by mutants
killed, it would have been cut. It buys no score and removes a failure mode the score
cannot see. It is also the only test in the backend that would catch a firmware/server
framing drift — both halves are otherwise tested against their own idea of the format,
and nothing tests them against each other.

### Verdict: ADOPT NOW

| For | Against |
|---|---|
| Found a real, shipped-today hole on first run | Pinned to an unmaintained 2.5.1 |
| Minutes, not hours — fits in the loop | Needs the string filter or it is noise |
| 61.5% is a real baseline to hold a line against | Score is a bad target if anyone starts optimising it |
| Cross-checks our hand-made mutation | 69 survivors is a backlog nobody has triaged yet |

**Concrete first steps, in order:**

1. sama writes the failing test for #94 (local write fails → no Drive copy attempted).
2. Add a test asserting the sentinel terminates the writer thread, so #89 is caught by
   an assertion instead of a clock.
3. Record the invocation in `backend/README.md` — pinned version, the
   `--disable-mutation-types=string` flag, and *why* both are there. An undocumented
   pin is a trap for the next person.
4. Re-run with `--disable-mutation-types=string,fstring` and confirm survivors drop to
   roughly 48. Triage is [done](#the-70-survivors-triaged); the eight rows in that table
   are the backlog, and `recv_exact`'s boundary and the `max_acq = 1` rejection are the
   two I would write first.
5. **Do not put a mutation-score gate in CI.** 61.5% is a baseline to watch, not a
   number to hit. A score target turns into tests written against mutants rather than
   against behaviour — the firmware harness's own rule ("choose the mutation before you
   see which tests are weak") exists for the same reason.

`.mutmut-cache` is deleted and should stay untracked. A nightly run wants a cold cache
anyway.

---

## Tool 2 — QEMU, pytest-embedded, Wokwi

### Starting position: nothing is installed

```
which qemu-system-xtensa qemu-system-riscv32 wokwi-cli idf.py   -> all four absent
ls -d ~/.espressif ~/esp                                        -> neither exists
~/.platformio/packages -> contrib-piohome, framework-arduinoespressif32,
                          toolchain-xtensa-esp32, tool-esptoolpy, tool-mkfatfs,
                          tool-mklittlefs, tool-mkspiffs, tool-scons
```

No QEMU, no ESP-IDF, no wokwi-cli, and PlatformIO's package set is **Arduino-framework
only**. So unlike Tool 1, nothing here could be evaluated by running it — the cost of
producing *any* evidence is an installation first. The evaluation below is therefore
against primary sources, and every claim is labelled with how it was established.

### The three questions, answered

| # | Question | QEMU | Wokwi | Bench |
|---|---|---|---|---|
| **Q1** | Does the OTA flag survive `ESP.restart()`? (NVS) | plausible | plausible | **yes, definitive** |
| **Q2** | Is R9 real — `RTC_DATA_ATTR` retained across deep-sleep wake, reloaded on other resets? | **no** | probable | **yes, definitive** |
| **Q3** | Can a deep sleep and its wake cause be observed at all? | **no** | probable | **yes, definitive** |

### QEMU — rejected, and two of three questions are structurally dead

*Established from Espressif's own QEMU documentation and the ESP32 forum; not run.*

- **Deep sleep crashes.** Calling `esp_deep_sleep_start()` under the Espressif QEMU
  fork produces `Guru Meditation Error: Core 0 panic'ed (LoadStorePIFAddrError)`,
  traced to the RTC sleep init. **Q2 and Q3 are not "low fidelity" here — they cannot
  be asked at all.**
- **No WiFi emulation.** Espressif's peripheral list covers crypto, Ethernet (OpenCores
  MAC), GPIO strapping, eFuse, SPI flash, SD/MMC, timers. WiFi is absent. Our entire
  transmit path is out of reach.
- **RTC watchdog not emulated** — stated explicitly.
- **PSRAM is better than expected and it does not rescue this.** PSRAM *is* emulated as
  memory (`-m 2M` / `-m 4M`); only the PSRAM **MMU** is missing, so bank switching and
  himem do not work. Our 700000 B allocation is ~683 KB, well under 2 MB, and needs no
  bank switching — so `ps_malloc(700000)` would plausibly work in QEMU. lave's
  assumption that PSRAM caps the whole approach turns out to be **wrong**; it is deep
  sleep and WiFi that kill it, not memory.
- **Framework mismatch.** `pytest-embedded-qemu` is built around the ESP-IDF build
  layout. This project is `framework = arduino` under PlatformIO. Bridging that is real
  work before the first assertion runs.

So QEMU would cost an ESP-IDF installation plus a build-system port, to answer **one**
of three questions — the one a bench answers in ten minutes.

### Wokwi — capable, and rejected anyway for these questions

*Established from Wokwi's documentation and public projects; not run.*

What it genuinely does that QEMU cannot:

- **Deep sleep works**, including `esp_sleep_enable_ext0_wakeup` and
  `esp_sleep_get_wakeup_cause()` returning `ESP_SLEEP_WAKEUP_EXT0`. Public projects use
  `esp_sleep_enable_ext0_wakeup(GPIO_NUM_33, ...)` — **our exact pin**.
- **PSRAM supported** on ESP32 via a `psramSize` attribute (2/4/8 MB).
- **WiFi supported**, unlike QEMU.
- **GPIO with interrupts supported.**

**The GPIO33 question, answered directly:** yes, `wokwi-cli` can script a reed-switch
edge — but **not** by driving a raw pin level, which is what the question assumes. There
is no "set pin 33 low" step. Scenarios drive a *part's control*:

```yaml
steps:
  - set-control: { part-id: reed, control: pressed, value: 1 }
  - delay: 100ms
  - set-control: { part-id: reed, control: pressed, value: 0 }
  - wait-serial: 'Coletando dados'
```

Model the reed as a `wokwi-pushbutton` or slide switch wired to GPIO33 and you have a
scriptable, CI-runnable magnet pass. Full step vocabulary: `delay`, `set-control`,
`wait-serial`, `write-serial`, `expect-pin`, `take-screenshot`, `touch*`.

Why it is still a reject **for these three questions**:

1. **It cannot close the transmit path.** Wokwi CI runs in **Wokwi's cloud**. Our device
   takes static IP `192.168.1.118` and connects to `192.168.1.100:12345` — a server on
   our LAN. A cloud simulation cannot reach it. The half of the duty cycle that is
   currently least tested is the half Wokwi cannot help with.
2. **The firmware binary leaves the building.** Today that is harmless. Once DEC-2 lands
   and the real plant SSID and password are compiled into the image, uploading that
   binary to a third-party cloud service becomes a credential-disclosure decision, not a
   tooling decision. **That is bigboss's call, not QA's, and it should be made before we
   build a dependency on it, not after.**
3. **Metered and alpha.** Free tier is 50 simulation-minutes/month; the scenario API is
   documented as alpha and subject to change. Fine for an experiment, thin for something
   the release depends on.
4. **A simulator answering Q2 is weak evidence for Q2.** The question is whether *the
   silicon* reloads RTC memory on a non-deep-sleep reset. A simulator that agrees with
   the documentation tells us the simulator implements the documentation. The failure
   mode we care about — silicon behaving unlike the docs on some reset path — is exactly
   the one emulation cannot surface.

### What to do instead

H6 and H7 from `test-plan.md`, on one pico32, with a serial print. Concretely:

```
Q2/R9:  print `stage` at boot, then exercise four resets —
        timer deep sleep, ESP.restart(), power cycle, EN pin.
        Expect: retained on the first, initial value on the other three.
Q1/H7:  arm OTA, let a device transmit, watch it restart, confirm the AP appears;
        then power-cycle mid-window and confirm it does NOT come back into OTA.
Q3:     esp_sleep_get_wakeup_cause() printed at boot — timer vs ext0, directly.
```

Under an hour, on the actual chip, answering all three at a fidelity no emulator
offers. **Fifteen minutes of that hour closes the most load-bearing unverified
assumption on the branch.**

### When to revisit Wokwi

One case is genuinely strong, and it is not on lave's list: **putting the
trigger-stops-acquisition path in CI.** A scripted GPIO33 edge is the closest thing to
testing the belt without a belt, and no host test can reach the `digitalRead` →
`BeltTrigger::poll` → `AcquisitionService::step` loop in `app.cpp:96-152`, which is
precisely the Arduino-side code round 9 is meant to extract.

Revisit if **all** of these hold:

1. Round 9 has extracted the cycle loop, and it is *still* worth testing the wired
   version — if extraction makes it host-testable, Wokwi buys nothing.
2. bigboss has ruled on shipping a credential-bearing binary to a third-party cloud.
3. Someone wants the belt path in CI badly enough to fund a metered dependency.

Until then the honest position is that we have hardware, we have not used it for these
questions yet, and buying an emulator before using the bench would be paying to avoid a
walk to the lab.

---

## Sources

Tool 1 is measured on this branch. Tool 2's capability claims are from:

- [Espressif QEMU — ESP32 emulation status](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/esp32/README.md) — peripheral list, PSRAM MMU limitation, RTC watchdog
- [ESP-IDF QEMU guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/qemu.html) — flash image handling
- [ESP32 forum — QEMU crash on deep sleep](https://esp32.com/viewtopic.php?t=35904) — `LoadStorePIFAddrError` in `esp_deep_sleep_start`
- [pytest-embedded](https://docs.espressif.com/projects/pytest-embedded/en/latest/) — plugin set incl. `-qemu`, `-arduino`, `-wokwi`
- [Wokwi ESP32 simulation features](https://docs.wokwi.com/guides/esp32) — PSRAM, WiFi, GPIO support matrix
- [Wokwi automation scenarios](https://docs.wokwi.com/wokwi-ci/automation-scenarios) — full step vocabulary
- [Wokwi CI](https://docs.wokwi.com/wokwi-ci/getting-started) — cloud execution, metering
