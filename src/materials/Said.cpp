#include "materials/Said.h"

#include <array>
#include <cctype>
#include <cstdio>

namespace materials
{

namespace
{

std::string Fixed(double value, int places)
{
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%.*f", places, value);
    return text.data();
}

// said_text: in the tool's order, each line as it writes it, with the tab
// before it and the line's end left out, as materials.answers has them.
std::string SaidText(bool metal, const std::string &mapFile, const std::optional<Bumps> &bumps,
                     const std::optional<std::pair<std::string, std::string>> &clear)
{
    std::string said;
    if (clear)
    {
        said += R"(<Vector4f Name="mml_vDaoClear" value=")" + clear->first + " " + clear->second +
                R"( 0.00 0.00"></Vector4f>)";
    }
    if (metal)
    {
        said += R"(<Float Name="mml_fDaoMetal" value="1.00"></Float>)";
    }
    if (!mapFile.empty())
    {
        said += R"(<Texture Name="mml_tDaoMetal" ResName=")" + mapFile + R"("></Texture>)";
    }
    if (bumps)
    {
        said += R"(<Vector4f Name="mml_vDaoBumps" value=")" + Fixed(bumps->width, 2) + " " + Fixed(bumps->height, 2) +
                " " + Fixed(bumps->spreads[0], 5) + " " + Fixed(bumps->spreads[1], 5) + R"("></Vector4f>)";
        said += R"(<Vector4f Name="mml_vDaoBumpsFar" value=")" + Fixed(bumps->spreads[2], 5) + " " +
                Fixed(bumps->spreads[3], 5) + " " + Fixed(bumps->spreads[4], 5) + " " + Fixed(bumps->spreads[5], 5) +
                R"("></Vector4f>)";
    }
    return said;
}

} // namespace

std::string MapName(std::string_view specular)
{
    std::string stem;
    for (const char c : specular)
    {
        stem.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos)
    {
        stem.resize(dot);
    }
    // (.*?)(?:_spec|_?s)((?:_?l[23])?) matched whole: a level of detail's
    // mark kept, then _spec, _s or s taken off before it.
    std::string mark;
    for (const std::string_view tail : {"_l2", "_l3", "l2", "l3"})
    {
        if (stem.size() > tail.size() && stem.ends_with(tail))
        {
            const std::string before = stem.substr(0, stem.size() - tail.size());
            if (before.ends_with("_spec") || before.ends_with('s'))
            {
                mark = std::string(tail);
                stem = before;
                break;
            }
        }
    }
    if (stem.ends_with("_spec"))
    {
        stem.resize(stem.size() - 5);
    }
    else if (stem.ends_with("_s"))
    {
        stem.resize(stem.size() - 2);
    }
    else if (stem.ends_with('s'))
    {
        stem.resize(stem.size() - 1);
    }
    else if (!mark.empty())
    {
        stem += mark;
        mark.clear();
    }
    return stem + mark + "_m.dds";
}

std::string SaidFor(const std::string &cls, const Names &names, const std::string &mapFile,
                    const std::optional<Bumps> &bumps)
{
    const auto clear = names.Clear(cls);
    if (cls != "metal" && cls != "mixed")
    {
        return bumps || clear ? SaidText(false, {}, bumps, clear) : std::string{};
    }
    if (cls == "mixed" && mapFile.empty())
    {
        return bumps ? SaidText(false, {}, bumps, std::nullopt) : std::string{};
    }
    return SaidText(true, mapFile, bumps, std::nullopt);
}

} // namespace materials
