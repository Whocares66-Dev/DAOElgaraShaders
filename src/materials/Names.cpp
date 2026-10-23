// The names' order of asking (by_name), a material object's reading and
// settle, from the Python tool they were written in first (2026-10-12; ported
// and checked against it over every material, then it was taken out,
// 2026-10-15). The few patterns that are the code's own and not a table's (a
// level of detail's mark, a diffuse texture's stem, a project's name, a
// tileset's sheet) are written here.
#include "materials/Names.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <memory>
#include <sstream>
#include <utility>

namespace materials
{

namespace
{

constexpr std::array<std::string_view, 6> kSolid = {"static.mat", "prop.mat",  "character.mat",
                                                    "weapon.mat", "cloth.mat", "canopy.mat"};

// The share of a material's used texels found metal from which it is mixed,
// and all metal (SHARE_MIXED, SHARE_METAL). Under the first it is what else
// its family is made of: a material whose name says it has metal in it and in
// which the rule finds next to none.
constexpr double kShareMixed = 0.02;
constexpr double kShareMetal = 0.85;

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> SplitOn(std::string_view text, char separator)
{
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size())
    {
        const std::size_t end = text.find(separator, at);
        out.emplace_back(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (end == std::string_view::npos)
        {
            break;
        }
        at = end + 1;
    }
    return out;
}

// A table row's words, each a pattern, as one pattern joined by |.
std::string OnePattern(std::string_view words)
{
    std::string out;
    for (const auto &word : SplitOn(words, ' '))
    {
        if (!word.empty())
        {
            out += out.empty() ? word : "|" + word;
        }
    }
    return out;
}

// The value of an attribute after `key` in `text`: key="value".
std::optional<std::string_view> After(std::string_view text, std::string_view key, std::size_t from = 0)
{
    const std::size_t at = text.find(key, from);
    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::size_t start = at + key.size();
    const std::size_t end = text.find('"', start);
    if (end == std::string_view::npos)
    {
        return std::nullopt;
    }
    return text.substr(start, end - start);
}

} // namespace

// A compiled pattern, searched or matched at the start.
struct Names::Pattern
{
    pcre2_code *code = nullptr;

    explicit Pattern(std::string_view source, std::string &error)
    {
        int number = 0;
        PCRE2_SIZE offset = 0;
        code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(source.data()), source.size(), 0, &number, &offset, nullptr);
        if (code == nullptr)
        {
            std::array<PCRE2_UCHAR, 256> message{};
            pcre2_get_error_message(number, message.data(), message.size());
            error = std::string(reinterpret_cast<const char *>(message.data())) + " at " + std::to_string(offset) +
                    " in " + std::string(source);
        }
    }
    ~Pattern()
    {
        pcre2_code_free(code);
    }
    Pattern(const Pattern &) = delete;
    Pattern &operator=(const Pattern &) = delete;
    Pattern(Pattern &&) = delete;
    Pattern &operator=(Pattern &&) = delete;

    // Where it matches in `text`: the first match's start and end.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> Find(std::string_view text,
                                                                          bool atStart = false) const
    {
        pcre2_match_data *data = pcre2_match_data_create_from_pattern(code, nullptr);
        const int found = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), 0,
                                      atStart ? PCRE2_ANCHORED : 0, data, nullptr);
        std::optional<std::pair<std::size_t, std::size_t>> out;
        if (found > 0)
        {
            const PCRE2_SIZE *at = pcre2_get_ovector_pointer(data);
            out = std::pair{static_cast<std::size_t>(at[0]), static_cast<std::size_t>(at[1])};
        }
        pcre2_match_data_free(data);
        return out;
    }

    // A group of the match at the start of `text`, or empty.
    [[nodiscard]] std::string Group(std::string_view text, std::uint32_t group) const
    {
        pcre2_match_data *data = pcre2_match_data_create_from_pattern(code, nullptr);
        const int found =
            pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), 0, PCRE2_ANCHORED, data, nullptr);
        std::string out;
        if (std::cmp_greater(found, group))
        {
            const PCRE2_SIZE *at = pcre2_get_ovector_pointer(data);
            if (at[2 * group] != PCRE2_UNSET)
            {
                out = std::string(text.substr(at[2 * group], at[(2 * group) + 1] - at[2 * group]));
            }
        }
        pcre2_match_data_free(data);
        return out;
    }
};

struct Names::Row
{
    std::string cls;
    std::string fallback;
    std::unique_ptr<Pattern> pattern;
};

bool MaterialObject::Solid() const
{
    return std::ranges::find(kSolid, definition) != kSolid.end();
}

bool MaterialObject::Plate() const
{
    const std::string lower = Lower(project);
    // And the templars' armor, plate built on a robe's body (ROB_TMP; issue 32).
    return lower.find("arm_mas") != std::string::npos || lower.find("arm_hvy") != std::string::npos ||
           lower.find("arm_dwp") != std::string::npos || lower.find("rob_tmp") != std::string::npos;
}

std::optional<MaterialObject> ReadMaterialObject(std::string_view name, std::string_view text)
{
    const auto definition = After(text, "<Material Name=\"");
    if (!definition)
    {
        return std::nullopt;
    }
    MaterialObject out;
    out.name = Lower(name);
    out.definition = Lower(*definition);
    // dc:source=file:///<path> -->, the path with no space in it and a space
    // before the --> or none: the project is the path's last part.
    if (const std::size_t source = text.find("dc:source=file:///"); source != std::string_view::npos)
    {
        const std::size_t start = source + 18;
        const std::size_t comment = text.find("-->", start);
        if (comment != std::string_view::npos)
        {
            std::string_view path = text.substr(start, comment - start);
            if (path.ends_with(' '))
            {
                path.remove_suffix(1);
            }
            if (!path.empty() && path.find_first_of(" \t\r\n") == std::string_view::npos)
            {
                const std::size_t slash = path.rfind('/');
                out.project = std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
            }
        }
    }
    for (std::size_t at = text.find("<Texture Name=\""); at != std::string_view::npos;
         at = text.find("<Texture Name=\"", at + 1))
    {
        const auto slot = After(text, "<Texture Name=\"", at);
        const std::size_t close = text.find('>', at);
        const auto res = After(text.substr(0, close), "ResName=\"", at);
        if (slot && res)
        {
            out.textures.emplace_back(std::string(*slot), Lower(*res));
        }
    }
    if (const std::size_t sound = text.find("Name=\"mml_iSoundMaterialType\""); sound != std::string_view::npos)
    {
        if (const auto value = After(text, "value=\"", sound))
        {
            int number = 0;
            const char *const begin = std::to_address(value->begin());
            const char *const end = std::to_address(value->end());
            if (std::from_chars(begin, end, number).ec == std::errc{})
            {
                out.sound = number;
            }
        }
    }
    return out;
}

Names::Names() = default;
Names::~Names() = default;

bool Names::ParseRow(const std::vector<std::string> &fields, std::string &error)
{
    const auto compiled = [&](const std::string &source) {
        auto pattern = std::make_unique<Pattern>(source, error);
        return pattern->code == nullptr ? nullptr : std::move(pattern);
    };
    if (fields[0] == "table" && fields.size() == 5)
    {
        auto pattern = compiled(OnePattern(fields[4]));
        if (!pattern)
        {
            return false;
        }
        tables_[fields[1]].push_back({fields[2], fields[3] == "-" ? std::string{} : fields[3], std::move(pattern)});
        return true;
    }
    if (fields[0] == "tileset" && fields.size() == 3)
    {
        tilesets_.emplace_back(fields[1], SplitOn(fields[2], ' '));
        return true;
    }
    if (fields[0] == "sound" && fields.size() == 3)
    {
        sounds_[std::stoi(fields[1])] = fields[2];
        return true;
    }
    if (fields[0] == "clear" && fields.size() == 3)
    {
        clear_[fields[1]] = fields[2];
        return true;
    }
    if (fields[0] == "clear_power" && fields.size() == 2)
    {
        clearPower_ = fields[1];
        return true;
    }
    if ((fields[0] == "wielded" || fields[0] == "worn_texture") && fields.size() == 2)
    {
        auto pattern = compiled(fields[1]);
        if (!pattern)
        {
            return false;
        }
        (fields[0] == "wielded" ? wielded_ : wornTexture_) = std::move(pattern);
        return true;
    }
    error = "a line not understood: " + fields[0];
    return false;
}

bool Names::Parse(std::string_view text, std::string &error)
{
    for (const auto &line : SplitOn(text, '\n'))
    {
        std::string_view row = line;
        if (!row.empty() && row.back() == '\r')
        {
            row.remove_suffix(1);
        }
        if (row.empty() || row.front() == ';')
        {
            continue;
        }
        if (!ParseRow(SplitOn(row, '\t'), error))
        {
            return false;
        }
    }
    if (!wielded_ || !wornTexture_)
    {
        error = "no wielded or worn_texture line";
        return false;
    }
    return true;
}

const Names::Row *Names::LookUp(const std::string &table, std::string_view text) const
{
    const auto found = tables_.find(table);
    if (found == tables_.end())
    {
        return nullptr;
    }
    for (const Row &row : found->second)
    {
        if (row.pattern->Find(text))
        {
            return &row;
        }
    }
    return nullptr;
}

Verdict Names::ByThings(const std::string &name, std::string_view text, const std::optional<int> &sound) const
{
    const auto heardOf = [&]() -> std::string {
        if (!sound)
        {
            return {};
        }
        const auto found = sounds_.find(*sound);
        return found == sounds_.end() ? std::string{} : found->second;
    };
    const std::string heard = heardOf();
    if (const Row *row = LookUp("things", text))
    {
        // A wooden or a cloth footstep is too much against a name that could be
        // metal; a stone one is what a tileset's pieces were given whatever
        // they are.
        if (!heard.empty() && heard != "metal" && heard != "stone" && (row->cls == "metal" || row->cls == kPaint))
        {
            return {heard, {}};
        }
        return {row->cls, row->fallback};
    }
    for (const auto &[cls, codes] : tilesets_)
    {
        if (name.size() > 3 && name[3] == '_' && std::ranges::find(codes, name.substr(0, 3)) != codes.end())
        {
            return {cls, {}};
        }
    }
    if (!heard.empty() && heard != "metal")
    {
        return {heard, {}};
    }
    return {"other", {}};
}

Verdict Names::ByName(const MaterialObject &material) const
{
    std::string name = material.name;
    if (name.size() > 3 && name[name.size() - 3] == '_' && name[name.size() - 2] == 'l' &&
        (name.back() == '2' || name.back() == '3'))
    {
        name.resize(name.size() - 3);
    }
    const std::string diffuse = material.Texture("mml_tDiffuse");
    std::string error;
    static const Pattern stemTail(R"((_0?d|_d|_diff|_dif|d)?(_?l[23])?\.dds$)", error);
    std::string stem = diffuse;
    if (const auto tail = stemTail.Find(stem))
    {
        stem.resize(tail->first);
    }
    static const Pattern readOnly(R"(readonly-\d+-rev-\d+-)", error);
    std::string project = Lower(material.project);
    if (const auto head = readOnly.Find(project, true))
    {
        project.erase(0, head->second);
    }
    if (project.ends_with(".matproj"))
    {
        project.resize(project.size() - 8);
    }
    const std::string text = name + " " + stem + " " + project;
    const std::string &definition = material.definition;

    if (definition == "canopy.mat")
    {
        return {"foliage", {}};
    }
    if (definition == "cloth.mat")
    {
        return {name.find("default") != std::string::npos ? "other" : "cloth", {}};
    }
    if (definition == "weapon.mat")
    {
        const Row *row = LookUp("weapons", text);
        return row != nullptr ? Verdict{row->cls, row->fallback} : Verdict{"other", {}};
    }
    if (definition == "character.mat")
    {
        const Row *row = LookUp("worn", text);
        if (row == nullptr)
        {
            row = LookUp("creatures", text);
        }
        return row != nullptr ? Verdict{row->cls, row->fallback} : Verdict{"other", {}};
    }
    return ByLevel(name, stem, text, material);
}

// A level's or a prop's: by its tileset's sheet, the weapon's or the suit's texture it
// wears, then the things' table.
Verdict Names::ByLevel(const std::string &name, const std::string &stem, std::string_view text,
                       const MaterialObject &material) const
{
    std::string error;
    if (material.Texture("mml_tDiffuse").empty())
    {
        return {"other", {}};
    }
    static const Pattern master(R"(([a-z]{3})_(test)?master|([a-z]{3})_mastersheet)", error);
    std::string code = master.Group(name, 1);
    if (code.empty())
    {
        code = master.Group(name, 3);
    }
    if (!code.empty())
    {
        for (const auto &[cls, codes] : tilesets_)
        {
            if (std::ranges::find(codes, code) != codes.end())
            {
                return {cls, {}};
            }
        }
    }
    const Row *worn = nullptr;
    if (wielded_->Find(stem, true))
    {
        worn = LookUp("weapons", stem);
    }
    else if (wornTexture_->Find(stem, true))
    {
        worn = LookUp("worn", stem);
    }
    if (worn != nullptr)
    {
        return {worn->cls, worn->fallback};
    }
    return ByThings(name, text, material.sound);
}

std::optional<std::pair<std::string, std::string>> Names::Clear(const std::string &cls) const
{
    const auto found = clear_.find(cls);
    if (found == clear_.end())
    {
        return std::nullopt;
    }
    return std::pair{found->second, clearPower_};
}

std::string Settle(const Verdict &verdict, std::optional<double> share, bool ragged)
{
    if (verdict.cls != kPaint)
    {
        return verdict.cls;
    }
    if (!share)
    {
        return verdict.fallback;
    }
    if (*share >= kShareMetal)
    {
        return "metal";
    }
    if (ragged && verdict.fallback == "skin")
    {
        return "skin";
    }
    if (*share >= kShareMixed)
    {
        return "mixed";
    }
    return verdict.fallback;
}

} // namespace materials
