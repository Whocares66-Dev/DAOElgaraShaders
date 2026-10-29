#include "GpuTimer.h"

#include "dao/Module.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gputimer
{

namespace
{

constexpr std::string_view kFile = "gpu-timing";
constexpr std::size_t kFrames = 6;      // frames in flight before one is read
constexpr std::size_t kMostMarks = 512; // a frame's
constexpr int kLogFrames = 600;

bool g_on = false;

struct Frame
{
    IDirect3DQuery9 *disjoint = nullptr;
    IDirect3DQuery9 *frequency = nullptr;
    std::vector<IDirect3DQuery9 *> stamps;
    std::vector<std::string> names;
    std::vector<int> draws;
    std::size_t marks = 0;
    bool begun = false;
};
std::array<Frame, kFrames> g_frames{};
std::size_t g_now = 0;
bool g_failed = false;

struct Sum
{
    double ms = 0;
    int times = 0;
    int draws = 0;
};
std::map<std::string, Sum> g_sums;
double g_frameMs = 0;
int g_read = 0;
int g_unread = 0;
int g_presented = 0;
std::string g_within;  // the plugin's part running, or none
std::string g_resumed; // the last mark made outside it

void Let(IDirect3DQuery9 *&query)
{
    if (query != nullptr)
    {
        query->Release();
        query = nullptr;
    }
}

void LetFrame(Frame &frame)
{
    for (IDirect3DQuery9 *&query : frame.stamps)
    {
        Let(query);
    }
    Let(frame.disjoint);
    Let(frame.frequency);
    frame = Frame{};
}

// An older frame's answers added to the sums, if the GPU has them all.
void Read(Frame &frame)
{
    if (!frame.begun)
    {
        return;
    }
    frame.begun = false;
    BOOL disjoint = TRUE;
    UINT64 frequency = 0;
    if (frame.disjoint->GetData(&disjoint, sizeof disjoint, 0) != S_OK || disjoint != FALSE ||
        frame.frequency->GetData(&frequency, sizeof frequency, 0) != S_OK || frequency == 0)
    {
        ++g_unread;
        return;
    }
    std::vector<UINT64> at(frame.marks);
    for (std::size_t i = 0; i < frame.marks; ++i)
    {
        if (frame.stamps.at(i)->GetData(&at.at(i), sizeof(UINT64), 0) != S_OK)
        {
            ++g_unread;
            return;
        }
    }
    const auto ms = [&](UINT64 from, UINT64 to) {
        return 1000.0 * static_cast<double>(to - from) / static_cast<double>(frequency);
    };
    for (std::size_t i = 0; i + 1 < frame.marks; ++i)
    {
        Sum &sum = g_sums[frame.names.at(i)];
        sum.ms += ms(at.at(i), at.at(i + 1));
        ++sum.times;
        sum.draws += frame.draws.at(i);
    }
    g_frameMs += frame.marks > 1 ? ms(at.front(), at.back()) : 0;
    ++g_read;
}

void Log()
{
    if (g_read == 0)
    {
        spdlog::info("gpu timing: no frame read ({} unread)", g_unread);
        return;
    }
    std::vector<std::pair<std::string, Sum>> parts(g_sums.begin(), g_sums.end());
    std::ranges::sort(parts, [](const auto &a, const auto &b) { return a.second.ms > b.second.ms; });
    std::string lines;
    for (const auto &[name, sum] : parts)
    {
        lines += fmt::format("\n    {:7.3f} ms, {:5.1f} times, {:6.1f} draws: {}", sum.ms / g_read,
                             static_cast<double>(sum.times) / g_read, static_cast<double>(sum.draws) / g_read, name);
    }
    spdlog::info("gpu timing: {} frames read ({} unread), {:.3f} ms a frame from its first mark to its present; a "
                 "frame's mean by part:{}",
                 g_read, g_unread, g_frameMs / g_read, lines);
    g_sums.clear();
    g_frameMs = 0;
    g_read = 0;
    g_unread = 0;
}

} // namespace

void Start()
{
    std::error_code error;
    g_on = std::filesystem::exists(dao::Module::OwnFolder() / kFile, error);
    if (g_on)
    {
        spdlog::info("gpu timing: on, by the file {} (a developer's: no player's build has it)", kFile);
    }
}

bool On()
{
    return g_on && !g_failed;
}

void Mark(IDirect3DDevice9 *device, std::string_view name)
{
    if (!On() || device == nullptr)
    {
        return;
    }
    Frame &frame = g_frames.at(g_now);
    if (frame.marks >= kMostMarks)
    {
        return;
    }
    if (frame.disjoint == nullptr && (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &frame.disjoint)) ||
                                      FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frame.frequency))))
    {
        spdlog::warn("gpu timing: the GPU's timestamps cannot be had");
        Release();
        g_failed = true;
        return;
    }
    if (frame.stamps.size() <= frame.marks)
    {
        IDirect3DQuery9 *query = nullptr;
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &query)))
        {
            spdlog::warn("gpu timing: a timestamp query cannot be made");
            Release();
            g_failed = true;
            return;
        }
        frame.stamps.push_back(query);
    }
    if (!frame.begun)
    {
        frame.disjoint->Issue(D3DISSUE_BEGIN);
        frame.begun = true;
    }
    frame.stamps.at(frame.marks)->Issue(D3DISSUE_END);
    if (frame.names.size() <= frame.marks)
    {
        frame.names.resize(frame.marks + 1);
        frame.draws.resize(frame.marks + 1);
    }
    frame.names.at(frame.marks) = g_within.empty() ? std::string(name) : g_within + ": " + std::string(name);
    if (g_within.empty())
    {
        g_resumed = std::string(name);
    }
    frame.draws.at(frame.marks) = 0;
    ++frame.marks;
}

void Begin(IDirect3DDevice9 *device, std::string_view name)
{
    if (!On())
    {
        return;
    }
    g_within.clear();
    const std::string resumed = g_resumed;
    g_within = std::string(name);
    Mark(device, "(begun)");
    g_resumed = resumed;
}

void End(IDirect3DDevice9 *device)
{
    if (!On())
    {
        return;
    }
    g_within.clear();
    Mark(device, g_resumed + " (again)");
}

void Drawn()
{
    if (!On())
    {
        return;
    }
    Frame &frame = g_frames.at(g_now);
    if (frame.marks > 0)
    {
        ++frame.draws.at(frame.marks - 1);
    }
}

void Presented(IDirect3DDevice9 *device)
{
    if (!On())
    {
        return;
    }
    Mark(device, "(present)");
    Frame &frame = g_frames.at(g_now);
    if (frame.begun)
    {
        frame.disjoint->Issue(D3DISSUE_END);
        frame.frequency->Issue(D3DISSUE_END);
    }
    g_now = (g_now + 1) % kFrames;
    Read(g_frames.at(g_now));
    g_frames.at(g_now).marks = 0;
    if (++g_presented % kLogFrames == 0)
    {
        Log();
    }
}

void Release()
{
    for (Frame &frame : g_frames)
    {
        LetFrame(frame);
    }
}

} // namespace gputimer
