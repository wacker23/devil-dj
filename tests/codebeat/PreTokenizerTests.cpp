// P5-15: the pre-tokenizer - string literals in argument position rewritten with their place in the buffer.

#include "codebeat/PreTokenizer.h"

#include <catch2/catch_test_macros.hpp>

using devildj::codebeat::lineColumnOf;
using devildj::codebeat::preTokenize;

TEST_CASE("Pre-tokenizer: strings in argument position carry their offset", "[codebeat][pretokenize]")
{
    // s("kick snare hat*2"): the text starts at character 3.
    CHECK(preTokenize(R"(s("kick snare hat*2"))").code == R"(s(__mini("kick snare hat*2", 3)))");
    CHECK(preTokenize("stack(s('bd'), s(`hh*8`))").code == "stack(s(__mini('bd', 9)), s(__mini(`hh*8`, 18)))");
    // Arguments after a comma and in arrays.
    CHECK(preTokenize(R"(play("drums", s("bd")))").code == R"(play(__mini("drums", 6), s(__mini("bd", 17))))");
    CHECK(preTokenize(R"(seq(["a", "b"]))").code == R"(seq([__mini("a", 6), __mini("b", 11)]))");
    // Escapes stay as written; the offset is the opening quote's + 1.
    CHECK(preTokenize(R"(s("a\"b"))").code == R"(s(__mini("a\"b", 3)))");
}

TEST_CASE("Pre-tokenizer: what is left alone", "[codebeat][pretokenize]")
{
    // Not in argument position: assignments, comparisons, a bare completion value, case labels.
    CHECK(preTokenize(R"(const p = "bd sd"; x === "a"; "bd ~ sd")").code == R"(const p = "bd sd"; x === "a"; "bd ~ sd")");
    CHECK(preTokenize("switch (x) { case \"a\": break; }").code == "switch (x) { case \"a\": break; }");
    // Object keys, also after a comma.
    CHECK(preTokenize(R"(f({ "a": 1, "b": 2 }))").code == R"(f({ "a": 1, "b": 2 }))");
    CHECK(preTokenize(R"(f(1, "k": 2))").code == R"(f(1, "k": 2))");
    // Templates with ${}, comments, unclosed strings.
    CHECK(preTokenize("s(`bd*${n}`)").code == "s(`bd*${n}`)");
    CHECK(preTokenize("// s(\"bd\")\ns(\"hh\")").code == "// s(\"bd\")\ns(__mini(\"hh\", 14))");
    CHECK(preTokenize("/* s(\"bd\") */ s(\"hh\")").code == "/* s(\"bd\") */ s(__mini(\"hh\", 17))");
    CHECK(preTokenize("s(\"bd\n)").code == "s(\"bd\n)");
}

TEST_CASE("Pre-tokenizer: offsets count characters (as the editor does); lines stay; columns map back", "[codebeat][pretokenize]")
{
    // "é" is two bytes but one character.
    const auto r = preTokenize("// caf\xc3\xa9\ns(\"bd\")");
    CHECK(r.code == "// caf\xc3\xa9\ns(__mini(\"bd\", 11))");
    // A multi-line template keeps its newlines; no line is added.
    const auto t = preTokenize("s(`bd\nsd`)\nx");
    CHECK(t.code == "s(__mini(`bd\nsd`, 3))\nx");
    // Columns: after the 7 characters of "__mini(" on line 2, column 12 of the rewritten line is column 5.
    const auto m = preTokenize("x\ns(\"bd\").nope()");
    REQUIRE(m.code == "x\ns(__mini(\"bd\", 5)).nope()");
    CHECK(m.originalColumn(2, 1) == 1);
    CHECK(m.originalColumn(2, 10) == 3);  // the quote: after "__mini("
    CHECK(m.originalColumn(2, 19) == 8);  // ".nope": after ", 5)" too
    CHECK(m.originalColumn(1, 1) == 1);
    // Character offsets -> line / column.
    CHECK(lineColumnOf("ab\ncd", 0) == std::pair{1, 1});
    CHECK(lineColumnOf("ab\ncd", 4) == std::pair{2, 2});
    CHECK(lineColumnOf("\xc3\xa9x\ny", 3) == std::pair{2, 1});
}
