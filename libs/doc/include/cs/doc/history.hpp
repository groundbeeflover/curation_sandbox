// SPDX-License-Identifier: Apache-2.0

#pragma once

// The open document and its undo and redo stacks (ADR 0007). Committing a transaction applies it
// and puts it on the undo stack; undo applies its before values and moves it to the redo stack;
// redo applies its after values and moves it back. Selection, camera and view state are not part
// of the document and are not undone.

#include <cs/doc/document.hpp>
#include <cs/doc/transaction.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cs::doc {

class History {
  public:
    // An empty project with the default settings and nothing to undo.
    History() = default;
    explicit History(Document document) : document_(std::move(document)) {}

    // History owns the document; transactions point at it, so it neither copies nor moves.
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    const Document& document() const { return document_; }

    // Applies the transaction to the document, all of it at once, and returns what changed.
    // - It must have been made for this document at its current revision; otherwise throws
    //   std::invalid_argument.
    // - When the result would hold a reference check_references (project_io.hpp) refuses, such as
    //   a placement on a wall the transaction removes, throws that ReferenceError, naming the
    //   entity and the reference; the document and both stacks are left as they were.
    // - A transaction that changes nothing is not recorded: it returns an empty ChangeList and
    //   leaves the stacks as they were.
    // - Otherwise the transaction goes on the undo stack, the redo stack is cleared, and the
    //   document's revision goes up by one. Refused and empty commits leave the revision too.
    ChangeList commit(Transaction transaction);

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::size_t undo_count() const { return undo_.size(); }
    std::size_t redo_count() const { return redo_.size(); }

    // The names of the transactions undo() and redo() would apply; nullopt when there is none.
    std::optional<std::string_view> undo_name() const;
    std::optional<std::string_view> redo_name() const;

    // Undoes the last committed (or redone) transaction, or redoes the last undone one, and
    // returns what changed. Throws std::logic_error when there is none.
    ChangeList undo();
    ChangeList redo();

  private:
    struct Record {
        std::string name;
        detail::Edits edits;
    };

    Document document_;
    std::vector<Record> undo_; // the last committed at the back
    std::vector<Record> redo_; // the last undone at the back

    // Writes the edits' after (forward) or before values into the document.
    void apply(const detail::Edits& edits, bool forward);
};

} // namespace cs::doc
