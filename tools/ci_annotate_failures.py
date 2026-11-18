"""Turn failed tests in a ctest --output-on-failure log into GitHub Actions error annotations.

Annotations are readable through the public checks API, so a failed CI run can be diagnosed without
downloading the (sign-in only) job log. Usage: python tools/ci_annotate_failures.py ctest.log
"""

import re
import sys

MAX_ANNOTATIONS = 10  # GitHub shows at most 10 error annotations per step
MAX_LINES = 12


def failures(log_text):
    """Yields (title, detail lines) for each failed test in the log."""
    lines = log_text.splitlines()
    failed_tests = []
    for line in lines:
        m = re.search(r"Test\s+#\d+:\s+(.*?)\s+\.{3,}\*+(Failed|Exception|Timeout)", line)
        if m:
            failed_tests.append((m.group(1).strip(), m.group(2)))

    # Catch2 failure blocks: "...cpp(123): FAILED:" followed by the check and its expansion.
    blocks = []
    for i, line in enumerate(lines):
        if re.search(r"\): FAILED:|\): error:", line) or "due to unexpected exception" in line:
            block = [line.strip()]
            for follow in lines[i + 1:i + MAX_LINES]:
                if not follow.strip() or follow.startswith("---") or follow.startswith("==="):
                    break
                block.append(follow.rstrip())
            blocks.append(block)

    if not failed_tests and not blocks:
        return
    for index, (name, kind) in enumerate(failed_tests or [("tests", "Failed")]):
        detail = blocks[index] if index < len(blocks) else [kind]
        yield name, detail
    for block in blocks[len(failed_tests):]:
        yield "test failure", block


def escape(text):
    return text.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


def main(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    count = 0
    for title, detail in failures(text):
        if count >= MAX_ANNOTATIONS:
            break
        print(f"::error title={escape(title)}::{escape(chr(10).join(detail))}")
        count += 1
    if count == 0:
        print("::error title=ctest failed::no failure details found in the log")


if __name__ == "__main__":
    main(sys.argv[1])
