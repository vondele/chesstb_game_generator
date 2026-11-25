#pragma once

// Slot-ordered PGN writer.  Out-of-order slots are buffered in memory until
// their predecessor arrives.  Thread-safe.

#include <fstream>
#include <map>
#include <mutex>
#include <string>

class SlotWriter {
public:
    explicit SlotWriter(std::ofstream& out) : out_(out), next_slot_(0) {}

    void write(size_t slot, const std::string& pgn) {
        std::lock_guard<std::mutex> lk(mu_);
        if (slot == next_slot_) {
            out_.write(pgn.data(), static_cast<std::streamsize>(pgn.size()));
            ++next_slot_;
            auto it = reorder_.begin();
            while (it != reorder_.end() && it->first == next_slot_) {
                out_.write(it->second.data(), static_cast<std::streamsize>(it->second.size()));
                ++next_slot_;
                it = reorder_.erase(it);
            }
        } else {
            reorder_[slot] = pgn;
        }
    }

    // Called after all workers have joined to drain any remaining buffered slots.
    void flush() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : reorder_) {
            out_.write(kv.second.data(), static_cast<std::streamsize>(kv.second.size()));
        }
        reorder_.clear();
    }

private:
    std::ofstream& out_;
    size_t next_slot_;
    std::map<size_t, std::string> reorder_;
    std::mutex mu_;
};
