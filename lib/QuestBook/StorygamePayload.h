#pragma once

#include <cstddef>

#include "StorygameModel.h"

// Parses the converted storygame payload XML into the shapes StorygameModel.h
// declares. The grammar is docs/storygames/ARCHITECTURE.md §4.6; the on-SD
// bytes this reads are written by StorygameImporter (game.dat payloads start
// with their own `<p>` root, function payloads are a bare sequence of `<k>`
// elements with no root).
namespace questbook_game {

// Parses one `<p>` element into `out`. `xml` must point at the whole payload
// buffer and `length` is its byte length; expat needs no NUL terminator.
// Returns false when the input is not a single well-formed `<p>` element.
bool parseParagraph(const char* xml, size_t length, Paragraph& out);

// Parses a task-list fragment (the children of an `<fn>` element) into `out`.
// The fragment has no single root element, so this wraps it in a synthetic one
// before handing it to expat.
bool parseTaskList(const char* xml, size_t length, TaskList& out);

}  // namespace questbook_game