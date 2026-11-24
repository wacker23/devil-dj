#pragma once

// The pre-tokenizer (P5-15, CODEBEAT_SPEC §8.3): before evaluation, every string literal in argument position -
// "...", '...' and backtick literals without ${} - is rewritten to __mini("...", <offset>), where <offset> is where
// its text starts in the editor buffer (in characters, as the editor counts them). The mini-notation parser adds
// it to every atom's place, so sounding atoms can be lit in the code.
//
// Left as they are: comments, object keys ({ "a": 1 }), template literals with ${...}. No line is added or
// removed, so line numbers stay right; columns on lines with rewritten strings move, and originalColumn() maps an
// error's column back to the owner's text.

#include <string>
#include <string_view>
#include <vector>

namespace devildj::codebeat
{

struct PreTokenized
{
    std::string code; // the rewritten code (UTF-8)

    struct Insertion
    {
        int line = 1;       // 1-based
        int column = 1;     // where, in the rewritten line (1-based, in characters)
        int characters = 0; // how many characters were added there
    };
    std::vector<Insertion> insertions;

    // A column (1-based, characters) in the rewritten code -> the same place in the original line.
    int originalColumn(int line, int column) const;
};

PreTokenized preTokenize(std::string_view code);

// The 1-based line and column of a character offset in `code` (UTF-8; offsets count characters).
std::pair<int, int> lineColumnOf(std::string_view code, int characterOffset);

} // namespace devildj::codebeat
