// A lightmap the relighter wrote (32 bits a pixel, not packed) packed as the
// game keeps its own: elgarashaders_ddspack <in.dds> <out.dds> dxt1|dxt5.
// The relighter's 64-bit build has DAOLib's reader of DDS files and not its
// writer (DirectXTex); the night lightmaps the plugin ships (tools/bake-night.ps1) are
// packed with this, a quarter or an eighth of their size (the user,
// 2026-10-30: "any reason our bake is low-res compared to vanilla? seems to
// me we should be at least as good as vanilla?").
#include "dao/core/Dds.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        std::fputs("usage: elgarashaders_ddspack <in.dds> <out.dds> dxt1|dxt5\n", stderr);
        return 2;
    }
    const std::string_view kind(argv[3]);
    if (kind != "dxt1" && kind != "dxt5")
    {
        std::fputs("the format is dxt1 or dxt5\n", stderr);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::optional<dao::core::dds::Image> image =
        dao::core::dds::Decode(std::as_bytes(std::span(bytes.data(), bytes.size())));
    if (!image)
    {
        std::fprintf(stderr, "%s is not a DDS file read here\n", argv[1]);
        return 1;
    }
    const std::vector<std::byte> packed = dao::core::dds::Encode(
        *image, kind == "dxt1" ? dao::core::dds::Format::Dxt1 : dao::core::dds::Format::Dxt5);
    if (packed.empty())
    {
        std::fprintf(stderr, "%s could not be packed: its sides are not powers of two\n", argv[1]);
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char *>(packed.data()), static_cast<std::streamsize>(packed.size()));
    return out ? 0 : 1;
}
