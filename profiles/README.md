# KeyWeaver Style Profiles

Generated Target-K profile JSON files can live here when they should be reused across playtests.

Recommended first profile:

```bash
python scripts/build_target_k_profile.py --songs-root "<osu Songs folder>" --target-keys 10 --profile-name keyweaver_10k_style_v1 --profile-kind style --window-ms 1000 --curated-list scripts/u_e_10k_curated_patterns.txt --out profiles/keyweaver_10k_style_v1.json
```

`keyweaver_10k_style_v1` is not a general 10K average. It is a KeyWeaver style profile built from curated u_e 10K reference patterns, intended to express the target hand-feel and lane usage KeyWeaver should chase.

For a broader style-stat profile, include both u_e and CircusGalop references while keeping key-conversion-tagged variants such as `4K10C` / `5K7C` out:

```bash
python scripts/build_target_k_profile.py --songs-root "<osu Songs folder>" --author u_e --author CircusGalop --author-path-prefilter --target-key-path-prefilter --target-keys 10 --profile-name keyweaver_10k_broad_style_v1 --profile-kind style --window-ms 1000 --out profiles/keyweaver_10k_broad_style_v1.json
```

The committed `keyweaver_10k_broad_style_v1.json` is sanitized: it keeps aggregate statistics and removes local Songs-folder paths.

The current v0.7.0 algorithm contract is frozen in `../docs/algorithm-lock-v0.7.0.md`.
The older v0.5.5 Composer baseline remains available in `../docs/algorithm-lock-v0.5.5.md`
as historical context for the density-bucket change.
For 10K conversion tuning, Composer/Repair local pressure should use the 1000 ms
`densityBuckets.low/mid/high/chordHeavy/jackRisk` window features from this profile,
not chart-level `chartSummary` fields.
