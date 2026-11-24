// SPDX-License-Identifier: Apache-2.0

#pragma once

// A work's name from the image file it was imported from (ADR 0008): images are stored under their
// SHA-256 (ADR 0007), so the name a curator knows a work by is its title, made from the file's
// name, and its source, which keeps the file's name as it was.
//
// A file name comes from outside and is hostile (CLAUDE.md): any bytes, of any length. Both
// functions return valid UTF-8 of at most doc::max_string_bytes, which the project file can hold:
// each maximal run of bytes that does not begin a valid UTF-8 sequence, as the Unicode Standard's
// "maximal subpart" practice (and WHATWG's decoder) counts them, becomes one U+FFFD; overlong
// forms, surrogates (U+D800 to U+DFFF) and code points above U+10FFFF are not valid. The cut is
// at a code point boundary, so no sequence is split.

#include <cs/doc/work.hpp>

#include <string>
#include <string_view>

namespace cs::editor {

// The title of a work imported from the file `file_name` (a name, not a path): its stem, the part
// before the last '.' (all of it when there is none, nothing for ".png"), with each '_' made a
// space, made valid UTF-8 (above), each run of ASCII white space (space, tab, line feed, vertical
// tab, form feed, carriage return) collapsed to one space and trimmed at both ends, then cut to
// at most doc::max_string_bytes and trimmed again. "Monet_Haystacks  final.JPG" gives
// "Monet Haystacks final"; ".png" gives "", an untitled work.
std::string title_from_file_name(std::string_view file_name);

// The source of a work imported from the file `file_name`: FromImport with the name made valid
// UTF-8 and cut to at most doc::max_string_bytes (above), and otherwise as it was.
doc::FromImport source_from_file_name(std::string_view file_name);

} // namespace cs::editor
