// Parts.h has what this is for.
#include "materials/Parts.h"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace materials
{
namespace
{

enum class Part : unsigned char
{
    No,
    Metal,
    Texels,
};

std::vector<int> Numbers(std::string_view list)
{
    std::vector<int> out;
    std::stringstream in{std::string(list)};
    std::string word;
    while (std::getline(in, word, ','))
    {
        out.push_back(std::stoi(word));
    }
    return out;
}

} // namespace

PartsFixes ReadParts(const std::filesystem::path &file, std::vector<std::string> &errors)
{
    PartsFixes out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        line = line.substr(0, line.find('#'));
        line = line.substr(0, line.find(';'));
        std::stringstream words(line);
        std::string name;
        std::string fix;
        if (!(words >> name))
        {
            continue;
        }
        if (!(words >> fix))
        {
            errors.push_back(line);
            continue;
        }
        try
        {
            if (fix != "=m" && fix != "=n")
            {
                (void)Numbers(fix[0] == '+' || fix[0] == '~' ? std::string_view(fix).substr(1) : std::string_view(fix));
            }
            out[name].push_back(fix);
        }
        catch (const std::exception &)
        {
            errors.push_back(line);
        }
    }
    return out;
}

namespace
{

std::vector<Part> Defaults(const Islands &islands, Kind kind, const Picture *texels)
{
    const auto shares = IslandShares(islands, texels);
    std::vector<Part> parts(shares.size());
    for (std::size_t i = 0; i < shares.size(); ++i)
    {
        parts[i] = DefaultMetal(kind, shares[i]) ? Part::Metal : Part::No;
    }
    return parts;
}

// One correction applied: "=m" or "=n" all; else a list of islands, after
// "+" metal, after "~" the texels' own, bare not metal.
void Apply(const std::string &fix, std::vector<Part> &parts)
{
    if (fix == "=m" || fix == "=n")
    {
        parts.assign(parts.size(), fix == "=m" ? Part::Metal : Part::No);
        return;
    }
    Part to = Part::No;
    std::string_view list = fix;
    if (fix[0] == '+' || fix[0] == '~')
    {
        to = fix[0] == '+' ? Part::Metal : Part::Texels;
        list.remove_prefix(1);
    }
    for (const int island : Numbers(list))
    {
        if (island >= 0 && std::cmp_less(island, parts.size()))
        {
            parts[static_cast<std::size_t>(island)] = to;
        }
    }
}

} // namespace

std::vector<double> IslandShares(const Islands &islands, const Picture *texels)
{
    const auto count = static_cast<std::size_t>(islands.count);
    std::vector<double> sum(count);
    std::vector<double> seen(count);
    for (std::size_t i = 0; i < islands.label.size(); ++i)
    {
        if (islands.label[i] >= 0)
        {
            const auto at = static_cast<std::size_t>(islands.label[i]);
            seen[at] += 1;
            sum[at] += texels != nullptr ? texels->values[i] : 0.0;
        }
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        sum[i] = seen[i] > 0 ? sum[i] / seen[i] : 0.0;
    }
    return sum;
}

bool DefaultMetal(Kind kind, double share)
{
    return kind == Kind::Metal || share >= 0.5;
}

Picture PartsMap(const Islands &islands, Kind kind, const Picture *texels, const std::vector<std::string> &fixes)
{
    std::vector<Part> parts = Defaults(islands, kind, texels);
    for (const auto &fix : fixes)
    {
        Apply(fix, parts);
    }
    Picture out{islands.width, islands.height, 1, std::vector<float>(islands.label.size(), 0.0F)};
    for (std::size_t i = 0; i < islands.label.size(); ++i)
    {
        if (islands.label[i] < 0)
        {
            continue;
        }
        const Part part = parts[static_cast<std::size_t>(islands.label[i])];
        if (part == Part::Metal)
        {
            out.values[i] = 1.0F;
        }
        else if (part == Part::Texels && texels != nullptr)
        {
            out.values[i] = texels->values[i];
        }
    }
    return out;
}

} // namespace materials
