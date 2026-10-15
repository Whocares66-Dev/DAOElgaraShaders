// DAOLib #124's hook, read in its exe 2026-10-14 by lib-agent: the material
// loader asks for <name>.mao.xml before <name>.mao and parses whichever it
// finds, straight from the bytes the resource's first read gives, with no
// cache between; so a patch there is what the parser sees. The game has no
// .mao.xml of its own that is known, but a mod may: both are patched.
//
// The patch runs on whichever thread reads, holding that archive's lock, so
// it only looks the answer up and copies: the answers are read once, before
// the game reads anything, and not changed after.
#include "MaterialPatch.h"

#include "MaterialLearn.h"

#include "materials/Answers.h"

#include "dao/Resources.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace materialpatch
{

namespace
{

constexpr std::string_view kAnswersFile = "materials.answers";
// How many frames between looks at the counts: ten seconds or so.
constexpr int kEvery = 600;
// How many names of material objects with no answer are logged.
constexpr std::size_t kNamesLogged = 20;

materials::Answers g_answers;
std::atomic<std::size_t> g_given{0};
std::atomic<std::size_t> g_none{0};
std::mutex g_namesMutex;
std::vector<std::string> g_noneNames;
std::size_t g_logged = 0;
int g_frames = 0;

std::string Narrow(std::wstring_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const wchar_t c : name)
    {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

std::optional<std::vector<std::byte>> Patch(std::wstring_view name, std::span<const std::byte> bytes)
{
    auto patched = g_answers.Patched(bytes);
    if (!patched)
    {
        patched = materiallearn::Patched(bytes);
    }
    if (patched)
    {
        g_given.fetch_add(1, std::memory_order_relaxed);
        return patched;
    }
    // Not shipped, nor learned in an earlier session: to be worked out
    // (MaterialLearn.h), unless it was learned to need nothing.
    if (g_answers.For(bytes) == nullptr && !materiallearn::Knows(bytes))
    {
        materiallearn::Notice(name, bytes);
    }
    g_none.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard lock(g_namesMutex);
    if (g_noneNames.size() < kNamesLogged)
    {
        g_noneNames.push_back(Narrow(name));
    }
    return std::nullopt;
}

} // namespace

void Start(const std::filesystem::path &folder)
{
    std::ifstream file(folder / kAnswersFile, std::ios::binary);
    if (!file)
    {
        spdlog::warn("materials: no {} beside the plugin, so every material is drawn as no metal", kAnswersFile);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    g_answers.Parse(text);
    spdlog::info("materials: {} answers read, {} lines that did not parse", g_answers.Count(), g_answers.Unparsed());
    dao::resources::OnLoad(L"mao", &Patch);
    dao::resources::OnLoad(L"xml", [](std::wstring_view name, std::span<const std::byte> bytes) {
        return name.ends_with(L".mao.xml") ? Patch(name, bytes) : std::nullopt;
    });
}

void LogCounts()
{
    if (++g_frames % kEvery != 0)
    {
        return;
    }
    const std::size_t given = g_given.load(std::memory_order_relaxed);
    const std::size_t none = g_none.load(std::memory_order_relaxed);
    if (given + none == g_logged)
    {
        return;
    }
    g_logged = given + none;
    std::string names;
    {
        const std::lock_guard lock(g_namesMutex);
        for (const auto &n : g_noneNames)
        {
            names += names.empty() ? n : ", " + n;
        }
    }
    spdlog::info("materials: {} read with an answer, {} with none{}{}", given, none, names.empty() ? "" : ": ", names);
}

} // namespace materialpatch
