#include "command_stack.h"
#include <algorithm>

namespace eod::editor {

CommandStack::CommandStack(size_t max_memory_bytes, size_t max_history)
    : max_memory_bytes_(max_memory_bytes), max_history_(max_history) {}

void CommandStack::PushAndExecute(std::unique_ptr<IEditorCommand> cmd) {
    if (!cmd) return;

    cmd->Execute();
    PushExecuted(std::move(cmd));
}

void CommandStack::PushExecuted(std::unique_ptr<IEditorCommand> cmd) {
    if (!cmd) return;

    if (IsInTransaction()) {
        active_transaction_->AddCommand(std::move(cmd));
        return;
    }

    // Try merging with last command if possible and at head
    if (current_index_ > 0 && current_index_ == history_.size()) {
        IEditorCommand* prev = history_[current_index_ - 1].get();
        if (prev && prev->MergeWith(cmd.get())) {
            // Updated in place, update memory footprint
            EnforceBudget();
            return;
        }
    }

    PruneRedoHistory();

    current_memory_bytes_ += cmd->GetMemoryFootprint();
    history_.push_back(std::move(cmd));
    current_index_ = history_.size();

    EnforceBudget();
}

void CommandStack::BeginTransaction(const std::string& name) {
    if (transaction_depth_ == 0) {
        active_transaction_ = std::make_unique<CompositeCommand>(name);
    }
    ++transaction_depth_;
}

void CommandStack::EndTransaction() {
    if (transaction_depth_ <= 0) return;

    --transaction_depth_;
    if (transaction_depth_ == 0 && active_transaction_) {
        if (!active_transaction_->IsEmpty()) {
            std::unique_ptr<IEditorCommand> cmd = std::move(active_transaction_);
            PushExecuted(std::move(cmd));
        }
        active_transaction_.reset();
    }
}

void CommandStack::CancelTransaction() {
    if (transaction_depth_ > 0 && active_transaction_) {
        // Roll back any executed commands inside transaction
        active_transaction_->Undo();
        active_transaction_.reset();
        transaction_depth_ = 0;
    }
}

bool CommandStack::IsInTransaction() const {
    return transaction_depth_ > 0;
}

bool CommandStack::CanUndo() const {
    return current_index_ > 0 && !IsInTransaction();
}

bool CommandStack::CanRedo() const {
    return current_index_ < history_.size() && !IsInTransaction();
}

void CommandStack::Undo() {
    if (!CanUndo()) return;

    --current_index_;
    history_[current_index_]->Undo();
}

void CommandStack::Redo() {
    if (!CanRedo()) return;

    history_[current_index_]->Redo();
    ++current_index_;
}

void CommandStack::Clear() {
    history_.clear();
    current_index_ = 0;
    clean_index_ = 0;
    clean_index_valid_ = true;
    current_memory_bytes_ = 0;
    transaction_depth_ = 0;
    active_transaction_.reset();
}

std::string CommandStack::GetUndoDescription() const {
    if (!CanUndo()) return "";
    return history_[current_index_ - 1]->GetDescription();
}

std::string CommandStack::GetRedoDescription() const {
    if (!CanRedo()) return "";
    return history_[current_index_]->GetDescription();
}

size_t CommandStack::GetUndoCount() const {
    return current_index_;
}

size_t CommandStack::GetRedoCount() const {
    return history_.size() - current_index_;
}

size_t CommandStack::GetTotalHistoryCount() const {
    return history_.size();
}

size_t CommandStack::GetCurrentMemoryUsage() const {
    return current_memory_bytes_;
}

void CommandStack::SetMaxMemoryBudget(size_t bytes) {
    max_memory_bytes_ = bytes;
    EnforceBudget();
}

bool CommandStack::IsDirty() const {
    if (!clean_index_valid_) return true;
    return current_index_ != clean_index_;
}

void CommandStack::MarkClean() {
    clean_index_ = current_index_;
    clean_index_valid_ = true;
}

void CommandStack::PruneRedoHistory() {
    if (current_index_ < history_.size()) {
        for (size_t i = current_index_; i < history_.size(); ++i) {
            current_memory_bytes_ -= history_[i]->GetMemoryFootprint();
        }
        if (clean_index_ > current_index_) {
            clean_index_valid_ = false;
        }
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(current_index_), history_.end());
    }
}

void CommandStack::EnforceBudget() {
    // Evict oldest commands from the beginning if exceeding count or memory budget
    while ((history_.size() > max_history_ || current_memory_bytes_ > max_memory_bytes_) && !history_.empty()) {
        size_t oldest_footprint = history_.front()->GetMemoryFootprint();
        current_memory_bytes_ = (current_memory_bytes_ >= oldest_footprint)
                                    ? current_memory_bytes_ - oldest_footprint
                                    : 0;

        history_.erase(history_.begin());

        if (current_index_ > 0) {
            --current_index_;
        }
        if (clean_index_valid_) {
            if (clean_index_ > 0) {
                --clean_index_;
            } else {
                clean_index_valid_ = false; // The clean point was evicted
            }
        }
    }
}

} // namespace eod::editor
