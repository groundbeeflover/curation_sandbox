// SPDX-License-Identifier: Apache-2.0

#include <cs/doc/history.hpp>
#include <cs/doc/project_io.hpp>

#include <stdexcept>

namespace cs::doc {

namespace {

template <class Key, class T>
void apply_to(std::map<Key, T>& values, const std::map<Key, detail::Edit<T>>& edits, bool forward) {
    for (const auto& [id, edit] : edits) {
        if (const std::optional<T>& value = forward ? edit.after : edit.before) {
            values.insert_or_assign(id, *value);
        } else {
            values.erase(id);
        }
    }
}

// Leaves out the edits that change nothing, so that a record holds only what its change list
// names.
template <class Key, class T> void drop_unchanged(std::map<Key, detail::Edit<T>>& edits) {
    std::erase_if(edits, [](const auto& entry) { return !entry.second.changed(); });
}

} // namespace

void History::apply(const detail::Edits& edits, bool forward) {
    DocumentData& data = document_.data_;
    if (edits.settings) {
        data.settings = forward ? *edits.settings->after : *edits.settings->before;
    }
    apply_to(data.library.fixture_types, edits.fixture_types, forward);
    apply_to(data.works, edits.works, forward);
    apply_to(data.entities, edits.entities, forward);
}

ChangeList History::commit(Transaction transaction) {
    if (transaction.document_ != &document_ || transaction.revision_ != document_.revision_) {
        throw std::invalid_argument("the transaction \"" + transaction.name() +
                                    "\" was made for another document, or an older revision of "
                                    "this one");
    }
    detail::Edits& edits = transaction.edits_;
    if (edits.settings && !edits.settings->changed()) {
        edits.settings.reset();
    }
    drop_unchanged(edits.fixture_types);
    drop_unchanged(edits.works);
    drop_unchanged(edits.entities);
    ChangeList changes = detail::changes_of(edits, true);
    if (changes.empty()) {
        return changes;
    }
    // Applied in place and checked whole; a refused (or failed) commit puts the before values
    // back, which leaves the document's data as it was.
    apply(edits, true);
    try {
        check_references(document_.data_);
    } catch (...) {
        apply(edits, false);
        throw;
    }
    ++document_.revision_;
    redo_.clear();
    undo_.push_back({std::move(transaction.name_), std::move(edits)});
    return changes;
}

std::optional<std::string_view> History::undo_name() const {
    return undo_.empty() ? std::nullopt : std::optional<std::string_view>(undo_.back().name);
}

std::optional<std::string_view> History::redo_name() const {
    return redo_.empty() ? std::nullopt : std::optional<std::string_view>(redo_.back().name);
}

ChangeList History::undo() {
    if (undo_.empty()) {
        throw std::logic_error("there is nothing to undo");
    }
    return step(undo_, redo_, false);
}

ChangeList History::redo() {
    if (redo_.empty()) {
        throw std::logic_error("there is nothing to redo");
    }
    return step(redo_, undo_, true);
}

ChangeList History::step(std::vector<Record>& from, std::vector<Record>& to, bool forward) {
    to.push_back(std::move(from.back()));
    from.pop_back();
    apply(to.back().edits, forward);
    ++document_.revision_;
    return detail::changes_of(to.back().edits, forward);
}

} // namespace cs::doc
