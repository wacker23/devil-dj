"""P6-11: check that the Track Matcher's choice-log exports open in pandas.

Usage: python tools/check_choice_export.py [folder]
The folder (default build/choice-export-test, written by the ChoiceLog tests) holds choices.csv and choices.json.
The owner's own exports (Options > Track Matcher log > Export) can be checked the same way.
"""
import json
import sys
from pathlib import Path

import pandas as pd

DIMS = ["tempo", "key", "energy", "sound", "groove", "vocal", "mix", "tags", "history", "rating", "fresh",
        "energy_up", "energy_down", "sound_switch"]


def main() -> int:
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "build" / "choice-export-test"
    csv = pd.read_csv(folder / "choices.csv")
    assert list(csv.columns[-14:]) == DIMS, csv.columns
    assert {"impression", "track", "rank", "chosen", "outcome", "intent"} <= set(csv.columns)
    per = csv.groupby("impression")
    assert (per["chosen"].sum() == 1).all(), "each impression has exactly one chosen track"
    print(f"CSV: {len(csv)} rows, {csv['impression'].nunique()} impressions; outcomes {dict(csv.groupby('impression')['outcome'].first().value_counts())}")
    print(csv[["impression", "track", "rank", "chosen", "outcome", "tempo", "energy_up"]].head(8).to_string(index=False))

    data = json.loads((folder / "choices.json").read_text(encoding="utf-8"))
    assert data["dims"] == DIMS
    shown = pd.json_normalize(data["impressions"], record_path="shown", meta=["id", "outcome", "preset"])
    x = pd.DataFrame(shown["x"].tolist(), columns=DIMS)
    assert len(x) == len(csv), (len(x), len(csv))
    print(f"JSON: {len(data['impressions'])} impressions, {len(shown)} shown rows, x {x.shape}")
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
