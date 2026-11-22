// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace orbit
{
// Remember identities, not row indices: live refreshes can remove earlier rows.
class QueuePosition
{
  public:
    int restore(const std::string &scope, const std::vector<std::string> &keys) const
    {
        if (scope != scope_ || selected_.empty()) return -1;
        const auto find = [&](const std::string &key) {
            const auto at = std::find(keys.begin(), keys.end(), key);
            return at == keys.end() ? -1 : static_cast<int>(at - keys.begin());
        };
        if (const int at = find(selected_); at >= 0) return at;
        const auto old = std::find(keys_.begin(), keys_.end(), selected_);
        if (old != keys_.end())
        {
            for (auto it = old + 1; it != keys_.end(); ++it)
                if (const int at = find(*it); at >= 0) return at;
            for (auto it = old; it != keys_.begin();)
                if (const int at = find(*--it); at >= 0) return at;
        }
        return keys.empty() ? -1 : 0;
    }
    void remember(std::string scope, std::vector<std::string> keys, int index)
    {
        scope_ = std::move(scope);
        keys_ = std::move(keys);
        selected_ = index >= 0 && index < static_cast<int>(keys_.size()) ? keys_[index] : std::string();
    }
    void clear() { selected_.clear(); }
  private:
    std::string scope_, selected_;
    std::vector<std::string> keys_;
};
}
