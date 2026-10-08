#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <span>
#include <string_view>

import mm.fs;
import mm.fs.local;

namespace filecommander {
namespace {
constexpr std::size_t maximum_entries = 8;
constexpr std::size_t maximum_path = mm::fs::max_path;
struct Entry {
    std::array<char, mm::fs::max_name + 1> name{};
    bool directory = false;
    std::uint64_t size = 0;
};
std::array<Entry, maximum_entries> entries{};
std::array<char, maximum_path + 1> current{ '/', 'd', 'a', 't', 'a', '\0' };
unsigned int listed = 0;
bool more = false;
bool mounted = false;

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > mm::fs::max_name ||
        name == "." || name == "..") return false;
    for (char ch : name) if (ch == '/' || ch == '\\' || ch == '\0' ||
                              static_cast<unsigned char>(ch) < 32) return false;
    return true;
}

mm::fs::Status child_path(std::string_view name, std::array<char, maximum_path + 1>& out) {
    if (!valid_name(name)) return mm::fs::Status::BadArgument;
    const std::size_t parent = std::strlen(current.data());
    const std::size_t length = parent + 1 + name.size();
    if (length > maximum_path) return mm::fs::Status::NameTooLong;
    std::memcpy(out.data(), current.data(), parent);
    out[parent] = '/';
    std::memcpy(out.data() + parent + 1, name.data(), name.size());
    out[length] = 0;
    return mm::fs::Status::Ok;
}

mm::fs::Status existing(unsigned int index,
                        std::array<char, maximum_path + 1>& out) {
    if (index >= listed) return mm::fs::Status::BadArgument;
    return child_path(entries[index].name.data(), out);
}
}

mm::fs::Status initialize(bool format_if_blank) {
    if (mounted) return mm::fs::Status::Ok;
    const auto status = mm::fs::local::mount("/data", {false, format_if_blank});
    if (status == mm::fs::Status::Ok) mounted = true;
    return status;
}

void shutdown() {
    if (mounted) {
        (void)mm::fs::local::unmount("/data");
        mounted = false;
    }
}

bool ready() { return mounted; }
const char* path() { return current.data(); }
unsigned int count() { return listed; }
bool has_more() { return more; }
const char* name(unsigned int index) { return index < listed ? entries[index].name.data() : ""; }
bool directory(unsigned int index) { return index < listed && entries[index].directory; }
std::uint64_t size(unsigned int index) { return index < listed ? entries[index].size : 0; }

mm::fs::Status refresh(unsigned int offset, unsigned int limit) {
    listed = 0;
    more = false;
    if (!mounted) return mm::fs::Status::NotFound;
    if (limit == 0 || limit > maximum_entries) return mm::fs::Status::BadArgument;
    mm::fs::Directory dir;
    auto status = mm::fs::open_directory(current.data(), dir);
    if (status != mm::fs::Status::Ok) return status;
    std::array<char, mm::fs::max_name + 1> filename{};
    for (unsigned int position = 0;; ++position) {
        std::size_t length = 0;
        mm::fs::Stat stat{};
        bool done = false;
        status = dir.next(filename, length, stat, done);
        if (status != mm::fs::Status::Ok || done) break;
        if (position < offset) continue;
        if (listed == limit) { more = true; break; }
        auto& entry = entries[listed++];
        std::memcpy(entry.name.data(), filename.data(), length);
        entry.name[length] = 0;
        entry.directory = stat.kind == mm::fs::Kind::Directory;
        entry.size = stat.size;
    }
    const auto closed = dir.close();
    if (status != mm::fs::Status::Ok) return status;
    return closed;
}

mm::fs::Status enter(unsigned int index) {
    if (!directory(index)) return mm::fs::Status::NotDirectory;
    std::array<char, maximum_path + 1> child{};
    auto status = existing(index, child);
    if (status != mm::fs::Status::Ok) return status;
    std::memcpy(current.data(), child.data(), std::strlen(child.data()) + 1);
    return mm::fs::Status::Ok;
}

mm::fs::Status up() {
    if (std::strcmp(current.data(), "/data") == 0) return mm::fs::Status::BadArgument;
    char* end = std::strrchr(current.data(), '/');
    if (end == nullptr || end <= current.data() + 4) return mm::fs::Status::BadArgument;
    *end = 0;
    return mm::fs::Status::Ok;
}

mm::fs::Status create(std::string_view name, bool as_directory) {
    if (!mounted) return mm::fs::Status::NotFound;
    std::array<char, maximum_path + 1> child{};
    auto status = child_path(name, child);
    if (status != mm::fs::Status::Ok) return status;
    if (as_directory) return mm::fs::make_directory(child.data());
    mm::fs::File file;
    status = mm::fs::open(child.data(), mm::fs::Access::Write,
                          mm::fs::Disposition::CreateNew, file);
    return status == mm::fs::Status::Ok ? file.close() : status;
}

mm::fs::Status rename(unsigned int index, std::string_view new_name) {
    std::array<char, maximum_path + 1> from{}, to{};
    auto status = existing(index, from);
    if (status != mm::fs::Status::Ok) return status;
    status = child_path(new_name, to);
    return status == mm::fs::Status::Ok ? mm::fs::rename(from.data(), to.data()) : status;
}

mm::fs::Status remove(unsigned int index) {
    std::array<char, maximum_path + 1> child{};
    const auto status = existing(index, child);
    return status == mm::fs::Status::Ok ? mm::fs::remove(child.data()) : status;
}

mm::fs::Status read(unsigned int index, std::span<std::byte> output,
                    std::size_t& count) {
    if (directory(index)) return mm::fs::Status::IsDirectory;
    std::array<char, maximum_path + 1> child{};
    auto status = existing(index, child);
    if (status != mm::fs::Status::Ok) return status;
    mm::fs::File file;
    status = mm::fs::open(child.data(), mm::fs::Access::Read,
                          mm::fs::Disposition::OpenExisting, file);
    if (status != mm::fs::Status::Ok) return status;
    status = file.read(output, count);
    const auto closed = file.close();
    return status == mm::fs::Status::Ok ? closed : status;
}

mm::fs::Status append(unsigned int index, std::string_view text) {
    if (directory(index)) return mm::fs::Status::IsDirectory;
    std::array<char, maximum_path + 1> child{};
    auto status = existing(index, child);
    if (status != mm::fs::Status::Ok) return status;
    mm::fs::File file;
    status = mm::fs::open(child.data(), mm::fs::Access::Append,
                          mm::fs::Disposition::OpenExisting, file);
    if (status != mm::fs::Status::Ok) return status;
    std::size_t written = 0;
    status = file.write(std::as_bytes(std::span{text.data(), text.size()}), written);
    const auto closed = file.close();
    if (status != mm::fs::Status::Ok) return status;
    if (written != text.size()) return mm::fs::Status::NoSpace;
    return closed;
}
}
