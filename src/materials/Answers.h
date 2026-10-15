// What the plugin adds to a material object as the game reads it (issue 19):
// the table's answers, a line for every copy of every material object the
// game has, known by the FNV-1a hash of the copy's bytes
// (elgarashaders_materials writes them, materials.answers). A copy the game
// resolves from anywhere, a campaign's, an add-in's or a mod's, finds its own
// answer or none; nothing of ours replaces the game's file. Nothing here
// touches the game or the process.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace materials
{

// FNV-1a of 64 bits over bytes: the table tool's material_hash.
std::uint64_t Fnv1a64(std::span<const std::byte> bytes);

// The answers as materials.answers has them: by hash, the XML a copy is to be
// given before its </MaterialObject>.
class Answers
{
  public:
    Answers() = default;
    Answers(const Answers &) = delete;
    Answers &operator=(const Answers &) = delete;
    Answers(Answers &&) = delete;
    Answers &operator=(Answers &&) = delete;
    ~Answers() = default;

    // Reads the file's text into these: a line a copy, "<hash in hex> <name>
    // <XML>", and lines beginning ';' left out. A line that does not parse is
    // left out and counted.
    void Parse(std::string_view text);

    // The XML for a copy of these bytes, or null for none.
    [[nodiscard]] const std::string *For(std::span<const std::byte> bytes) const;

    // A copy's bytes with its answer put in before its last </MaterialObject>,
    // or nullopt where it has no answer or no such tag.
    [[nodiscard]] std::optional<std::vector<std::byte>> Patched(std::span<const std::byte> bytes) const;

    [[nodiscard]] std::size_t Count() const
    {
        return byHash_.size();
    }
    [[nodiscard]] std::size_t Unparsed() const
    {
        return unparsed_;
    }

  private:
    std::unordered_map<std::uint64_t, std::string> byHash_;
    std::size_t unparsed_ = 0;
};

} // namespace materials
