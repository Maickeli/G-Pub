#pragma once

#include "gpub/active_window_info.h"
#include "gpub/rule.h"

#include <optional>
#include <regex>
#include <vector>

namespace gpub {

struct RuleMatch {
    std::string profile_name;
    int priority{0};
    int specificity{0};
    std::size_t rule_order{0};
};

class RulesEngine {
public:
    struct CompiledRule {
        Rule rule;
        std::optional<std::regex> title_pattern;
    };

    void setRules(const std::vector<Rule>& rules);
    std::optional<RuleMatch> match(const ActiveWindowInfo& window) const;

private:
    std::vector<CompiledRule> compiled_rules_;
};

} // namespace gpub
