#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <utility>

namespace eod::editor {

class IEditorCommand {
public:
    virtual ~IEditorCommand() = default;
    virtual void Execute() = 0;
    virtual void Undo() = 0;
    virtual void Redo() { Execute(); }
    virtual std::string GetDescription() const = 0;
    virtual size_t GetMemoryFootprint() const { return sizeof(*this); }
    virtual bool MergeWith(const IEditorCommand* /*other*/) { return false; }
};

// Generic property change command with value restoration
template <typename T>
class PropertyChangeCommand : public IEditorCommand {
public:
    PropertyChangeCommand(std::string description, T* target, T old_value, T new_value,
                          std::function<void()> on_change = nullptr, bool allow_merge = false)
        : description_(std::move(description)),
          target_(target),
          old_value_(std::move(old_value)),
          new_value_(std::move(new_value)),
          on_change_(std::move(on_change)),
          allow_merge_(allow_merge) {}

    void Execute() override {
        if (target_) {
            *target_ = new_value_;
            if (on_change_) on_change_();
        }
    }

    void Undo() override {
        if (target_) {
            *target_ = old_value_;
            if (on_change_) on_change_();
        }
    }

    std::string GetDescription() const override { return description_; }

    size_t GetMemoryFootprint() const override {
        return sizeof(*this) + description_.capacity() + sizeof(T) * 2;
    }

    bool MergeWith(const IEditorCommand* other) override {
        if (!allow_merge_) return false;
        const auto* prop_other = dynamic_cast<const PropertyChangeCommand<T>*>(other);
        if (prop_other && prop_other->target_ == target_) {
            new_value_ = prop_other->new_value_;
            return true;
        }
        return false;
    }

    void SetAllowMerge(bool allow) { allow_merge_ = allow; }

private:
    std::string description_;
    T* target_ = nullptr;
    T old_value_;
    T new_value_;
    std::function<void()> on_change_;
    bool allow_merge_ = false;
};

// Lambda-based command for quick procedural actions
class LambdaCommand : public IEditorCommand {
public:
    LambdaCommand(std::string description,
                  std::function<void()> do_action,
                  std::function<void()> undo_action,
                  size_t memory_footprint = sizeof(LambdaCommand))
        : description_(std::move(description)),
          do_action_(std::move(do_action)),
          undo_action_(std::move(undo_action)),
          memory_footprint_(memory_footprint) {}

    void Execute() override {
        if (do_action_) do_action_();
    }

    void Undo() override {
        if (undo_action_) undo_action_();
    }

    std::string GetDescription() const override { return description_; }
    size_t GetMemoryFootprint() const override { return memory_footprint_; }

private:
    std::string description_;
    std::function<void()> do_action_;
    std::function<void()> undo_action_;
    size_t memory_footprint_ = sizeof(LambdaCommand);
};

// Composite command aggregating multiple commands into an atomic transaction
class CompositeCommand : public IEditorCommand {
public:
    explicit CompositeCommand(std::string description)
        : description_(std::move(description)) {}

    void AddCommand(std::unique_ptr<IEditorCommand> cmd) {
        if (cmd) {
            commands_.push_back(std::move(cmd));
        }
    }

    void Execute() override {
        for (auto& cmd : commands_) {
            cmd->Execute();
        }
    }

    void Undo() override {
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
            (*it)->Undo();
        }
    }

    void Redo() override {
        for (auto& cmd : commands_) {
            cmd->Redo();
        }
    }

    std::string GetDescription() const override { return description_; }

    size_t GetMemoryFootprint() const override {
        size_t total = sizeof(*this) + description_.capacity();
        for (const auto& cmd : commands_) {
            total += cmd->GetMemoryFootprint();
        }
        return total;
    }

    const std::vector<std::unique_ptr<IEditorCommand>>& GetCommands() const {
        return commands_;
    }

    bool IsEmpty() const { return commands_.empty(); }

private:
    std::string description_;
    std::vector<std::unique_ptr<IEditorCommand>> commands_;
};

} // namespace eod::editor
