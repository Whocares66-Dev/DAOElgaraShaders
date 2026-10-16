// Items.h has what this is for. Every file is read through Files: for the
// table's tool a campaign's own copy, else another's, else the folder of the
// add-ins' files read out of the running game (tools/dump_addins.py --types
// uti,gda,mmh), since the add-ins' archives are enciphered; for the plugin
// the install as it is on disk, where a player's mods are in the clear.
#include "materials/Items.h"

#include "dao/core/GdaFile.h"
#include "dao/core/GdaHash.h"
#include "dao/core/Gff3.h"
#include "dao/core/Gff4.h"
#include "dao/core/Install.h"
#include "dao/core/M2da.h"
#include "dao/core/Model.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string_view>
#include <system_error>
#include <variant>

namespace materials
{
namespace
{

using dao::core::GdaFile;
using dao::core::install::Index;
namespace m2da = dao::core::m2da;

std::string Lowered(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Narrow(const std::wstring &text)
{
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text)
    {
        out.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return out;
}

// A merged table's rows by ID, a cell by column name. Made where it is kept
// (Tables' map) and never moved.
class Table
{
  public:
    Table() = default;
    explicit Table(m2da::Merged merged) : merged_(std::move(merged))
    {
        for (std::size_t i = 0; i < merged_.rows.size(); ++i)
        {
            const auto &row = merged_.rows[i];
            if (row.taken && row.id)
            {
                byId_.emplace(*row.id, i);
            }
        }
    }
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;
    Table(Table &&) = delete;
    Table &operator=(Table &&) = delete;
    ~Table() = default;

    [[nodiscard]] bool Has(std::int64_t id) const
    {
        return byId_.contains(id);
    }
    [[nodiscard]] std::vector<std::int64_t> Ids() const
    {
        std::vector<std::int64_t> out;
        out.reserve(byId_.size());
        for (const auto &[id, ignored] : byId_)
        {
            out.push_back(id);
        }
        return out;
    }
    [[nodiscard]] std::int64_t Int(std::int64_t id, std::string_view column) const
    {
        const auto *cell = Cell(id, column);
        if (cell == nullptr)
        {
            return 0;
        }
        if (const auto *whole = std::get_if<std::int64_t>(cell))
        {
            return *whole;
        }
        if (const auto *real = std::get_if<double>(cell))
        {
            return static_cast<std::int64_t>(*real);
        }
        return 0;
    }
    [[nodiscard]] std::string Text(std::int64_t id, std::string_view column) const
    {
        const auto *cell = Cell(id, column);
        const auto *text = cell == nullptr ? nullptr : std::get_if<std::wstring>(cell);
        return text == nullptr ? std::string() : Narrow(*text);
    }

  private:
    [[nodiscard]] const GdaFile::Cell *Cell(std::int64_t id, std::string_view column) const
    {
        const auto row = byId_.find(id);
        if (row == byId_.end())
        {
            return nullptr;
        }
        const std::uint32_t hash = dao::core::GdaColumnHash(column);
        const auto at = std::ranges::find(merged_.columns, hash, &GdaFile::Column::hash);
        if (at == merged_.columns.end())
        {
            return nullptr;
        }
        return &merged_.rows[row->second].cells[static_cast<std::size_t>(at - merged_.columns.begin())];
    }

    m2da::Merged merged_;
    std::map<std::int64_t, std::size_t> byId_;
};

// One campaign's tables, merged as its game merges them.
class Tables
{
  public:
    Tables(const Files &files, const std::string &campaign, const Index &index)
        : files_(files), campaign_(campaign), index_(index)
    {
        const auto base = files_.bytes("m2da_base.gda", campaign_);
        const auto table = base ? GdaFile::Parse(*base) : std::nullopt;
        if (table)
        {
            worksheets_ = m2da::Worksheets(*table, true);
        }
    }

    // The table whose files' names start with `stem` (a worksheet's prefix).
    const Table &ByStem(const std::string &stem)
    {
        const auto known = byStem_.find(stem);
        if (known != byStem_.end())
        {
            return known->second;
        }
        return byStem_.try_emplace(stem, Merge(stem)).first->second;
    }

    // The table of a worksheet's ID (bitm_base's Variation_Worksheet).
    const Table &ById(std::int64_t id)
    {
        const auto worksheet = std::ranges::find(worksheets_, id, &m2da::Worksheet::id);
        if (worksheet == worksheets_.end())
        {
            static const Table none;
            return none;
        }
        return ByStem(Narrow(worksheet->prefix));
    }

  private:
    m2da::Merged Merge(const std::string &stem)
    {
        m2da::Worksheet worksheet;
        worksheet.prefix = std::wstring(stem.begin(), stem.end());
        std::vector<dao::core::install::NamedCopy> files;
        for (const auto &[file, copies] : index_.Resources())
        {
            if (!file.ends_with(".gda") || copies.empty())
            {
                continue;
            }
            const std::string name = file.substr(0, file.size() - 4);
            if (m2da::Takes(worksheet, std::wstring(name.begin(), name.end())))
            {
                files.push_back({file, copies.front()});
            }
        }
        index_.SortFiles(files);
        std::vector<GdaFile> parsed;
        parsed.reserve(files.size());
        for (const auto &file : files)
        {
            const auto bytes = files_.bytes(file.name, campaign_);
            auto table = bytes ? GdaFile::Parse(*bytes) : std::nullopt;
            if (table)
            {
                parsed.push_back(std::move(*table));
            }
        }
        std::vector<const GdaFile *> parts;
        parts.reserve(parsed.size());
        for (const auto &table : parsed)
        {
            parts.push_back(&table);
        }
        return m2da::Merge(parts);
    }

    const Files &files_;
    const std::string &campaign_;
    const Index &index_;
    std::vector<m2da::Worksheet> worksheets_;
    std::map<std::string, Table> byStem_;
};

// The material objects a model's meshes name, from the model hierarchies of
// it its wearers are drawn with (Wearers).
class Models
{
  public:
    explicit Models(const Files &files) : files_(files)
    {
    }

    // `prefixes` before the model's name make its files: "w_" for a held
    // thing's, "hm_", "ef_" ... for a part a figure wears; and each of its
    // levels of detail, _0 to _3, since the far ones are drawn with material
    // objects of their own (pn_hlf_gwra_l2 in hm_hlf_gwra_2.mmh), and the game
    // may read one of those first (the Runic helm, 2026-10-16).
    std::vector<std::string> ObjectsOf(const std::vector<std::string> &prefixes, const std::string &model,
                                       const std::string &campaign)
    {
        std::set<std::string> out;
        for (const auto &prefix : prefixes)
        {
            for (const char *level : {"_0.mmh", "_1.mmh", "_2.mmh", "_3.mmh"})
            {
                const auto &objects = Objects(prefix + model + level, campaign);
                out.insert(objects.begin(), objects.end());
            }
        }
        return {out.begin(), out.end()};
    }

    // By material object, the full detail's mesh chunks read so far.
    std::vector<Drawn> TakeMeshes()
    {
        std::vector<Drawn> out;
        out.reserve(meshes_.size());
        for (const auto &[object, chunks] : meshes_)
        {
            out.push_back({object, {chunks.begin(), chunks.end()}});
        }
        meshes_.clear();
        return out;
    }

  private:
    const std::set<std::string> &Objects(const std::string &file, const std::string &campaign)
    {
        const auto known = read_.find(file);
        if (known != read_.end())
        {
            return known->second;
        }
        std::set<std::string> objects;
        const auto bytes = files_.bytes(file, campaign);
        const auto gff = bytes ? dao::core::gff4::File::Parse(*bytes) : std::nullopt;
        const auto hierarchy = gff ? dao::core::model::HierarchyOf(*gff) : std::nullopt;
        if (!hierarchy)
        {
            return read_.emplace(file, std::move(objects)).first->second;
        }
        const std::string meshFile = Lowered(Narrow(hierarchy->meshFile));
        for (const auto &node : hierarchy->nodes)
        {
            if (!node.mesh || node.mesh->material.empty())
            {
                continue;
            }
            const std::string object = Lowered(Narrow(node.mesh->material));
            objects.insert(object);
            // the full detail's chunks: the texture's islands are read from them
            if (file.ends_with("_0.mmh"))
            {
                meshes_[object].insert({meshFile, Narrow(node.mesh->chunk)});
            }
        }
        return read_.emplace(file, std::move(objects)).first->second;
    }

    const Files &files_;
    std::map<std::string, std::set<std::string>> read_;
    std::map<std::string, std::set<Chunk>> meshes_;
};

// Who draws a base item's model, so the files it is drawn from: bitm_base
// says where it is carried. Held in a hand or on the back (WieldR, WieldL,
// WearR, WearL: weapons, shields, staves, bows, ammunition), a weapon's model,
// w_<model>. Worn in a figure's chest, head, feet or hands slot
// (EquippableSlots 16, 32, 64, 128), a part of each figure made of parts:
// apr_base's appearances of ModelType P, a model for each race (ModelRace)
// and gender, hm_<model>, ef_<model> ... The dog's collar and warpaint go in
// slots of its own (262144, 524288) and the dog is a model whole (C_DOGa):
// they name a variation but no part is drawn from it (no file is named so
// but the figures', read 2026-10-16), so none.
std::vector<std::string> Wearers(const Table &bases, std::int64_t baseItem, const std::vector<std::string> &figures)
{
    const bool held = bases.Int(baseItem, "WieldR") != 0 || bases.Int(baseItem, "WieldL") != 0 ||
                      bases.Int(baseItem, "WearR") != 0 || bases.Int(baseItem, "WearL") != 0;
    if (held)
    {
        return {"w_"};
    }
    constexpr std::int64_t kFigureSlots = 16 | 32 | 64 | 128;
    if ((bases.Int(baseItem, "EquippableSlots") & kFigureSlots) != 0)
    {
        return figures;
    }
    return {};
}

// The prefixes of the figures made of parts: a race's letter and a gender's.
std::vector<std::string> Figures(const Table &appearances)
{
    std::set<std::string> races;
    for (const std::int64_t id : appearances.Ids())
    {
        const std::string race = Lowered(appearances.Text(id, "ModelRace"));
        if (Lowered(appearances.Text(id, "ModelType")) == "p" && race.size() == 1)
        {
            races.insert(race);
        }
    }
    std::vector<std::string> out;
    out.reserve(races.size() * 2);
    for (const auto &race : races)
    {
        out.push_back(race + "m_");
        out.push_back(race + "f_");
    }
    return out;
}

// An integer field, of whichever of GFF's integer types the template keeps it as.
std::int64_t Field(const dao::core::gff3::Struct &root, std::string_view label)
{
    if (const auto value = root.Get<std::int32_t>(label))
    {
        return *value;
    }
    if (const auto value = root.Get<std::uint32_t>(label))
    {
        return *value;
    }
    if (const auto value = root.Get<std::uint16_t>(label))
    {
        return *value;
    }
    if (const auto value = root.Get<std::int16_t>(label))
    {
        return *value;
    }
    if (const auto value = root.Get<std::uint8_t>(label))
    {
        return *value;
    }
    return 0;
}

// The item templates a campaign has: its index's, and those read out of the game.
std::set<std::string> ItemNames(const Files &files, const Index &index)
{
    std::set<std::string> out;
    for (const auto &[file, ignored] : index.Resources())
    {
        if (file.ends_with(".uti"))
        {
            out.insert(file);
        }
    }
    std::error_code failed;
    for (const auto &entry : std::filesystem::directory_iterator(files.readOut, failed))
    {
        const std::string file = Lowered(entry.path().filename().string());
        if (file.ends_with(".uti"))
        {
            out.insert(file);
        }
    }
    return out;
}

// A campaign's tables an item is read against.
struct Campaign
{
    const std::string &name;
    Tables &tables;
    const Table &bases;
    std::vector<std::string> figures;
};

// An item template read: its kind and its model's material objects; none where it is no GFF.
std::optional<Item> ItemOf(const Files &files, const std::string &file, const Campaign &campaign, Models &models)
{
    const auto bytes = files.bytes(file, campaign.name);
    const auto gff = bytes ? dao::core::gff3::File::Parse(*bytes) : std::nullopt;
    const auto root = gff ? gff->Root() : std::nullopt;
    if (!root)
    {
        return std::nullopt;
    }
    const std::int64_t base = Field(*root, "BaseItem");
    const std::int64_t variation = Field(*root, "ModelVariation");
    Item item;
    item.name = file.substr(0, file.size() - 4);
    item.kind = KindOf({campaign.bases.Int(base, "Type"), campaign.bases.Int(base, "ArmorType"),
                        campaign.bases.Int(base, "EquippableSlots")});
    const Table &variations = campaign.tables.ById(campaign.bases.Int(base, "Variation_Worksheet"));
    const std::string type = variations.Has(variation) ? Lowered(variations.Text(variation, "ModelType")) : "";
    if (!type.empty())
    {
        item.model = type;
        item.model += '_';
        item.model += Lowered(variations.Text(variation, "ModelSubType"));
        item.model += Lowered(variations.Text(variation, "ModelVariation"));
        item.objects = models.ObjectsOf(Wearers(campaign.bases, base, campaign.figures), item.model, campaign.name);
    }
    return item;
}

} // namespace

Inventory TakeInventory(const Files &files)
{
    Inventory out;
    std::set<std::string> done;
    Models models(files);
    for (const auto &[campaign, index] : files.campaigns)
    {
        Tables tables(files, campaign, *index);
        const Campaign here{campaign, tables, tables.ByStem("bitm_"), Figures(tables.ByStem("apr_"))};
        for (const auto &file : ItemNames(files, *index))
        {
            // An item the main campaign has is its; another campaign's own items are theirs.
            if (done.contains(file))
            {
                continue;
            }
            if (auto item = ItemOf(files, file, here, models))
            {
                done.insert(file);
                out.items.push_back(std::move(*item));
            }
        }
    }
    out.meshes = models.TakeMeshes();
    return out;
}

std::vector<UvMesh> UvMeshesOf(const Files &files, const std::vector<Chunk> &chunks)
{
    std::vector<UvMesh> out;
    const std::string main = files.campaigns.empty() ? std::string() : files.campaigns.front().first;
    for (const auto &[file, chunkName] : chunks)
    {
        // the main campaign's copy, else another's (Files::bytes looks in each)
        const auto bytes = files.bytes(file, main);
        const auto gff = bytes ? dao::core::gff4::File::Parse(*bytes) : std::nullopt;
        const auto mesh = gff ? dao::core::model::MeshOf(*gff) : std::nullopt;
        for (const auto &chunk : mesh ? mesh->chunks : std::vector<dao::core::model::Chunk>{})
        {
            const auto *uv = chunk.Find(dao::core::model::Usage::TexCoord, 0);
            if (Narrow(chunk.name) != chunkName || uv == nullptr)
            {
                continue;
            }
            UvMesh one;
            const auto values = dao::core::model::Floats(chunk, *uv);
            one.uv.reserve(values.size());
            for (const auto &value : values)
            {
                one.uv.push_back({value[0], value[1]});
            }
            one.triangles = dao::core::model::Indices(chunk);
            out.push_back(std::move(one));
        }
    }
    return out;
}

std::map<std::string, Kind> KindsOf(const Inventory &inventory)
{
    std::map<std::string, std::set<Kind>> said;
    for (const auto &item : inventory.items)
    {
        for (const auto &object : item.objects)
        {
            said[object].insert(item.kind);
        }
    }
    std::map<std::string, Kind> out;
    for (const auto &[object, kinds] : said)
    {
        // the kind its items agree on; none where they differ (an object two kinds draw with)
        out.emplace(object, kinds.size() == 1 ? *kinds.begin() : Kind::None);
    }
    return out;
}

} // namespace materials
