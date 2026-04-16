// Unified filter config loader: JSONC with legacy .txt fallback.

#include "pso_filters.hpp"
#include "pso_log.hpp"

#include <deps/nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using json = nlohmann::json;

// ============================================================================
// JSONC comment stripper. Removes `// ...` (line) and `/* ... */` (block)
// comments from raw text so that nlohmann's strict JSON parser accepts it.
// ============================================================================

static std::string StripJsoncComments(const std::string &src)
{
    std::string out;
    out.reserve(src.size());
    bool in_string = false;
    for (size_t i = 0; i < src.size(); ++i)
    {
        const char c = src[i];
        if (in_string)
        {
            out += c;
            if (c == '"' && (i == 0 || src[i - 1] != '\\'))
                in_string = false;
            continue;
        }
        if (c == '"')
        {
            in_string = true;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < src.size())
        {
            if (src[i + 1] == '/')
            {
                // Line comment — skip to end of line.
                while (i < src.size() && src[i] != '\n') ++i;
                out += '\n';
                continue;
            }
            if (src[i + 1] == '*')
            {
                // Block comment — skip to closing */.
                i += 2;
                while (i + 1 < src.size() &&
                       !(src[i] == '*' && src[i + 1] == '/'))
                    ++i;
                if (i + 1 < src.size()) ++i; // skip the '/'
                continue;
            }
        }
        out += c;
    }
    return out;
}

// ============================================================================
// Legacy .txt loader — one ID per line, # comments, hex or decimal.
// ============================================================================

static void LoadTxtIds(const std::string &path,
                       std::unordered_set<uint32_t> &out,
                       const char *label)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
    {
        PSO_LOG("LoadFilters: %s not found at %s", label, path.c_str());
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f))
    {
        // Strip \r\n.
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;
        // Parse hex (6-digit) or decimal.
        uint32_t id = 0;
        if (std::strlen(p) >= 6 &&
            std::sscanf(p, "%6x", &id) == 1)
        {
            out.insert(id);
        }
        else if (std::sscanf(p, "%u", &id) == 1)
        {
            out.insert(id);
        }
    }
    std::fclose(f);
    PSO_LOG("LoadFilters: %s loaded %zu IDs from %s",
            label, out.size(), path.c_str());
}

// ============================================================================
// JSONC loader.
// ============================================================================

static uint32_t ParseIdEntry(const json &entry)
{
    if (entry.is_string())
    {
        const std::string &s = entry.get_ref<const std::string &>();
        return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16));
    }
    if (entry.is_object() && entry.contains("id"))
    {
        const json &id = entry["id"];
        if (id.is_string())
        {
            const std::string &s = id.get_ref<const std::string &>();
            return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16));
        }
        if (id.is_number_unsigned())
            return id.get<uint32_t>();
        if (id.is_number_integer())
            return static_cast<uint32_t>(id.get<int>());
    }
    return 0xFFFFFFFF;
}

static bool LoadJsonc(const std::string &path, FilterSets &out)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;

    // Read the whole file.
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024)
    {
        std::fclose(f);
        return false;
    }
    std::string raw(static_cast<size_t>(len), '\0');
    std::fread(&raw[0], 1, static_cast<size_t>(len), f);
    std::fclose(f);

    // Strip JSONC comments.
    const std::string clean = StripJsoncComments(raw);

    // Parse.
    json doc;
    try
    {
        doc = json::parse(clean);
    }
    catch (const json::parse_error &e)
    {
        out.error_msg = std::string("JSONC parse error: ") + e.what();
        PSO_LOG("LoadFilters: %s", out.error_msg.c_str());
        return false;
    }

    // Extract arrays.
    if (doc.contains("rares") && doc["rares"].is_array())
    {
        for (const auto &entry : doc["rares"])
        {
            const uint32_t id = ParseIdEntry(entry);
            if (id != 0xFFFFFFFF) out.rare_ids.insert(id);
        }
    }
    if (doc.contains("hidden_items") && doc["hidden_items"].is_array())
    {
        for (const auto &entry : doc["hidden_items"])
        {
            const uint32_t id = ParseIdEntry(entry);
            if (id != 0xFFFFFFFF) out.hidden_item_ids.insert(id);
        }
    }
    if (doc.contains("hidden_monsters") && doc["hidden_monsters"].is_array())
    {
        for (const auto &entry : doc["hidden_monsters"])
        {
            const uint32_t id = ParseIdEntry(entry);
            if (id != 0xFFFFFFFF) out.hidden_monster_ids.insert(id);
        }
    }

    out.from_jsonc = true;
    PSO_LOG("LoadFilters: JSONC loaded (rares=%zu hidden_items=%zu hidden_monsters=%zu)",
            out.rare_ids.size(), out.hidden_item_ids.size(),
            out.hidden_monster_ids.size());
    return true;
}

// ============================================================================
// Public API.
// ============================================================================

FilterSets LoadFilters(const std::string &addon_dir)
{
    FilterSets sets{};

    // Try JSONC first.
    const std::string jsonc_path = addon_dir + "pixelated_mods_filters.jsonc";
    if (LoadJsonc(jsonc_path, sets))
        return sets;

    // Fallback: legacy .txt files.
    if (!sets.error_msg.empty())
        PSO_LOG("LoadFilters: JSONC failed, falling back to .txt files");

    LoadTxtIds(addon_dir + "pixelated_mods_rares.txt",
               sets.rare_ids, "rares");
    LoadTxtIds(addon_dir + "pixelated_mods_hidden.txt",
               sets.hidden_item_ids, "hidden_items");
    LoadTxtIds(addon_dir + "pixelated_mods_monster_hidden.txt",
               sets.hidden_monster_ids, "hidden_monsters");
    return sets;
}
