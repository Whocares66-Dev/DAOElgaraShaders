#include "materials/Answers.h"

#include <algorithm>
#include <charconv>
#include <memory>

namespace materials
{

std::uint64_t Fnv1a64(std::span<const std::byte> bytes)
{
    std::uint64_t value = 0xCBF29CE484222325ULL;
    for (const std::byte b : bytes)
    {
        value = (value ^ static_cast<std::uint64_t>(b)) * 0x100000001B3ULL;
    }
    return value;
}

void Answers::Parse(std::string_view text)
{
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == ';')
        {
            continue;
        }
        const std::size_t first = line.find(' ');
        const std::size_t second = first == std::string_view::npos ? first : line.find(' ', first + 1);
        const std::string_view hex = line.substr(0, std::min(first, line.size()));
        const char *const begin = std::to_address(hex.begin());
        const char *const hexEnd = std::to_address(hex.end());
        std::uint64_t hash = 0;
        const auto parsed = std::from_chars(begin, hexEnd, hash, 16);
        if (second == std::string_view::npos || hex.empty() || parsed.ec != std::errc{} || parsed.ptr != hexEnd)
        {
            ++unparsed_;
            continue;
        }
        byHash_.emplace(hash, std::string(line.substr(second + 1)));
    }
}

const std::string *Answers::For(std::span<const std::byte> bytes) const
{
    const auto found = byHash_.find(Fnv1a64(bytes));
    return found == byHash_.end() ? nullptr : &found->second;
}

std::optional<std::vector<std::byte>> Answers::Patched(std::span<const std::byte> bytes) const
{
    // An empty answer is a copy known to need nothing (MaterialLearn.h).
    const std::string *said = For(bytes);
    if (said == nullptr || said->empty())
    {
        return std::nullopt;
    }
    constexpr std::string_view kEnd = "</MaterialObject>";
    const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::size_t at = text.rfind(kEnd);
    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(bytes.size() + said->size());
    out.insert(out.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    const auto *added = reinterpret_cast<const std::byte *>(said->data());
    out.insert(out.end(), added, added + said->size());
    out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    return out;
}

} // namespace materials
