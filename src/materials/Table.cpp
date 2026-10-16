// What tools/material_table.py did in Python until 2026-10-15 (its Game,
// classify, build_table, table_text, hand_edits, table_classes and
// write_objects), on DAOLib's install index. Checked against its last output
// before it was taken out: the same class for every material and the same
// lines in every answer, but where this is right and it was not: a texture
// found as the material's own campaign finds it (Awakening's copies had been
// given the main campaign's placeholder textures, the elven shield the
// other campaign's normal map), and a map of its own for a material whose
// specular mask another with other paint shares (glv_masd).
#include "materials/Table.h"

#include "materials/Answers.h"
#include "materials/Said.h"

#include "dao/core/Dds.h"
#include "dao/core/Install.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>

namespace materials
{

namespace
{

constexpr std::array<std::string_view, 13> kClasses = {"metal",   "mixed", "stone",   "wood",  "cloth",
                                                       "leather", "skin",  "foliage", "glass", "crystal",
                                                       "gem",     "ice",   "other"};

constexpr std::string_view kTableHeader =
    R"(; What each of the game's solid materials is, for the shaders' metal and roughness model.
; Made by elgarashaders_materials (src/materials/Table.cpp) from the material objects' names and
; textures; one line a material object (<name>.mao), sorted.
;
;   metal    the whole surface is metal: metallic 1
;   mixed    metal and not in one texture: its metalness is a map made beside this table
;            (<its specular mask>_m.dds, or <the material>_m.dds where another material with
;            that mask has other paint); the comment says what share of its texels came out
;            metal, and the map's name
;   stone, wood, cloth, leather, skin, foliage, other
;            not metal: metallic 0. They are kept apart because they are useful to read and may
;            matter later. `other` is everything else, and what a material is where the
;            evidence does not say: a wrong metal is worse than a missed one.
;   glass, crystal, gem, ice
;            clear solids: not metal, and the shaders take the substance's mirror and smoothness
;
; A line may be edited by hand. End it with `; by hand` (and a reason, if you like) and
; the tool keeps it as it is from then on:
;
;   plc_statparagnm_01 = stone ; by hand: the paragon's statue is gilded, not gold
;
; A level-of-detail copy (<name>_l2, _l3) is given its base's class, a hand's too, unless
; it has a line by hand of its own. A material made `mixed` by hand gets a map by the rule.
;
; What a material is by its names is data/material_names.txt; the rule for a texel is
; src/materials/TextureRule.cpp.
)";

constexpr std::string_view kAnswersHeader =
    R"(; What the plugin adds to each material object the game reads, by the object's bytes
; (issue 19). Made by elgarashaders_materials from data/materials.ini; not edited by hand.
; A line a copy: the FNV-1a hash of 64 bits of the file's bytes as the game reads them, in
; hex; the material's name; the XML put before its </MaterialObject>, none where it is
; given nothing.
)";

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Hex(std::uint64_t value)
{
    std::array<char, 17> text{};
    std::snprintf(text.data(), text.size(), "%016llx", static_cast<unsigned long long>(value));
    return text.data();
}

// The text with its <!-- --> comments taken out: two copies the same but for
// them are one material.
std::string WithoutComments(std::string_view text)
{
    std::string out;
    std::size_t at = 0;
    while (true)
    {
        const std::size_t open = text.find("<!--", at);
        if (open == std::string_view::npos)
        {
            out.append(text.substr(at));
            return out;
        }
        out.append(text.substr(at, open - at));
        const std::size_t close = text.find("-->", open + 4);
        if (close == std::string_view::npos)
        {
            out.append(text.substr(open));
            return out;
        }
        at = close + 3;
    }
}

// A level of detail's copy's base: the name without its _l2 or _l3.
std::string Base(const std::string &name)
{
    if (name.size() > 3 && (name.ends_with("_l2") || name.ends_with("_l3")))
    {
        return name.substr(0, name.size() - 3);
    }
    return name;
}

// Whether a material object holds a line of the plugin's own (Said.cpp's:
// mml_fDaoMetal and the rest), which none of the game's has. Such a copy is
// the plugin's output, not the game's file: the add-ins' dump of 2026-10-12
// was read through the game while the release build's rewritten objects sat
// in its override folder, and 252 of its 539 objects were those. Their
// answers were keyed by the hash of our own copy, which the game never reads
// once the objects are patched at load, and the Blood Dragon plate and every
// other enciphered add-in's metal was drawn as no metal (seen 2026-10-15).
bool HoldsOurLines(std::string_view text)
{
    return text.find("mml_fDao") != std::string_view::npos || text.find("mml_vDao") != std::string_view::npos ||
           text.find("mml_tDao") != std::string_view::npos;
}

std::optional<Copy> CopyOf(const std::string &name, const std::vector<std::byte> &bytes, std::string where)
{
    std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (HoldsOurLines(text))
    {
        throw std::runtime_error(where + " holds lines of the plugin's own: it was read with the plugin's materials "
                                         "in place, and is not the game's (dump the add-ins again: dump_addins.py)");
    }
    auto object = ReadMaterialObject(name, text);
    if (!object || !object->Solid())
    {
        return std::nullopt;
    }
    return Copy{std::move(text), Fnv1a64(bytes), std::move(*object), std::move(where), {}};
}

void AddCopy(Material &material, Copy copy)
{
    const bool known = std::ranges::any_of(material.copies, [&](const Copy &c) { return c.hash == copy.hash; });
    if (!known)
    {
        material.copies.push_back(std::move(copy));
    }
}

std::string TrimmedLine(const std::string &line)
{
    std::string out = line;
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n'))
    {
        out.pop_back();
    }
    return out;
}

// `name = class` at a line's start (TABLE_LINE), and what follows it.
std::optional<std::tuple<std::string, std::string, std::string_view>> TableLine(std::string_view line)
{
    std::size_t at = line.find_first_not_of(" \t");
    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::size_t nameEnd = line.find_first_of(" \t=;", at);
    if (nameEnd == std::string_view::npos || nameEnd == at || line[at] == ';')
    {
        return std::nullopt;
    }
    std::string name(line.substr(at, nameEnd - at));
    at = line.find_first_not_of(" \t", nameEnd);
    if (at == std::string_view::npos || line[at] != '=')
    {
        return std::nullopt;
    }
    at = line.find_first_not_of(" \t", at + 1);
    std::size_t end = at;
    while (end < line.size() && line[end] >= 'a' && line[end] <= 'z')
    {
        ++end;
    }
    if (at == std::string_view::npos || end == at)
    {
        return std::nullopt;
    }
    return std::tuple{Lower(name), std::string(line.substr(at, end - at)), line.substr(end)};
}

// A mixed material's map, and the key it is known by: two materials with one
// specular mask and other paint, or the one plate and the other not, are two
// maps. The tool gave them one, the first's (glv_masc and glv_masd).
struct MapWork
{
    std::string material;
    // specular, diffuse, plate, and the material's own name where its map is its parts' (one
    // texture drawn by a dagger's islands and a level's prop is two maps)
    std::tuple<std::string, std::string, bool, std::string> key;
    std::optional<Picture> map;
};

// Names each map: its specular mask's (MapName) for the first of a key, and
// the material's own where another key has that name.
std::map<std::string, std::string> NameMaps(const std::vector<MapWork> &works)
{
    std::map<std::string, std::tuple<std::string, std::string, bool, std::string>> claimed;
    std::map<std::string, std::string> out;
    for (const MapWork &work : works)
    {
        if (!work.map)
        {
            continue;
        }
        std::string name = MapName(std::get<0>(work.key));
        const auto found = claimed.find(name);
        if (!std::get<3>(work.key).empty())
        {
            // a map made from a piece's parts is that piece's alone: a level's prop drawn with the
            // same texture (chainspike, with a darkspawn dagger's) claimed its name first
            name = Lower(work.material) + "_parts_m.dds";
        }
        else if (found != claimed.end() && found->second != work.key)
        {
            name = Lower(work.material) + "_m.dds";
        }
        claimed.emplace(name, work.key);
        out[work.material] = name;
    }
    return out;
}

// How many materials are worked on at once. Each holds a few pictures of up
// to 1024 a side as floats (16 MB a picture): with every core of twenty at
// it, the 32-bit tool ran out of room (std::bad_alloc, 2026-10-15).
constexpr std::size_t kWorkers = 6;

// Calls `work` with each of 0 to count - 1 on kWorkers threads. An exception
// in one is kept and thrown again after all have finished, where in a thread
// it would end the program at once.
void InParallel(std::size_t count, const std::function<void(std::size_t)> &work)
{
    std::atomic<std::size_t> next{0};
    std::mutex failedLock;
    std::exception_ptr failed;
    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (std::size_t t = 0; t < std::min(kWorkers, count); ++t)
    {
        threads.emplace_back([&] {
            for (std::size_t i = next++; i < count; i = next++)
            {
                try
                {
                    work(i);
                }
                catch (...)
                {
                    const std::scoped_lock guard(failedLock);
                    failed = std::current_exception();
                }
            }
        });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    if (failed)
    {
        std::rethrow_exception(failed);
    }
}

MapWork KeyOf(const MaterialObject &object, const std::string &label, bool parts)
{
    return {
        label,
        {object.Texture("mml_tSpecularMask"), object.Texture("mml_tDiffuse"), object.Plate(), parts ? object.name : ""},
        std::nullopt};
}

} // namespace

Catalogue::Catalogue() = default;
Catalogue::~Catalogue() = default;

namespace
{

using dao::core::install::Index;
using dao::core::install::Unused;

// Each campaign an add-in extends besides the main one is a campaign of its
// own, whose files the game takes in another order (Awakening's core_ep1, and
// its add-ins').
std::set<std::string> OtherCampaigns(const Index &main)
{
    std::set<std::string> out;
    for (const auto &addIn : main.AddIns())
    {
        if (!addIn.extends.empty() && addIn.extends != "Single Player")
        {
            out.insert(addIn.extends);
        }
    }
    return out;
}

// Whether the table reads a copy: one the game takes in the index's campaign,
// or a level's own, which it takes while the level is loaded; not a plugin's
// files (ours among them: an earlier build's material objects in the
// install) nor a player's override folder's, since the table is of the game
// as it ships and a mod's copy is answered in play; and one it can read.
bool Wanted(const Index &index, const dao::core::install::Copy &copy)
{
    const std::string &kind = index.Sources()[copy.source].kind;
    const bool taken = copy.unused == Unused::No || copy.unused == Unused::Level;
    return taken && kind != "plugin" && kind != "override" && index.Readable(copy);
}

// A material object's copies in one campaign's index, added to the
// material's.
void AddCopies(Material &material, const std::string &file, const std::string &campaign, const Index &index)
{
    const auto *copies = index.Find(file);
    if (copies == nullptr)
    {
        return;
    }
    for (const auto &copy : *copies)
    {
        if (!Wanted(index, copy))
        {
            continue;
        }
        auto bytes = index.Read(copy);
        auto read = bytes ? CopyOf(material.name, *bytes, index.Where(copy)) : std::nullopt;
        if (read)
        {
            read->campaign = campaign;
            AddCopy(material, std::move(*read));
        }
    }
}

std::optional<std::string> ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

std::unique_ptr<Catalogue> Catalogue::Load(const std::filesystem::path &game, const std::filesystem::path &documents,
                                           const std::filesystem::path &addins, std::string &error)
{
    std::unique_ptr<Catalogue> out(new Catalogue());
    out->addins_ = addins;
    auto main = Index::Build(game, documents);
    if (!main || main->Resources().empty())
    {
        error = "no game at " + game.string();
        return nullptr;
    }
    const auto others = OtherCampaigns(*main);
    out->indexes_.emplace_back("Single Player", std::move(main));
    for (const auto &campaign : others)
    {
        dao::core::install::Options options;
        options.campaign = campaign;
        out->indexes_.emplace_back(campaign, Index::Build(game, documents, options));
    }
    // Each material's copies, the main campaign's first.
    for (const auto &[file, ignored] : out->indexes_.front().second->Resources())
    {
        if (!file.ends_with(".mao"))
        {
            continue;
        }
        Material material{file.substr(0, file.size() - 4), {}, {}};
        for (const auto &[campaign, index] : out->indexes_)
        {
            AddCopies(material, file, campaign, *index);
        }
        if (!material.copies.empty())
        {
            material.main = material.copies.front();
            out->materials_.emplace(material.name, std::move(material));
        }
    }
    out->AddReadOut();
    return out;
}

Files Catalogue::FilesOf() const
{
    Files files;
    for (const auto &[campaign, index] : indexes_)
    {
        files.campaigns.emplace_back(campaign, index.get());
    }
    files.bytes = [this](const std::string &file, const std::string &campaign) { return Bytes(file, campaign); };
    files.readOut = addins_;
    return files;
}

void Catalogue::AddReadOut()
{
    std::error_code failed;
    for (const auto &entry : std::filesystem::directory_iterator(addins_, failed))
    {
        const std::string file = Lower(entry.path().filename().string());
        const std::string name = file.substr(0, file.size() - 4);
        if (!file.ends_with(".mao") || materials_.contains(name))
        {
            continue;
        }
        const auto text = ReadFile(entry.path());
        const auto *bytes = text ? reinterpret_cast<const std::byte *>(text->data()) : nullptr;
        auto read = text ? CopyOf(name, {bytes, bytes + text->size()}, entry.path().string()) : std::nullopt;
        if (read)
        {
            read->campaign = "Single Player";
            Material material{name, *read, {*read}};
            materials_.emplace(name, std::move(material));
        }
    }
}

std::optional<std::vector<std::byte>> Catalogue::Bytes(const std::string &resource, const std::string &campaign) const
{
    // The campaign's own copy first. Where it has none that can be read, an
    // other campaign's: an add-in's texture is in its enciphered archive,
    // which only the game reads, and the same file is often in Awakening's
    // (the Stone Prisoner's pigeon's normal map, c_stpigeon_0n.dds, is in
    // core_ep1's textures.erf). The Python tool took the first archive that
    // had a name whatever the campaign, so this is what it did there.
    const auto own = std::ranges::find(indexes_, campaign, [](const auto &pair) { return pair.first; });
    std::vector<const Index *> order{own == indexes_.end() ? indexes_.front().second.get() : own->second.get()};
    for (const auto &[name, other] : indexes_)
    {
        if (other.get() != order.front())
        {
            order.push_back(other.get());
        }
    }
    for (const auto *index : order)
    {
        const auto *copies = index->Find(resource);
        if (copies == nullptr)
        {
            continue;
        }
        const auto wanted = std::ranges::find_if(*copies, [&](const auto &copy) { return Wanted(*index, copy); });
        if (wanted != copies->end())
        {
            return index->Read(*wanted);
        }
    }
    const auto text = ReadFile(addins_ / resource);
    if (!text)
    {
        return std::nullopt;
    }
    const auto *start = reinterpret_cast<const std::byte *>(text->data());
    return std::vector<std::byte>(start, start + text->size());
}

std::optional<Picture> Catalogue::Image(const std::string &resource, const std::string &campaign) const
{
    const auto bytes = Bytes(resource, campaign);
    return bytes ? PictureOf(*bytes) : std::nullopt;
}

std::vector<std::string> Catalogue::Enciphered(std::string_view extension) const
{
    std::vector<std::string> out;
    const auto &index = *indexes_.front().second;
    for (const auto &[file, copies] : index.Resources())
    {
        const bool unread = std::ranges::any_of(copies, [&](const auto &copy) { return !index.Readable(copy); });
        if (file.ends_with(extension) && unread)
        {
            out.push_back(file);
        }
    }
    return out;
}

bool Catalogue::Readable(const std::string &resource) const
{
    return std::ranges::any_of(indexes_, [&](const auto &pair) {
        const auto *copies = pair.second->Find(resource);
        return copies != nullptr &&
               std::ranges::any_of(*copies, [&](const auto &copy) { return pair.second->Readable(copy); });
    });
}

ImageOf ImagesOf(const Catalogue &catalogue, const std::string &campaign)
{
    // The campaign held shared, so that the function's copies copy no string.
    auto held = std::make_shared<const std::string>(campaign);
    return [&catalogue, held](const std::string &resource) { return catalogue.Image(resource, *held); };
}

std::size_t Catalogue::LoadParts(const PartsFixes &fixes)
{
    const Files files = FilesOf();
    const Inventory inventory = TakeInventory(files);
    const auto kinds = KindsOf(inventory);
    for (const auto &[object, chunks] : inventory.meshes)
    {
        // held: every chunk in a weapon's model (w_...), whose parts are islands of their own
        const bool held = std::ranges::all_of(chunks, [](const auto &chunk) { return chunk.first.starts_with("w_"); });
        const auto material = materials_.find(object);
        if (!held || material == materials_.end())
        {
            continue;
        }
        const auto texels = MapOf(material->second.main.object, ImagesOf(*this, material->second.main.campaign));
        if (!texels)
        {
            continue;
        }
        const auto meshes = UvMeshesOf(files, chunks);
        const Islands islands = IslandsOf(meshes, texels->metal.width, texels->metal.height);
        if (islands.count == 0)
        {
            continue;
        }
        const auto kind = kinds.find(object);
        const auto fixed = fixes.find(object);
        parts_.emplace(object, PartsMap(islands, kind == kinds.end() ? Kind::None : kind->second, &texels->metal,
                                        fixed == fixes.end() ? std::vector<std::string>{} : fixed->second));
    }
    return parts_.size();
}

std::optional<MetalMap> Catalogue::MetalOf(const Copy &copy) const
{
    if (const auto part = parts_.find(copy.object.name); part != parts_.end())
    {
        double sum = 0;
        for (const float v : part->second.values)
        {
            sum += v;
        }
        return MetalMap{part->second, sum / static_cast<double>(std::max<std::size_t>(1, part->second.values.size()))};
    }
    return MapOf(copy.object, ImagesOf(*this, copy.campaign));
}

Classed Classify(const Catalogue &catalogue, const Names &names, const Copy &copy)
{
    Classed out = Classify(copy.object, names, ImagesOf(catalogue, copy.campaign));
    // A held piece's class is its parts': none, or mixed with their map (texels no island covers are not drawn).
    if (const auto *parts = catalogue.Parts(copy.object.name))
    {
        const auto metal = std::ranges::count_if(parts->values, [](float v) { return v >= 0.5F; });
        const auto all = static_cast<std::ptrdiff_t>(parts->values.size());
        if (metal == 0)
        {
            if (out.cls == "metal" || out.cls == "mixed")
            {
                out.cls = "other";
            }
            out.map.reset();
        }
        else
        {
            out.cls = "mixed";
            out.map = *parts;
        }
        out.share = static_cast<double>(metal) / static_cast<double>(std::max<std::ptrdiff_t>(1, all));
    }
    return out;
}

std::map<std::string, Hand> ReadHands(const std::filesystem::path &ini)
{
    std::map<std::string, Hand> out;
    std::ifstream file(ini);
    std::string line;
    while (std::getline(file, line))
    {
        line = TrimmedLine(line);
        const auto found = TableLine(line);
        if (!found)
        {
            continue;
        }
        const auto &[name, cls, rest] = *found;
        const std::size_t at = rest.find_first_not_of(" \t");
        if (at == std::string_view::npos || rest[at] != ';')
        {
            continue;
        }
        const std::size_t words = rest.find_first_not_of(" \t", at + 1);
        if (words != std::string_view::npos && Lower(rest.substr(words, 7)) == "by hand")
        {
            out[name] = {Lower(cls), line};
        }
    }
    return out;
}

std::map<std::string, std::string> ReadClasses(const std::filesystem::path &ini)
{
    std::map<std::string, std::string> out;
    std::ifstream file(ini);
    std::string line;
    while (std::getline(file, line))
    {
        const auto found = TableLine(TrimmedLine(line));
        if (found && std::ranges::find(kClasses, std::get<1>(*found)) != kClasses.end())
        {
            out[std::get<0>(*found)] = std::get<1>(*found);
        }
    }
    return out;
}

namespace
{

// Every material's main copy classed by the rule.
std::vector<Classed> ClassifyAll(const Catalogue &catalogue, const Names &names,
                                 const std::vector<const Material *> &materials,
                                 const std::function<void(std::size_t, std::size_t)> &progress)
{
    std::vector<Classed> out(materials.size());
    std::atomic<std::size_t> done{0};
    InParallel(materials.size(), [&](std::size_t i) {
        out[i] = Classify(catalogue, names, materials[i]->main);
        const std::size_t now = ++done;
        if (progress && now % 200 == 0)
        {
            progress(now, materials.size());
        }
    });
    return out;
}

// The classes as the table has them: a level of detail's copy takes its
// base's class; a hand's line is over the lot; a material made mixed by hand
// gets a map by the rule. The maps, a material each, to be named.
std::vector<MapWork> Settled(const Catalogue &catalogue, const std::vector<const Material *> &materials,
                             const std::map<std::string, Hand> &hands, std::vector<Classed> &classed)
{
    std::map<std::string, std::size_t> place;
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        place[materials[i]->name] = i;
    }
    std::vector<MapWork> works;
    works.reserve(materials.size());
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        Classed &row = classed[i];
        if (const auto base = place.find(Base(materials[i]->name)); base != place.end() && base->second != i)
        {
            row.cls = classed[base->second].cls;
        }
        if (const auto hand = hands.find(materials[i]->name); hand != hands.end())
        {
            row.cls = hand->second.cls;
        }
        MapWork work = KeyOf(materials[i]->main.object, materials[i]->name,
                             catalogue.Parts(materials[i]->main.object.name) != nullptr);
        if (row.cls == "mixed" && !row.map)
        {
            if (auto map = catalogue.MetalOf(materials[i]->main))
            {
                row.share = map->share;
                row.map = std::move(map->metal);
            }
        }
        if (row.cls == "mixed")
        {
            work.map = row.map;
        }
        works.push_back(std::move(work));
    }
    return works;
}

// The table's text: its header, then a line a material, a hand's as written.
std::string TableText(const std::vector<const Material *> &materials, const std::vector<Classed> &classed,
                      const std::map<std::string, Hand> &hands, const std::map<std::string, std::string> &mapNames)
{
    std::map<std::string, const Classed *> rows;
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        rows[materials[i]->name] = &classed[i];
    }
    std::set<std::string> all;
    std::size_t width = 0;
    for (const auto &[name, row] : rows)
    {
        all.insert(name);
    }
    for (const auto &[name, hand] : hands)
    {
        all.insert(name);
    }
    for (const auto &name : all)
    {
        width = std::max(width, name.size());
    }
    std::string text(kTableHeader);
    for (const auto &name : all)
    {
        if (const auto hand = hands.find(name); hand != hands.end())
        {
            text += hand->second.line + "\n";
            continue;
        }
        const Classed &row = *rows.at(name);
        std::string line = name + std::string(width - name.size(), ' ') + " = " + row.cls;
        if (row.cls == "mixed" && row.share && mapNames.contains(name))
        {
            std::array<char, 32> percent{};
            std::snprintf(percent.data(), percent.size(), "%.0f%%", *row.share * 100);
            line += std::string("  ; ") + percent.data() + " metal, " + mapNames.at(name);
        }
        text += line + "\n";
    }
    return text;
}

} // namespace

std::string MakeTable(const Catalogue &catalogue, const Names &names, const std::map<std::string, Hand> &hands,
                      const std::function<void(std::size_t, std::size_t)> &progress)
{
    std::vector<const Material *> materials;
    materials.reserve(catalogue.Materials().size());
    for (const auto &[name, material] : catalogue.Materials())
    {
        materials.push_back(&material);
    }
    auto classed = ClassifyAll(catalogue, names, materials, progress);
    const auto works = Settled(catalogue, materials, hands, classed);
    return TableText(materials, classed, hands, NameMaps(works));
}

namespace
{

// A copy of a material to give an answer: the table's class for a copy that
// says what the material's own does, its own by the rule for one that says
// other things (another campaign's).
struct Work
{
    std::string name;
    std::uint64_t hash = 0;
    const Copy *copy = nullptr;
    std::string cls;
    bool another = false;
    MapWork map;
    std::optional<Bumps> bumps;
};

std::vector<Work> CopiesToAnswer(const Catalogue &catalogue, const std::map<std::string, std::string> &classes)
{
    std::vector<Work> works;
    for (const auto &[name, material] : catalogue.Materials())
    {
        const auto found = classes.find(name);
        const std::string cls = found == classes.end() ? "other" : found->second;
        const std::string own = WithoutComments(material.main.text);
        std::vector<const Copy *> copies;
        copies.reserve(material.copies.size());
        for (const Copy &copy : material.copies)
        {
            copies.push_back(&copy);
        }
        std::ranges::sort(copies, {}, [](const Copy *c) { return Hex(c->hash); });
        for (const Copy *copy : copies)
        {
            works.push_back({name, copy->hash, copy, cls, WithoutComments(copy->text) != own, {}, std::nullopt});
        }
    }
    return works;
}

void WorkOut(const Catalogue &catalogue, const Names &names, Work &work)
{
    if (work.another)
    {
        work.cls = Classify(catalogue, names, *work.copy).cls;
    }
    work.map = KeyOf(work.copy->object, work.another ? work.name + "#" + Hex(work.hash) : work.name,
                     catalogue.Parts(work.copy->object.name) != nullptr);
    const ImageOf image = ImagesOf(catalogue, work.copy->campaign);
    if (work.cls == "mixed")
    {
        if (auto map = catalogue.MetalOf(*work.copy))
        {
            work.map.map = std::move(map->metal);
        }
    }
    work.bumps = BumpsOf(work.copy->object, image);
}

// The maps' names, the main copies' first so a map is named by the material
// whose own it is; another copy's map that cannot have its mask's name is
// named by the material and its hash.
std::map<std::string, std::string> MapNames(const std::vector<Work> &works)
{
    std::vector<MapWork> maps;
    maps.reserve(works.size());
    for (const bool another : {false, true})
    {
        for (const Work &work : works)
        {
            if (work.another == another)
            {
                maps.push_back(work.map);
            }
        }
    }
    auto names = NameMaps(maps);
    for (auto &[label, file] : names)
    {
        if (const std::size_t hash = label.find('#'); hash != std::string::npos && file == label + "_m.dds")
        {
            file = label.substr(0, hash) + "_" + label.substr(hash + 1, 8) + "_m.dds";
        }
    }
    return names;
}

void Count(Objects &out, const std::string &what, std::size_t by = 1)
{
    const auto found = std::ranges::find(out.counts, what, [](const auto &pair) { return pair.first; });
    if (found == out.counts.end())
    {
        out.counts.emplace_back(what, by);
    }
    else
    {
        found->second += by;
    }
}

} // namespace

Objects MakeObjects(const Catalogue &catalogue, const Names &names, const std::map<std::string, std::string> &classes)
{
    auto works = CopiesToAnswer(catalogue, classes);
    InParallel(works.size(), [&](std::size_t i) { WorkOut(catalogue, names, works[i]); });
    const auto mapNames = MapNames(works);

    Objects out;
    std::set<std::string> written;
    std::map<std::pair<std::string, std::string>, std::string> said;
    for (const Work &work : works)
    {
        const auto named = mapNames.find(work.map.material);
        const std::string mapFile = named == mapNames.end() ? std::string{} : named->second;
        // A copy of the same textures in another campaign may have had none
        // to read, and names its map by the one that did.
        if (!mapFile.empty() && work.map.map && written.insert(mapFile).second)
        {
            out.maps.emplace_back(mapFile, MapFile(*work.map.map));
        }
        if (work.cls == "mixed" && mapFile.empty())
        {
            Count(out, "mixed with no textures: left no metal");
        }
        if (work.another)
        {
            Count(out, "answers for another copy");
        }
        // Every copy has a line, an empty one where it is given nothing: a
        // copy the plugin has no line for is one it works out in play
        // (src/MaterialLearn.cpp), and these need no working out.
        said[{work.name, Hex(work.hash)}] = SaidFor(work.cls, names, mapFile, work.bumps);
    }
    out.answers = kAnswersHeader;
    for (const auto &[key, text] : said)
    {
        out.answers += key.second + " " + key.first + " " + text + "\n";
    }
    Count(out, "answers", said.size());
    Count(out, "maps", out.maps.size());
    return out;
}

} // namespace materials
