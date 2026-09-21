#!/usr/bin/env python3
"""Repair the punctuation damaged by a PowerShell text round-trip on a UTF-8 file.

Reading a UTF-8 file as cp936 and writing it back as UTF-8 mangles every 3-byte
character that is followed by an ASCII byte: the trailing byte merges with the next
one, so the pair [3-byte char][ASCII char] comes back as two broken characters. The
ASCII half is recoverable from context, and the rules below encode that knowledge
(they were read off the damaged spots, one by one).

    python tools/fix_utf8.py README.md           # report what is broken
    python tools/fix_utf8.py README.md --apply   # repair
"""
import sys

# Longest / most specific first: every rule is an exact context, so order matters.
RULES = [
    ("\ufffd?36\u00d7", "\u2248136\u00d7"),          # **48.79 ms (approx 136x)**
    ("\ufffd?6\u00d7", "\u224816\u00d7"),            # sun disc approx 16x
    ("\ufffd?\u00d7", "\u22488\u00d7"),              # mirror specular approx 8x
    ("\ufffd?.6\u00d7", "\u22483.6\u00d7"),          # receiver core approx 3.6x
    ("(\ufffd?off)", "(\u2248 off)"),
    ("(0.5\ufffd?.0)", "(0.5\u20138.0)"),
    ("25\ufffd?9 ms", "25\u201349 ms"),
    ("20\ufffd?0 FPS", "20\u201340 FPS"),
    ("| \ufffd?59 FPS", "| \u2248 59 FPS"),
    ("`\ufffd?\u2192`", "`\u2190/\u2192`"),          # the left/right arrow hints
    # Arrows rather than dashes where the text describes a transformation.
    ("\ufffd?**46.26 m", "\u2192 **46.26 m"),
    ("226.67 \ufffd?46.26", "226.67 \u2192 46.26"),
    ("1.0 \ufffd?2.5", "1.0 \u2192 2.5"),
    ("757 \ufffd?724", "757 \u2192 724"),
    ("221.4 \ufffd?221.7", "221.4 \u2192 221.7"),
    ("computeBoltSurface \ufffd?fluxLite", "computeBoltSurface \u2192 fluxLite"),
    ("fluxLite \ufffd?finalizeFlux", "fluxLite \u2192 finalizeFlux"),
    ("finalizeFlux \ufffd?S95", "finalizeFlux \u2192 S95"),
    ("changes \ufffd?the landing point changes", "changes \u2192 the landing point changes"),
    ("\ufffd?**the spot shape", "\u2192 **the spot shape"),
    ("= 1 (a 4.8\u00d7 contraction)", "= 1 (a 4.8\u00d7 contraction)"),
]
DEFAULT = ("\ufffd?", "\u2014 ")                      # everything else is an em dash


def report(text, limit=200):
    n = 0
    for i, ch in enumerate(text):
        if ch == "\ufffd":
            n += 1
            if n <= limit:
                print(f"  @{i}: ...{ascii(text[max(0, i - 30):i + 30])}...")
    return n


def main():
    path = sys.argv[1]
    apply = "--apply" in sys.argv
    text = open(path, "rb").read().decode("utf-8", errors="replace")
    print(f"{path}: {report(text)} broken characters")
    if not apply:
        return 0
    for old, new in RULES:
        text = text.replace(old, new)
    text = text.replace(*DEFAULT)
    left = report(text)
    open(path, "w", encoding="utf-8", newline="").write(text)
    print(f"repaired, {left} broken characters left")
    return 0 if left == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
