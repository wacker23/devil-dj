# Track Matcher — "what should I play next?"

The Track Matcher ranks candidate tracks for the idle deck while another deck plays. It searches the current playlist (or any scope the DJ picks), explains every suggestion, learns the owner's taste from her own choices, and can reorder a whole playlist for the best flow. Everything runs on the laptop; nothing is uploaded.

It is built in three layers, each shippable on its own:

| Layer | What it adds | Phase |
| --- | --- | --- |
| 1. Rule-based matcher | Hard filters + a weighted, explainable score from BPM, key, energy, sound, groove, tags, history | 3 (MVP) |
| 2. Structure-aware + personal AI | Mix-point, phrase and vocal features; logs choices; learns personal weights (learning to rank) | 6 |
| 3. Deep audio similarity + set planning | Neural audio embeddings, Smart Sort (whole-playlist ordering), Bridge (find tracks that connect A to B) | 7 |

## 1. Terms

- **A** = the track playing on the master deck (the context). **B** = a candidate for the other deck.
- **Mix-out region of A**: the last 32 beats before its mix-out point (or before the outro phrase / track end).
- **Mix-in region of B**: the first 32 beats after its mix-in point (or the first downbeat).
- **Intent** (DJ-controlled): `down` (cool down), `hold` (keep energy), `up` (build), `switch` (change style on purpose). Default `hold`.
- **Scope**: current playlist (default when a playlist is loaded), a folder, a smart playlist, tagged tracks, or the whole library.

## 2. Pipeline

```
trigger ─▶ candidate generation (SQL prefilter) ─▶ feature computation f_i(A,B) ─▶ score S(A→B)
        ─▶ diversity re-rank (MMR) + 1 wildcard slot ─▶ top 10 with explanations ─▶ Next Track panel
```

Triggers: master deck changes, a track loads, A's playhead enters its last 25 % (mix-out context becomes known), intent/scope/preset changes, library analysis finishes. Runs on a worker thread; results go to the UI via a message. Budget: ≤ 30 ms for 5,000 candidates.

## 3. Hard filters (candidate generation)

Executed as one SQL query, then in-memory checks:

1. File present and analyzed (BPM + key known).
2. Not the same track, not a duplicate (same content hash), not played in this session (setting: exclude / penalize / ignore).
3. **Tempo reachable**: exists m ∈ {½, 1, 2} with |A.bpm_now / (B.bpm · m) − 1| ≤ `maxStretch` (default 8 %).
4. **Key policy**: `strict` (score ≥ 0.85 in the harmonic table), `relaxed` (≥ 0.5), `off`.
5. In scope; optional rating ≥ N; optional tag include/exclude.

## 4. Features (each returns a value in [0, 1] and a short reason)

### 4.1 Tempo fit

`s = A.bpm_now / (B.bpm · m) − 1` for the best m. `f_tempo = exp(−(s / 0.03)²)`.
If key lock is off on the target deck, the stretch also shifts B's key by `12·log2(1 + s)` semitones; the harmonic feature uses that effective key.
Reason: `BPM +2.1 %` (or `BPM ×2`).

### 4.2 Harmonic fit (Camelot wheel)

Key index k: 0–11 = C…B major, 12–23 = C…B minor, pitch class p = k mod 12.

```
camelotNumber(major p) = ((7·p) mod 12 + 7) mod 12 + 1      // C major → 8, G major → 9
camelotNumber(minor p) = ((7·p) mod 12 + 4) mod 12 + 1      // A minor → 8, E minor → 9
letter = minor ? 'A' : 'B'
```

Let Δ = (numB − numA) wrapped to [−6, 6], `same` = same letter.

| Relationship | Condition | f_key |
| --- | --- | --- |
| Same key | Δ = 0, same | 1.00 |
| Adjacent (fifth up/down) | \|Δ\| = 1, same | 0.90 |
| Relative major/minor | Δ = 0, different | 0.85 |
| Diagonal | \|Δ\| = 1, different | 0.60 |
| Energy boost (+2) | Δ = +2, same | 0.55 |
| One semitone up | Δ = −5, same (Camelot +7) | 0.50 |
| Two steps down | Δ = −2, same | 0.45 |
| One semitone down | Δ = +5, same (Camelot −7) | 0.40 |
| Anything else | — | 0.10 |

The table lives in `matcher/harmonic_table.json` so it can be tuned. If Key Shift is allowed (setting), also evaluate B transposed by −1/+1 semitone with a 0.1 cost each and keep the best; the reason then says `8A→9A (shift +1)`.
Reason: `8A → 9A`.

### 4.3 Energy flow

Energy per 16-bar window, computed in analysis (Phase 2):

```
raw = 0.35·z(short-term loudness) + 0.25·z(onset density) + 0.20·z(low-band energy ratio) + 0.20·z(spectral centroid)
energy = percentile of raw within the owner's library   // 0..1; also shown as 1–10
```

`eA` = A's energy over its mix-out region, `eB` = B's energy over its mix-in region (whole-track mean until phrase data exists). Target delta by intent: down −0.10, hold 0, up +0.10, switch 0 (with wider tolerance).

`f_energy = exp(−((eB − eA − target) / 0.15)²)` (tolerance 0.25 for `switch`).
Reason: `Energy 6 → 7 ↑`.

### 4.4 Sound similarity

Each track gets a **sound vector**:

- v1 (Phase 2, our own DSP, 32-d): means and standard deviations of 13 MFCCs, spectral centroid, roll-off, flatness, onset rate, low/mid/high energy ratios; z-normalized over the library.
- v2 (Phase 7): a neural audio embedding (see §8), computed for the whole track, the intro and the outro.

`c = cosine(A.outro or A.whole, B.intro or B.whole)`, calibrated with library percentiles of pairwise similarity: `f_sound = clamp((c − c10) / (c90 − c10), 0, 1)`, so 0.5 means "typical for your library". With intent `switch`, the feature is inverted (prefer contrast) at half weight.
Reason: `Similar sound 84 %`.

### 4.5 Groove similarity

A 48-d groove profile: average accent strength at each of 16 sixteenth-note positions per bar in three bands (low/kick, mid/snare, high/hats), normalized. `f_groove = cosine` mapped to [0, 1]. Distinguishes four-on-the-floor, broken beat, half-time.
Reason: `Same groove`.

As built (P6-10): the regions come from `util::mixRegions` (MIX IN / MIX OUT, else the phrases, else the ends). eB is B's energy over its mix-in region and B's sound its intro vector; A's are its whole-track values until its playhead enters its mix-out region (`Context::aInOutro`, also a trigger), then the region's energy and its outro vector. Region energy = the 0..1 energy curve averaged over the region by window overlap (at query time, so mix points count at once); the intro / outro sound vectors are computed by the analysis on the phrases it finds (`track_embeddings` 'sound_v1' / 'intro', 'outro').

### 4.6 Vocal clash (Phase 6)

From vocal presence per phrase (vocal stem energy or a vocal detector): `f_vocal = 1 − 0.8·(vocal(A.mixOut) · vocal(B.mixIn))`. Reason when low: `Vocal clash`.

### 4.7 Mixability (Phase 6)

Rewards tracks built for mixing: B has a DJ-friendly intro (≥ 16 bars, sparse, little vocal), A has a usable outro. `f_mix = 0.5·introQuality(B) + 0.5·outroQuality(A)`. Reason: `Long intro`.
As built (P6-10): quality = 0.4·min(1, bars / 16) + 0.3·clamp(0.5 + track energy − region energy) + 0.3·(1 − vocal); a side without phrases counts 0.5 (its unknown parts 0.5 too); no value when neither has phrases. Vocal: reasons `Vocal clash` (product ≥ 0.4) and `No vocals over the mix`.

### 4.8 Tags and genre

Jaccard overlap of genre + custom tags: `f_tags`. With intent `switch`, weight drops to 0.

### 4.9 Personal history

`f_hist = 0.6·g(n_AB) + 0.25·g(n_artistA→artistB) + 0.15·g(n_tagpair)` with `g(n) = log(1 + n) / log(1 + n_max)`, counts from the owner's history (transitions she actually played). Reason: `You played this after A 3×`.

### 4.10 Rating and freshness

`f_rating = rating / 5` (unrated = 0.5). `f_fresh` = 1 − (plays in the last 30 days / max), so overplayed tracks sink a little.

### 4.11 Penalties (subtracted after weighting)

- Same artist as any of the last 3 tracks: −0.15.
- Played earlier this session (if the filter is set to "penalize"): −0.30.
- Same sound cluster for the last 4 tracks and intent ≠ `hold`: −0.05 (gentle variety push).

## 5. Score

```
S(A→B) = Σ_i w_i · f_i / Σ_i w_i  −  penalties          // features with missing data are skipped, weights renormalized
```

Weight presets (`matcher/presets.json`, user-editable):

| Preset | tempo | key | energy | sound | groove | vocal | mix | tags | history | rating | fresh |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Balanced (default) | 1.0 | 1.0 | 1.0 | 0.8 | 0.6 | 0.6 | 0.4 | 0.4 | 0.5 | 0.3 | 0.2 |
| Harmonic | 0.8 | 2.0 | 0.8 | 0.6 | 0.5 | 0.8 | 0.4 | 0.3 | 0.4 | 0.3 | 0.2 |
| Peak time | 1.0 | 0.8 | 1.6 | 0.8 | 0.8 | 0.6 | 0.6 | 0.3 | 0.4 | 0.4 | 0.2 |
| Warm-up | 1.0 | 1.0 | 1.2 | 1.0 | 0.6 | 0.6 | 0.6 | 0.5 | 0.4 | 0.3 | 0.3 |
| Open format | 0.6 | 0.6 | 1.0 | 0.4 | 0.4 | 0.8 | 0.4 | 0.8 | 0.8 | 0.4 | 0.3 |
| Discovery | 1.0 | 1.0 | 0.8 | 0.8 | 0.6 | 0.6 | 0.4 | 0.3 | 0.0 | 0.0 | 1.0 |

These starting weights are guesses to be tuned with the evaluation tool (§10) and then by the personal ranker (§7).

### 5.1 Diversity and exploration

- Re-rank the top 50 with maximal marginal relevance (λ = 0.8, similarity = sound vector cosine) so the top 10 aren't ten near-copies.
- Slot 10 is a **wildcard**: the best-scoring track whose sound differs most from A (still passes the hard filters). It keeps the list from getting stale and gives the learner informative choices. Marked with a ✦ icon.

## 6. UI and controls

**Next Track panel** (side panel, toggled with the browser):

- Header: scope dropdown, preset dropdown, intent buttons ↓ → ↑ ⤨, key policy, "include played".
- List of 10: title/artist, BPM, key, energy 1–10, total score bar, up to 4 reason chips (green = good, amber = weak, red = clash). Hover/long-press shows the full breakdown of every feature and weight.
- Mini view: energy line of the last 5 played tracks + the highlighted suggestion; Camelot wheel showing A and B.
- 👍 / 👎 after each transition (also on keys, see KEYMAP) — feeds §7.

Actions (see `docs/KEYMAP.md`): **N** load the highlighted suggestion into the idle deck; **Shift+N** move the highlight down the list; **Ctrl+N** preview it in headphones; **Ctrl+Shift+N** cycle intent.

**Automix "smart" mode:** instead of playing the playlist in order, Automix picks argmax S from the remaining tracks in scope at each transition, following the energy arc (§9) if one is set.

## 7. Personal AI: learning to rank from the owner's choices (Phase 6)

### 7.1 What is logged (locally, `choice_log` table)

An **impression** is recorded when a track is loaded into the idle deck while the other deck plays: context track A, intent, preset, scope, the candidates shown (ids, ranks, feature vectors x(A,B)), the chosen track B (in the list or not — if not, its features are computed on the spot), and later the **outcome**:

| Outcome | Label |
| --- | --- |
| B mixed in and played ≥ 60 s | positive (strong) |
| 👍 after the transition | positive (strong) |
| B loaded but ejected/replaced before mixing | negative (weak) |
| 👎 after the transition | negative (strong) |
| Shown but not chosen | implicit negative relative to the chosen track |

Logging can be turned off in settings; logs can be deleted or exported (CSV/JSON) for the owner's own research.

As built (P6-11): `matcher/ChoiceLog` (x(A, B) = the 11 features, null when missing, then energy delta x [intent up], energy delta x [intent down], sound x [intent switch]; the `choice_log` rows; CSV in long format - one row per shown track with its x - and JSON) and `app/ChoiceTracker` (the outcomes). A load counts when a library track goes on the idle deck while the other deck plays another, and Automix isn't running; the shown list is the last suggestions for that A, and a chosen track not among them is scored on the spot (`MatcherService::scoreOne`). Outcomes: 60 s played in all = mixed; the deck loaded again before that = ejected; ' / Shift+' (or the Next Track panel's check / cross) = thumbs up / down on the last impression heard. The app hands out the ids and writes in the background. Options > Track Matcher log: on / off (setting `logChoices`), export CSV / JSON, delete (asks). `tools/check_choice_export.py` reads both exports with pandas.

### 7.2 Model v1 — pairwise logistic ranker (on-device)

Feature vector x(A,B) = the 11 features of §4 plus 3 interaction terms (energy delta × intent up, energy delta × intent down, sound × intent switch) = 14 dims.

```
P(B ≻ C | A) = σ( wᵀ (x(A,B) − x(A,C)) )
loss = − Σ_pairs log P  +  λ ‖w − w_preset‖²        // stays close to the preset when data is scarce
```

- Pairs: chosen B vs each shown-not-chosen C; plus 5 random in-scope tracks as easy negatives; strong labels weight 2, weak 0.5.
- Weights constrained ≥ 0 (projected gradient) so they stay interpretable.
- Trained in the background at app exit (or on demand), L-BFGS or Adam over all logs; < 1 s for 10k pairs.
- Blending: `w = (1 − α)·w_preset + α·w_learned`, `α = n / (n + 200)` where n = number of impressions.
- Shown to the user as "What you care about": bar chart of normalized weights, with reset.

As built (P6-12, `matcher/Ranker`): pairs from the log only - the 5 random easy negatives are left out (their x isn't in the log; it would mean scoring the library at training time). Pair weights: mixed / thumbs up 2, a plain choice 1; ejected 0.5 and thumbs down 2 with the pair turned round. A value missing on either side counts 0. Full-batch Adam, 400 steps, w ≥ 0 by projection, λ = 20 (the preset weighs about as much as 20 pairs). The learnt weights are scaled to the preset's sum of the 11 features before blending. The three interaction weights go into the score as Σ v·g over the same total weight (0 in every preset). Stored in `matcher_models` "personal-v1" (JSON: based_on, dims, w). The app loads it at start, blends it into the current preset's weights for every request, writes "personal-v1.<version> a=<alpha>" as the log's model version, and trains at exit when there are choices it hasn't seen (Options > Track Matcher log > What you care about: TRAIN NOW, RESET).

### 7.3 Model v2 (optional, Phase 7 or research)

- A small gradient-boosted-tree or MLP ranker trained offline in Python on exported logs and shipped as ONNX (`matcher/ranker.onnx`), used if it beats v1 on the evaluation tool.
- Embedding adaptation: learn a 64-d projection of the neural embedding with a triplet loss (anchor = A's outro, positive = chosen B's intro, negative = shown-not-chosen) so "sounds good together" reflects the owner's taste rather than generic similarity.
- Research track (not product-critical): pre-train a transition model on public DJ-mix analyses (mix-to-track alignment work exists in the research literature); check dataset licences before any use in the product.

## 8. Neural audio embeddings (Phase 7)

- Requirements: ONNX-exportable, runs on CPU in ≤ 2 s per 30-s segment, **licence allows commercial use of the weights**.
- Candidates to evaluate (licence status as found in Nov 2026 — re-verify at the source before shipping):
  - LAION CLAP music checkpoint (`music_audioset_epoch_15_esc_90.14`) — mirrors list the checkpoints as CC0-1.0. Strong for music similarity; 512-d.
  - OpenL3 — MIT-licensed library; confirm the weight files' licence.
  - Essentia Discogs-EffNet / MAEST — CC BY-NC-SA 4.0 (non-commercial) with a proprietary licence on request → **not allowed** under the closed-capable policy unless licensed.
- Store 3 vectors per track (whole, intro, outro), PCA-reduced to 128-d float16 (50k tracks ≈ 39 MB in RAM).
- Background job, lowest priority, resumable; tracks without embeddings fall back to the v1 sound vector.

## 9. Set planning

### 9.1 Energy arcs

Presets: Warm-up ramp (0.3→0.6), Peak plateau (0.7→0.85), Wave (two peaks), Cool-down (0.8→0.4), Custom (draw a curve). The arc gives the target energy at each set position; the intent buttons override it live.

### 9.2 Smart Sort (order a playlist)

Given a playlist (≤ 500 tracks), optional fixed first track and an arc, find an order maximizing

```
J = Σ_i S(t_i → t_{i+1})  −  β · Σ_i (E(t_i) − arc(i / n))²  −  γ · (artist repeats within 3)
```

Algorithm: beam search (width 16) to build a sequence, then local search with or-opt moves (move a block of 1–3 tracks) and swaps until a 2-second budget expires; deterministic seed. Output: a **new** playlist "<name> (Smart Sort)" — the original is never overwritten without confirmation — plus a flow view (energy line vs arc, key wheel path, weakest transitions highlighted).

### 9.3 Bridge (connect A to a target track)

"I want to get to track X — what can I play in between?" Build a k-nearest-neighbour graph (top 50 successors per track by S, precomputed in the background and refreshed after analysis). Find the path A → … → X of 1–3 hops minimizing Σ −log S(edge) (Dijkstra). Show up to 3 alternative paths.

## 10. Evaluation tool

`tools/matcher_eval` (C++ CLI, uses the app's library code):

- **Offline replay**: for every transition in the owner's history (A→B actually played), rank B among the scope available at that time (excluding later imports). Report HitRate@1/5/10 and MRR per preset and for the learned model.
- **Ablation**: drop one feature at a time to see what matters for this library.
- **Ordering**: compare Smart Sort J against the owner's own historical orderings.
- Output a markdown report into `docs/reports/`.

Product metrics shown in the debug overlay: acceptance rate (share of loads taken from the top 10), mean rank of chosen track, 👍 rate. Targets are set after the first report, not guessed up front.

As built (P6-13): `matcher/Evaluation` + `tools/matcher_eval` (`matcher_eval.exe [--library] [--out] [--no-tune] [--write-presets]`). Replay: B ranked by its score among the tracks in the library at the time (date_added), not A itself, with the default filters (tempo within 8 %, keys relaxed; B filtered = a miss); the history features from history before the transition only (pairs after A, artist pairs, plays in the 30 days before), so nothing is seen from hindsight; ties count as the middle of the tie. Each candidate's features are computed once per transition and only re-weighted per weight set (presets, the personal blend, Balanced without each feature). Tuning: coordinate ascent of Balanced's weights (x 0 / 0.5 / 1 / 1.5 / 2 / 3, two passes) on MRR - in-sample, a suggestion. The report also has the choice log's metrics (share taken from the list, mean chosen rank, outcomes, thumbs) and the personal model's weights. `--write-presets` adds "Tuned" to `Documents\Devil-DJ\Matcher\presets.json`, which the app now reads at start (presets otherwise built in). Smart Sort orderings: with Smart Sort (Phase 7).

## 11. Data model additions (schema v2)

```sql
CREATE TABLE track_features (
  track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
  version INTEGER NOT NULL,
  energy REAL, energy_intro REAL, energy_outro REAL,
  energy_curve BLOB,              -- float16 per 16-bar window
  energy_features BLOB,           -- per window: start, length, loudness dB, onset rate, low ratio, centroid (6 x float32; P2-24)
  sound_v1 BLOB,                  -- 32 × float32
  groove BLOB,                    -- 48 × float32
  vocal_intro REAL, vocal_outro REAL,   -- Phase 6
  intro_beats INTEGER, outro_beats INTEGER
);

CREATE TABLE track_embeddings (
  track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
  model TEXT NOT NULL, segment TEXT NOT NULL,   -- 'whole' | 'intro' | 'outro'
  dim INTEGER NOT NULL, vec BLOB NOT NULL,      -- float16
  PRIMARY KEY (track_id, model, segment)
);

CREATE TABLE choice_log (
  id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL, created_at INTEGER NOT NULL,
  context_track INTEGER, chosen_track INTEGER,
  intent INTEGER, preset TEXT, scope TEXT,
  shown_json TEXT NOT NULL,       -- [{"track":123,"rank":1,"x":[...14 floats...]}, ...]
  chosen_rank INTEGER,            -- NULL if not from the list
  outcome INTEGER,                -- 0 unknown, 1 mixed ≥60 s, 2 ejected, 3 thumbs up, 4 thumbs down
  model_version TEXT
);

CREATE TABLE matcher_models (
  name TEXT PRIMARY KEY, version INTEGER NOT NULL,
  weights_json TEXT NOT NULL, trained_at INTEGER, n_impressions INTEGER
);

CREATE TABLE track_neighbors (      -- kNN graph for Bridge / Automix
  track_id INTEGER, neighbor_id INTEGER, score REAL,
  PRIMARY KEY (track_id, neighbor_id)
);

CREATE TABLE feature_stats (        -- library statistics (P2-24): 'energy' (z means / sds), 'sound_mean', 'sound_sd'
  name TEXT PRIMARY KEY, data BLOB
);
```

## 12. Code layout

```
src/matcher/
  Features.h/.cpp          f_tempo, f_key, f_energy, ... (pure functions + reasons)
  Camelot.h/.cpp           key ↔ Camelot conversions and relationship table
  CandidateQuery.h/.cpp    SQL prefilter
  Scorer.h/.cpp            weights, presets, penalties, MMR, wildcard
  MatcherService.h/.cpp    worker thread, triggers, results to UI
  ChoiceLogger.h/.cpp      impressions and outcomes
  Ranker.h/.cpp            pairwise logistic ranker (train + apply)
  SmartSort.h/.cpp         beam search + local search
  Bridge.h/.cpp            kNN graph + Dijkstra
src/analysis/
  EnergyAnalyzer, SoundVector, GrooveProfile (Phase 2), EmbeddingJob (Phase 7)
src/ui/
  NextTrackPanel, FlowView, MatcherWeightsView
tools/matcher_eval/
```

## 13. Tests

- Camelot: all 24 keys convert correctly (C major 8B, A minor 8A, G major 9B, E minor 9A, F major 7B, D minor 7A, …); relationship table symmetric where it should be.
- Each feature: boundary values, missing-data handling, reason strings.
- Scorer: renormalization when features are missing; presets load; penalties apply.
- MMR: no two items in the top 10 with sound cosine > 0.98 when alternatives exist.
- Ranker: on synthetic logs generated from known weights, training recovers the weight ordering; blending formula.
- Smart Sort: for n = 8 random tracks, result within 2 % of the brute-force optimum; respects fixed first track and artist spacing.
- Bridge: finds the known best path on a hand-built graph.
- Performance: 5,000 candidates scored in ≤ 30 ms (Release, target laptop).
