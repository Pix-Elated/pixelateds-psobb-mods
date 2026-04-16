#pragma once

#include <string>
#include <unordered_set>

// Unified filter config loader. Reads `pixelated_mods_filters.jsonc`
// if present, otherwise falls back to the three legacy .txt files.
// Call once at startup and again on the "Reload filters" button.

struct FilterSets
{
    std::unordered_set<uint32_t> rare_ids;
    std::unordered_set<uint32_t> hidden_item_ids;
    std::unordered_set<uint32_t> hidden_monster_ids;
    bool from_jsonc;         // true if the JSONC was loaded
    std::string error_msg;   // non-empty on parse failure
};

FilterSets LoadFilters(const std::string &addon_dir);
