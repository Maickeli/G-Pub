#include "gpub/rules_engine.h"
#include "gpub/text_util.h"

#include <regex>

namespace gpub {

namespace {

bool matchesRule(const RulesEngine::CompiledRule& compiled, const ActiveWindowInfo& window, int* specificity_out) {
    const Rule& rule = compiled.rule;
    int specificity = 0;

    if (rule.executable_path_equals.has_value()) {
        if (normalizePath(window.executable_path) != normalizePath(*rule.executable_path_equals)) {
            return false;
        }
        specificity += 3;
    }

    if (rule.process_name_equals.has_value()) {
        if (toLowerAscii(window.process_name) != toLowerAscii(*rule.process_name_equals)) {
            return false;
        }
        specificity += 2;
    }

    if (rule.window_title_regex.has_value()) {
        if (!compiled.title_pattern.has_value()) {
            return false;
        }
        if (!std::regex_search(window.window_title, *compiled.title_pattern)) {
            return false;
        }
        specificity += 1;
    }

    *specificity_out = specificity;
    return true;
}

} // namespace

void RulesEngine::setRules(const std::vector<Rule>& rules) {
    compiled_rules_.clear();
    compiled_rules_.reserve(rules.size());

    for (const Rule& rule : rules) {
        CompiledRule compiled{};
        compiled.rule = rule;
        if (rule.window_title_regex.has_value()) {
            try {
                compiled.title_pattern.emplace(*rule.window_title_regex, std::regex::ECMAScript | std::regex::icase);
            } catch (...) {
                compiled.title_pattern.reset();
            }
        }
        compiled_rules_.push_back(std::move(compiled));
    }
}

std::optional<RuleMatch> RulesEngine::match(const ActiveWindowInfo& window) const {
    std::optional<RuleMatch> best;

    for (const CompiledRule& compiled : compiled_rules_) {
        const Rule& rule = compiled.rule;
        int specificity = 0;
        if (!matchesRule(compiled, window, &specificity)) {
            continue;
        }

        RuleMatch current{};
        current.profile_name = rule.profile_name;
        current.priority = rule.priority;
        current.specificity = specificity;
        current.rule_order = rule.order;

        if (!best.has_value()) {
            best = current;
            continue;
        }

        if (current.priority > best->priority) {
            best = current;
            continue;
        }
        if (current.priority == best->priority && current.specificity > best->specificity) {
            best = current;
            continue;
        }
        if (current.priority == best->priority &&
            current.specificity == best->specificity &&
            current.rule_order < best->rule_order) {
            best = current;
            continue;
        }
    }

    return best;
}

} // namespace gpub
