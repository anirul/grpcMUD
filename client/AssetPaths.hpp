#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#define WINDOWS_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace grpcmud::client::assetpaths
{
enum class AssetSource
{
    ProjectAsset,
    FrameAsset,
    Missing,
};

struct AssetResolution
{
    std::filesystem::path path;
    AssetSource source = AssetSource::Missing;
    bool exists = false;
};

namespace detail
{
inline std::filesystem::path ResolveExecutablePath()
{
#if defined(_WIN32) || defined(_WIN64)
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true)
    {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            break;
        }
        if (length < buffer.size())
        {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2u);
    }
#else
    std::vector<char> buffer(1024, '\0');
    while (true)
    {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1u);
        if (length < 0)
        {
            break;
        }
        if (static_cast<std::size_t>(length) < (buffer.size() - 1u))
        {
            buffer[static_cast<std::size_t>(length)] = '\0';
            return std::filesystem::path(buffer.data());
        }
        buffer.resize(buffer.size() * 2u, '\0');
    }
#endif
    return std::filesystem::current_path();
}

inline std::filesystem::path Normalize(const std::filesystem::path& path)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
    {
        return path.lexically_normal();
    }
    return absolute.lexically_normal();
}

inline bool Exists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

inline bool IsDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

inline bool IsRegularFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

inline bool IsProjectRoot(const std::filesystem::path& candidate)
{
    return IsRegularFile(candidate / "CMakeLists.txt") &&
           IsDirectory(candidate / "client") &&
           IsDirectory(candidate / "server");
}

template <typename Predicate>
inline std::optional<std::filesystem::path> FindAncestorMatching(
    const std::filesystem::path& start_path,
    Predicate predicate)
{
    auto candidate = Normalize(start_path);
    while (true)
    {
        if (predicate(candidate))
        {
            return candidate;
        }

        const auto parent = candidate.parent_path();
        if (parent.empty() || parent == candidate)
        {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

inline std::optional<std::filesystem::path> FindRootFrom(
    const std::filesystem::path& start_path)
{
    if (const auto root = FindAncestorMatching(start_path, IsProjectRoot))
    {
        return *root;
    }
    if (const auto root = FindAncestorMatching(start_path, [](const auto& candidate) {
            return IsDirectory(candidate / "asset");
        }))
    {
        return *root;
    }
    if (const auto root = FindAncestorMatching(start_path, [](const auto& candidate) {
            return IsDirectory(candidate / "external" / "frame" / "asset");
        }))
    {
        return *root;
    }
    return std::nullopt;
}
} // namespace detail

inline std::filesystem::path ResolveExecutableDirectory()
{
    static const std::filesystem::path executable_directory = [] {
        const auto executable_path = detail::Normalize(detail::ResolveExecutablePath());
        if (detail::IsDirectory(executable_path))
        {
            return executable_path;
        }
        const auto parent = executable_path.parent_path();
        if (parent.empty())
        {
            return detail::Normalize(std::filesystem::current_path());
        }
        return parent.lexically_normal();
    }();
    return executable_directory;
}

inline std::filesystem::path ResolveProjectRoot()
{
    static const std::filesystem::path root = [] {
        if (const auto root = detail::FindRootFrom(ResolveExecutableDirectory()))
        {
            return *root;
        }
        if (const auto root = detail::FindRootFrom(std::filesystem::current_path()))
        {
            return *root;
        }
        return ResolveExecutableDirectory();
    }();
    return root;
}

inline std::filesystem::path ResolveProjectAssetRoot()
{
    static const std::filesystem::path asset_root =
        (ResolveProjectRoot() / "asset").lexically_normal();
    return asset_root;
}

inline std::filesystem::path ResolveFrameAssetRoot()
{
    static const std::filesystem::path frame_asset_root =
        (ResolveProjectRoot() / "external" / "frame" / "asset").lexically_normal();
    return frame_asset_root;
}

inline AssetResolution ResolveAsset(const std::filesystem::path& relative_path)
{
    const auto project_path = (ResolveProjectAssetRoot() / relative_path).lexically_normal();
    if (detail::Exists(project_path))
    {
        return AssetResolution{
            .path = project_path,
            .source = AssetSource::ProjectAsset,
            .exists = true,
        };
    }

    const auto frame_path = (ResolveFrameAssetRoot() / relative_path).lexically_normal();
    if (detail::Exists(frame_path))
    {
        return AssetResolution{
            .path = frame_path,
            .source = AssetSource::FrameAsset,
            .exists = true,
        };
    }

    return AssetResolution{
        .path = project_path,
        .source = AssetSource::Missing,
        .exists = false,
    };
}

inline std::filesystem::path ResolveAssetPath(const std::filesystem::path& relative_path)
{
    return ResolveAsset(relative_path).path;
}

inline bool AssetPathExists(const std::filesystem::path& relative_path)
{
    return ResolveAsset(relative_path).exists;
}

inline std::string_view AssetSourceLabel(AssetSource source)
{
    switch (source)
    {
    case AssetSource::ProjectAsset:
        return "project asset";
    case AssetSource::FrameAsset:
        return "frame asset";
    case AssetSource::Missing:
        return "missing";
    }
    return "unknown";
}
} // namespace grpcmud::client::assetpaths
