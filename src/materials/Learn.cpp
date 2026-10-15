#include "materials/Learn.h"

#include "materials/Said.h"

#include "dao/core/Dds.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace materials
{

std::optional<Picture> PictureOf(std::span<const std::byte> bytes)
{
    const auto image = dao::core::dds::Decode(bytes);
    if (!image)
    {
        return std::nullopt;
    }
    return FromRgba8(image->width, image->height, image->rgba);
}

std::vector<std::byte> MapFile(const Picture &grey)
{
    dao::core::dds::Image image{grey.width, grey.height, {}};
    image.rgba.resize(static_cast<std::size_t>(grey.width) * grey.height * 4);
    for (std::size_t i = 0; i < grey.values.size(); ++i)
    {
        const auto v = static_cast<std::uint8_t>(std::lround(std::clamp(grey.values[i], 0.0F, 1.0F) * 255.0F));
        image.rgba[(i * 4) + 0] = v;
        image.rgba[(i * 4) + 1] = v;
        image.rgba[(i * 4) + 2] = v;
        image.rgba[(i * 4) + 3] = 255;
    }
    return dao::core::dds::Encode(image, dao::core::dds::Format::Dxt1);
}

std::optional<MetalMap> MapOf(const MaterialObject &object, const ImageOf &image)
{
    const std::string specular = object.Texture("mml_tSpecularMask");
    const std::string diffuse = object.Texture("mml_tDiffuse");
    if (specular.empty() || diffuse.empty())
    {
        return std::nullopt;
    }
    const auto d = image(diffuse);
    const auto s = image(specular);
    if (!d || !s)
    {
        return std::nullopt;
    }
    return MakeMetalMap(*d, *s, object.Plate());
}

std::optional<Bumps> BumpsOf(const MaterialObject &object, const ImageOf &image)
{
    const std::string normal = object.Texture("mml_tNormalMap");
    const auto picture = normal.empty() ? std::nullopt : image(normal);
    if (!picture)
    {
        return std::nullopt;
    }
    return MeasureBumps(*picture);
}

Classed Classify(const MaterialObject &object, const Names &names, const ImageOf &image)
{
    Classed out;
    out.verdict = names.ByName(object);
    bool ragged = false;
    if (out.verdict.cls == kPaint)
    {
        if (auto map = MapOf(object, image))
        {
            out.share = map->share;
            ragged = Ragged(map->metal);
            out.map = std::move(map->metal);
        }
    }
    out.cls = Settle(out.verdict, out.share, ragged);
    if (out.cls != "mixed")
    {
        out.map.reset();
    }
    return out;
}

std::optional<Learned> Learn(const std::string &name, std::string_view text, const Names &names, const ImageOf &image,
                             const std::string &mapFile)
{
    const auto object = ReadMaterialObject(name, text);
    if (!object || !object->Solid())
    {
        return std::nullopt;
    }
    Classed classed = Classify(*object, names, image);
    Learned out;
    out.cls = classed.cls;
    if (classed.map)
    {
        out.mapFile = mapFile;
        out.map = MapFile(*classed.map);
    }
    out.said = SaidFor(out.cls, names, out.mapFile, BumpsOf(*object, image));
    return out;
}

} // namespace materials
