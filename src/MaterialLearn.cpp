// MaterialLearn.h has what this is for. Three parts on three sides of the
// game: Notice on whichever thread reads (it keeps the bytes and nothing
// else, holding the archive's lock); Poll on the game thread, which asks the
// game for files (dao::resources wants the game thread: its reads are the
// engine's own, which is what decides which copy of a file the game takes)
// within a budget a frame; and one worker of our own, which decodes, works
// the material out and writes it, so that the frame never waits for a
// texture's arithmetic.
#include "MaterialLearn.h"

#include "materials/Answers.h"
#include "materials/Learn.h"
#include "materials/Names.h"

#include "dao/ClientExoApp.h"
#include "dao/GameModes.h"
#include "dao/Log.h"
#include "dao/Resources.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace materiallearn
{

namespace
{

using Clock = std::chrono::steady_clock;

// The names' tables, which the table's tool puts beside the plugin with its
// answers (elgarashaders_materials objects).
constexpr std::string_view kNamesFile = "materials.names";
constexpr std::string_view kLearnedFile = "learned.answers";
// The game thread's time a frame for our reads, at least one read a frame
// while any is waiting.
constexpr std::chrono::microseconds kBudget{2000};
// The most noticed and not yet read that are kept.
constexpr std::size_t kMostWaiting = 8192;

struct Noticed
{
    std::string name; // lower case, no extension
    std::uint64_t hash = 0;
    std::string text;
};

struct Job
{
    Noticed material;
    std::map<std::string, std::vector<std::byte>> textures; // by resource name
};

// What is learned, this session's and earlier ones', read by every thread
// the game reads on and added to by the worker.
std::shared_mutex g_knownMutex;
materials::Answers g_known;

materials::Names g_names;
bool g_learning = false;
std::filesystem::path g_folder; // <log folder>\elgarashaders\materials
std::filesystem::path g_maps;   // its maps\, a folder the game finds files in

std::mutex g_noticedMutex;
std::deque<Noticed> g_noticed;
std::set<std::uint64_t> g_seen; // noticed this session, whatever came of it

// Learned with a map, and given to reads once the game finds the map:
// their answer's line and the map's file.
std::mutex g_pendingMutex;
std::vector<std::pair<std::string, std::string>> g_pending;

std::mutex g_jobsMutex;
std::condition_variable g_jobsReady;
std::deque<std::shared_ptr<Job>> g_jobs; // by pointer: a Job is never copied or moved

// The game thread's own.
std::optional<dao::GameMode> g_lastMode;
std::set<std::wstring> g_asked; // material objects read for discovery
std::wstring g_listed;          // the campaign they were listed for
// The main campaign's module, by its name (GetName(GetModule()) in it).
constexpr std::wstring_view kMainCampaign = L"single player";
std::deque<std::wstring> g_toAsk; // and those waiting to be
// Materials whose textures are being read away from the game thread
// (ReadAsync), and the most at once: their textures are held until the
// worker has them.
int g_inFlight = 0;
constexpr int kMostInFlight = 4;
struct Spent
{
    std::size_t objects = 0;   // material objects read
    std::size_t materials = 0; // materials whose textures were read
    Clock::duration objectsTime{};
    Clock::duration longestWait{}; // one material's textures, asked to all read
    Clock::duration longestFrame{};
    std::size_t frames = 0; // frames with reads asked or waiting
};
Spent g_spent;
std::optional<Clock::time_point> g_lastFrame;

std::string Narrow(std::wstring_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const wchar_t c : name)
    {
        if (c >= 0x80)
        {
            out.push_back('?');
            continue;
        }
        out.push_back(static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c));
    }
    return out;
}

std::wstring Wide(const std::string &name)
{
    return {name.begin(), name.end()};
}

double Milliseconds(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

std::optional<std::string> ReadText(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Writes a file whole or not at all: to a name beside it, then renamed, so
// that a game closed while it writes leaves no half a map.
bool WriteWhole(const std::filesystem::path &path, std::span<const std::byte> bytes)
{
    const std::filesystem::path partial = path.string() + ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    return !error;
}

void Work(const Job &job)
{
    const materials::ImageOf image = [&job](const std::string &resource) -> std::optional<materials::Picture> {
        const auto found = job.textures.find(resource);
        return found == job.textures.end() ? std::nullopt : materials::PictureOf(found->second);
    };
    const std::string hex = fmt::format("{:016x}", job.material.hash);
    // A learned map is named by its material and its copy, never by its
    // mask as the table's are: two mods' copies of one name may differ.
    const std::string mapFile = job.material.name + "_" + hex.substr(0, 8) + "_m.dds";
    const auto learned = materials::Learn(job.material.name, job.material.text, g_names, image, mapFile);
    if (!learned)
    {
        return;
    }
    if (!learned->map.empty() && !WriteWhole(g_maps / mapFile, learned->map))
    {
        spdlog::warn("materials: {}'s map was not written to {}", job.material.name, g_maps.string());
        return;
    }
    const std::string line = hex + ' ' + job.material.name + ' ' + learned->said + '\n';
    {
        const bool fresh = !std::filesystem::exists(g_folder / kLearnedFile);
        std::ofstream out(g_folder / kLearnedFile, std::ios::binary | std::ios::app);
        if (fresh)
        {
            out << "; Materials the shipped table does not know, worked out in play by the table's rule\n"
                   "; (src/MaterialLearn.cpp): as materials.answers, a line a copy, by its bytes' hash.\n";
        }
        out << line;
    }
    // Given to every later read: at once, or where it names a map, once the
    // game finds the map (Poll), which it does within a few frames of its
    // writing, the folder being watched (DAOLib #137). An answer given
    // before would name a map the game does not have.
    if (learned->map.empty())
    {
        const std::unique_lock lock(g_knownMutex);
        g_known.Parse(line);
    }
    else
    {
        const std::lock_guard lock(g_pendingMutex);
        g_pending.emplace_back(line, mapFile);
    }
    spdlog::info("materials: learned {} ({}): {}{}", job.material.name, hex, learned->cls,
                 learned->map.empty() ? "" : ", with a map");
}

void Worker()
{
    while (true)
    {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(g_jobsMutex);
            g_jobsReady.wait(lock, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        try
        {
            Work(*job);
        }
        catch (const std::exception &error)
        {
            spdlog::warn("materials: {} was not worked out: {}", job->material.name, error.what());
        }
    }
}

// The material objects the game has that are not yet read, put to be read:
// in the main menu, and after a load into another campaign than the one
// listed (Awakening, a DLC campaign), which brings its own files. Not after
// every load: the list took 145-167 ms on the game thread after one (23 ms
// in the menu), and what a load into the same campaign added were the
// area's own archives' (27 objects, all in the table; runic save,
// 2026-10-15). A mod's unknown material there is noticed as the game reads
// it, like any, and has its answer from that area's next load.
void Discover()
{
    const auto mode = dao::CClientExoApp::CurrentMode();
    if (!mode || *mode == dao::GameMode::Loading || mode == g_lastMode)
    {
        g_lastMode = mode;
        return;
    }
    const bool fresh = !g_lastMode || *g_lastMode == dao::GameMode::Loading;
    g_lastMode = mode;
    if (!fresh)
    {
        return;
    }
    // The menu's list has the main campaign's: its add-ins' and the override
    // folder's among them (the load into it above added none of theirs).
    std::wstring campaign(kMainCampaign);
    if (const dao::ObjectId module = dao::script::api::GetModule(); module != dao::kInvalidObject)
    {
        campaign = dao::script::api::GetName(module);
        std::ranges::transform(campaign, campaign.begin(), [](wchar_t c) { return std::towlower(c); });
    }
    if (campaign.empty() || campaign == g_listed)
    {
        return;
    }
    g_listed = campaign;
    const auto started = Clock::now();
    std::size_t added = 0;
    for (std::wstring &name : dao::resources::Names(L"mao"))
    {
        if (g_asked.insert(name).second)
        {
            g_toAsk.push_back(std::move(name));
            ++added;
        }
    }
    spdlog::info("materials: {} material objects to read, for the campaign {}; listed in {:.1f} ms", added,
                 Narrow(campaign), Milliseconds(Clock::now() - started));
}

// The answers whose maps the game now finds, given to reads.
void Publish()
{
    std::vector<std::pair<std::string, std::string>> waiting;
    {
        const std::lock_guard lock(g_pendingMutex);
        waiting.swap(g_pending);
    }
    std::vector<std::pair<std::string, std::string>> still;
    for (auto &[line, mapFile] : waiting)
    {
        if (!dao::resources::Exists(Wide(mapFile)))
        {
            still.emplace_back(std::move(line), std::move(mapFile));
            continue;
        }
        {
            const std::unique_lock lock(g_knownMutex);
            g_known.Parse(line);
        }
        spdlog::info("materials: {} is found, and its material is given its answer", mapFile);
    }
    if (!still.empty())
    {
        const std::lock_guard lock(g_pendingMutex);
        g_pending.insert(g_pending.end(), std::make_move_iterator(still.begin()), std::make_move_iterator(still.end()));
    }
}

// The next noticed solid material's textures read and handed to the worker;
// false where none is waiting. The rest noticed (effects, the interface's)
// are dropped here, a parse each.
bool ReadNextTextures()
{
    if (g_inFlight >= kMostInFlight)
    {
        return false;
    }
    std::optional<Noticed> next;
    std::optional<materials::MaterialObject> object;
    while (!object)
    {
        {
            const std::lock_guard lock(g_noticedMutex);
            if (g_noticed.empty())
            {
                return false;
            }
            next = std::move(g_noticed.front());
            g_noticed.pop_front();
        }
        object = materials::ReadMaterialObject(next->name, next->text);
        if (object && !object->Solid())
        {
            object.reset();
        }
    }
    // Its textures asked of the game's loader away from the game thread: a
    // material's took up to 165 ms read on it, which no budget a frame can
    // split (runic save, 2026-10-15). The worker has the material when the
    // last of them is back.
    auto job = std::make_shared<Job>();
    job->material = std::move(*next);
    std::vector<std::string> resources;
    for (const char *slot : {"mml_tDiffuse", "mml_tSpecularMask", "mml_tNormalMap"})
    {
        const std::string resource = object->Texture(slot);
        if (!resource.empty() && std::ranges::find(resources, resource) == resources.end())
        {
            resources.push_back(resource);
        }
    }
    ++g_inFlight;
    auto waiting = std::make_shared<std::size_t>(resources.size());
    const auto asked = Clock::now();
    const auto arrived = [job, waiting, asked] {
        if (--*waiting > 0)
        {
            return;
        }
        --g_inFlight;
        ++g_spent.materials;
        g_spent.longestWait = std::max(g_spent.longestWait, Clock::now() - asked);
        {
            const std::lock_guard lock(g_jobsMutex);
            g_jobs.push_back(job);
        }
        g_jobsReady.notify_one();
    };
    if (resources.empty())
    {
        ++*waiting;
        arrived();
    }
    for (const std::string &resource : resources)
    {
        // The name held shared, so that the callback's copies copy no string.
        auto held = std::make_shared<const std::string>(resource);
        dao::resources::ReadAsync(Wide(resource), [job, held, arrived](std::optional<std::vector<std::byte>> bytes) {
            if (bytes)
            {
                job->textures.emplace(*held, std::move(*bytes));
            }
            arrived();
        });
    }
    return true;
}

// The next material object read for discovery: the load patch sees it
// (MaterialPatch.cpp) and notices it where it is unknown. False where none
// is waiting.
bool ReadNextObject()
{
    if (g_toAsk.empty())
    {
        return false;
    }
    const auto started = Clock::now();
    const std::wstring name = std::move(g_toAsk.front());
    g_toAsk.pop_front();
    (void)dao::resources::Read(name);
    ++g_spent.objects;
    g_spent.objectsTime += Clock::now() - started;
    return true;
}

} // namespace

void Start(const std::filesystem::path &plugin)
{
    const auto names = ReadText(plugin / kNamesFile);
    std::string error;
    if (!names || !g_names.Parse(*names, error))
    {
        spdlog::warn("materials: {} {}, so no material is worked out in play", kNamesFile,
                     names ? "did not parse: " + error : std::string("is not beside the plugin"));
        return;
    }
    g_folder = dao::LogDirectory() / "elgarashaders" / "materials";
    g_maps = g_folder / "maps";
    std::error_code made;
    std::filesystem::create_directories(g_maps, made);
    if (made)
    {
        spdlog::warn("materials: {} could not be made ({}), so no material is worked out in play", g_maps.string(),
                     made.message());
        return;
    }
    if (const auto known = ReadText(g_folder / kLearnedFile))
    {
        g_known.Parse(*known);
    }
    // Watched: a map written as the game runs is found within a few frames
    // (the engine lists a watched folder again every fifth frame).
    if (!dao::resources::AddFolder(g_maps, true))
    {
        spdlog::warn("materials: the game did not take {}, so learned maps are not found", g_maps.string());
    }
    spdlog::info("materials: {} learned in earlier sessions, in {}", g_known.Count(), g_folder.string());
    std::thread(&Worker).detach();
    g_learning = true;
}

std::optional<std::vector<std::byte>> Patched(std::span<const std::byte> bytes)
{
    const std::shared_lock lock(g_knownMutex);
    return g_known.Patched(bytes);
}

bool Knows(std::span<const std::byte> bytes)
{
    const std::shared_lock lock(g_knownMutex);
    return g_known.For(bytes) != nullptr;
}

void Notice(std::wstring_view name, std::span<const std::byte> bytes)
{
    if (!g_learning)
    {
        return;
    }
    const std::uint64_t hash = materials::Fnv1a64(bytes);
    const std::lock_guard lock(g_noticedMutex);
    if (g_noticed.size() >= kMostWaiting || !g_seen.insert(hash).second)
    {
        return;
    }
    std::string narrow = Narrow(name);
    for (const std::string_view extension : {".mao.xml", ".mao"})
    {
        if (narrow.ends_with(extension))
        {
            narrow.resize(narrow.size() - extension.size());
            break;
        }
    }
    g_noticed.push_back(
        {std::move(narrow), hash, std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size())});
}

void Poll()
{
    if (!g_learning)
    {
        return;
    }
    const auto now = Clock::now();
    // A frame's length, but none that loads or follows a load: the game's
    // own loading takes seconds a frame, and would hide ours.
    const auto mode = dao::CClientExoApp::CurrentMode();
    const bool loading = !mode || *mode == dao::GameMode::Loading || g_lastMode == dao::GameMode::Loading;
    const auto frame = g_lastFrame && !loading ? now - *g_lastFrame : Clock::duration{};
    g_lastFrame = now;
    Discover();
    Publish();
    // Textures first: a material noticed is one the game is about to draw.
    // Their reads are only asked here; the objects' are made here, a fraction
    // of a millisecond each.
    bool read = false;
    while (Clock::now() - now < kBudget || !read)
    {
        if (!ReadNextTextures() && !ReadNextObject())
        {
            break;
        }
        read = true;
    }
    bool idle = false;
    {
        const std::lock_guard lock(g_noticedMutex);
        idle = g_noticed.empty() && g_toAsk.empty() && g_inFlight == 0;
    }
    if (read || !idle)
    {
        ++g_spent.frames;
        g_spent.longestFrame = std::max(g_spent.longestFrame, frame);
    }
    else if (g_spent.frames > 0)
    {
        spdlog::info("materials: read {} material objects ({:.1f} ms) and {} materials' textures away from the "
                     "game thread (the longest wait {:.1f} ms), over {} frames, the longest {:.1f} ms",
                     g_spent.objects, Milliseconds(g_spent.objectsTime), g_spent.materials,
                     Milliseconds(g_spent.longestWait), g_spent.frames, Milliseconds(g_spent.longestFrame));
        g_spent = {};
    }
}

} // namespace materiallearn
