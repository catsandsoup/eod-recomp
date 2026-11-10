#pragma once

#include "undo_command.h"
#include <vector>
#include <memory>
#include <string>
#include <cstddef>

namespace eod::editor {

class CommandStack {
public:
    explicit CommandStack(size_t max_memory_bytes = 256 * 1024 * 1024, size_t max_history = 256);
    ~CommandStack() = default;

    // Disallow copy, allow move
    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;
    CommandStack(CommandStack&&) noexcept = default;
    CommandStack& operator=(CommandStack&&) noexcept = default;

    // Execute and record command, pruning any redo history
    void PushAndExecute(std::unique_ptr<IEditorCommand> cmd);

    // Record an already executed command
    void PushExecuted(std::unique_ptr<IEditorCommand> cmd);

    // Transaction batching
    void BeginTransaction(const std::string& name);
    void EndTransaction();
    void CancelTransaction();
    bool IsInTransaction() const;

    // Undo / Redo controls
    bool CanUndo() const;
    bool CanRedo() const;
    void Undo();
    void Redo();

    // Clear entire history
    void Clear();

    // Inspection
    std::string GetUndoDescription() const;
    std::string GetRedoDescription() const;
    size_t GetUndoCount() const;
    size_t GetRedoCount() const;
    size_t GetTotalHistoryCount() const;
    size_t GetCurrentMemoryUsage() const;
    size_t GetMaxMemoryBudget() const { return max_memory_bytes_; }
    void SetMaxMemoryBudget(size_t bytes);

    // Dirty state tracking (relative to last save)
    bool IsDirty() const;
    void MarkClean();

private:
    void PruneRedoHistory();
    void EnforceBudget();

    std::vector<std::unique_ptr<IEditorCommand>> history_;
    size_t current_index_ = 0;   // Index of the NEXT command to execute (points just past last executed command)
    size_t clean_index_ = 0;     // Value of current_index_ when last marked clean
    bool clean_index_valid_ = true;

    size_t max_memory_bytes_;
    size_t max_history_;
    size_t current_memory_bytes_ = 0;

    // Transaction state
    int transaction_depth_ = 0;
    std::unique_ptr<CompositeCommand> active_transaction_;
};

} // namespace eod::editor
