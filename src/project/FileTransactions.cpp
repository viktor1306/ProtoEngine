#include "project/FileTransactions.hpp"
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include "scene/SceneIO.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace proto {
namespace {
namespace fs = std::filesystem;
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value)
            CloseHandle(value);
    }
};
[[noreturn]] void ioError(const std::string& text) {
    throw std::runtime_error(text + " (Windows " + std::to_string(GetLastError()) + ")");
}
bool noise(const std::string& name) {
    auto key = projectPathKey(name);
    return key == ".PROTO" || key.ends_with(".BAK") || key.ends_with(".LOCK") || key.find(".TMP-") != std::string::npos;
}
std::string relativeText(const fs::path& value) {
    return utf8(value.generic_wstring());
}
void checkComponent(const std::wstring& part) {
    if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
        part.find_first_of(L"\\/<>:\"|?*") != std::wstring::npos)
        throw std::runtime_error("Некоректний компонент шляху проєкту");
    for (auto c : part)
        if (c < 32)
            throw std::runtime_error("Керівний символ у шляху");
    auto base = part.substr(0, part.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(),
                   [](wchar_t c) { return c >= L'a' && c <= L'z' ? wchar_t(c - 32) : c; });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
        (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) && base[3] >= L'1' &&
         base[3] <= L'9'))
        throw std::runtime_error("Зарезервована назва Windows");
}
void noReparse(const fs::path& path) {
    const auto attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return;
        ioError("Не вдалося перевірити шлях: " + utf8(path.wstring()));
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::runtime_error("Посилання та reparse points у проєкті не підтримуються: " + utf8(path.wstring()));
}
FileStamp fileStamp(const fs::path& path) {
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        ioError("Не вдалося прочитати файл: " + utf8(path.wstring()));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info))
        ioError("Не вдалося перевірити файл");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::runtime_error("Файл є reparse point");
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 initialization failed");
    struct HashGuard {
        BCRYPT_ALG_HANDLE& a;
        BCRYPT_HASH_HANDLE& h;
        ~HashGuard() {
            if (h)
                BCryptDestroyHash(h);
            if (a)
                BCryptCloseAlgorithmProvider(a, 0);
        }
    } cleanup{algorithm, hash};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("SHA256 initialization failed");
    std::array<uint8_t, 65536> buffer{};
    uint64_t size{};
    DWORD read{};
    for (;;) {
        if (!ReadFile(file.value, buffer.data(), DWORD(buffer.size()), &read, nullptr))
            ioError("Неповне читання файла");
        if (!read)
            break;
        size += read;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0)
            throw std::runtime_error("SHA256 failed");
    }
    std::array<uint8_t, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), DWORD(digest.size()), 0) < 0)
        throw std::runtime_error("SHA256 failed");
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return {FileKind::File, size, hex};
}
using Children = std::map<std::string, FileKind>;
Children children(const fs::path& root, const std::string& relative) {
    Children result;
    const auto path = projectPath(root, relative);
    if (!fs::exists(path))
        return result;
    if (!fs::is_directory(path))
        throw std::runtime_error("Очікувалася папка: " + relative);
    for (const auto& entry : fs::directory_iterator(path)) {
        const auto name = relativeText(entry.path().filename());
        if (noise(name))
            continue;
        noReparse(entry.path());
        if (!entry.is_regular_file() && !entry.is_directory())
            throw std::runtime_error("Непідтримуваний тип файла");
        result.emplace(name, entry.is_directory() ? FileKind::Directory : FileKind::File);
    }
    return result;
}
FileStamp directoryStamp(const Children& entries) {
    std::string value;
    for (const auto& [name, kind] : entries)
        value += jsonString(name) + ":" + std::to_string(int(kind)) + "\n";
    return {FileKind::Directory, uint64_t(entries.size()), sha256(value)};
}
void flushBytes(const fs::path& path, std::string_view bytes) {
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        ioError("Не вдалося створити staged-файл");
    size_t offset{};
    while (offset < bytes.size()) {
        DWORD count{};
        const auto amount = DWORD(std::min<size_t>(65536, bytes.size() - offset));
        if (!WriteFile(file.value, bytes.data() + offset, amount, &count, nullptr) || count != amount)
            ioError("Неповний запис staged-файла");
        offset += count;
    }
    if (!FlushFileBuffers(file.value))
        ioError("Не вдалося зберегти staged-файл");
}
void copyFlushed(const fs::path& source, const fs::path& target) {
    noReparse(source);
    Handle input{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE)
        ioError("Файл змінюється іншою програмою: " + utf8(source.wstring()));
    Handle output{CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
    if (output.value == INVALID_HANDLE_VALUE)
        ioError("Не вдалося створити резервну копію");
    std::array<uint8_t, 65536> buffer{};
    for (;;) {
        DWORD read{}, written{};
        if (!ReadFile(input.value, buffer.data(), DWORD(buffer.size()), &read, nullptr))
            ioError("Неповне читання резервної копії");
        if (!read)
            break;
        if (!WriteFile(output.value, buffer.data(), read, &written, nullptr) || read != written)
            ioError("Неповний запис резервної копії");
    }
    if (!FlushFileBuffers(output.value))
        ioError("Не вдалося зберегти резервну копію");
}
std::string parentOf(const std::string& relative) {
    return relativeText(utf8Path(relative).parent_path());
}
std::string nameOf(const std::string& relative) {
    return relativeText(utf8Path(relative).filename());
}
bool samePath(const std::string& a, const std::string& b) {
    return projectPathKey(a) == projectPathKey(b);
}
size_t depthOf(const std::string& path) {
    return size_t(std::count(path.begin(), path.end(), '/'));
}
std::string stampJson(const FileStamp& s) {
    return "{\"kind\":" + std::to_string(int(s.kind)) + ",\"size\":" + std::to_string(s.size) +
           ",\"hash\":" + jsonString(s.hash) + "}";
}
FileStamp parseStamp(Json* j) {
    const auto kind = num(get(j, "kind")), size = num(get(j, "size"));
    if (kind < 0 || kind > 2 || kind != int(kind) || size < 0 || size > 9007199254740991.0)
        throw std::runtime_error("Некоректний журнал: версія файла");
    return {FileKind(int(kind)), uint64_t(size), str(get(j, "hash"))};
}
} // namespace

std::string projectPathKey(const std::string& value) {
    const auto input = utf8Path(value).generic_wstring();
    if (input.empty())
        return {};
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, input.data(), int(input.size()), nullptr, 0,
                                    nullptr, nullptr, 0);
    if (!count)
        ioError("Не вдалося нормалізувати шлях");
    std::wstring result(size_t(count), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, input.data(), int(input.size()), result.data(), count,
                       nullptr, nullptr, 0))
        ioError("Не вдалося нормалізувати шлях");
    return utf8(result);
}
fs::path projectPath(const fs::path& root, const std::string& relative, bool internal) {
    const auto base = fs::absolute(root).lexically_normal();
    noReparse(base);
    if (relative.empty() || relative == ".")
        return base;
    if (relative.find('\\') != std::string::npos)
        throw std::runtime_error("Використовуйте відносний шлях із '/' ");
    const auto path = utf8Path(relative);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        throw std::runtime_error("Шлях виходить за межі проєкту");
    auto resolved = base;
    for (const auto& part : path) {
        checkComponent(part.wstring());
        if (!internal && projectPathKey(relativeText(part)) == ".PROTO")
            throw std::runtime_error("Службова папка недоступна для файлових операцій");
        resolved /= part;
        noReparse(resolved);
    }
    return resolved;
}
std::string projectRelative(const fs::path& root, const fs::path& path) {
    const auto base = fs::absolute(root).lexically_normal(), absolute = fs::absolute(path).lexically_normal();
    auto a = absolute.begin(), b = base.begin();
    for (; b != base.end(); ++a, ++b)
        if (a == absolute.end() || !samePath(relativeText(*a), relativeText(*b)))
            throw std::runtime_error("Файл поза проєктом");
    fs::path result;
    for (; a != absolute.end(); ++a)
        result /= *a;
    const auto text = relativeText(result);
    projectPath(base, text);
    return text;
}
bool projectVisible(const std::string& relative) {
    for (const auto& part : utf8Path(relative)) {
        const auto name = relativeText(part);
        if (noise(name) || projectPathKey(name).ends_with(".META"))
            return false;
    }
    return true;
}
FileStamp projectStamp(const fs::path& root, const std::string& relative) {
    const auto path = projectPath(root, relative);
    const auto status = fs::symlink_status(path);
    if (status.type() == fs::file_type::not_found)
        return {};
    if (fs::is_directory(status))
        return directoryStamp(children(root, relative));
    if (fs::is_regular_file(status))
        return fileStamp(path);
    throw std::runtime_error("Непідтримуваний тип файла: " + relative);
}

struct FileTransaction::State {
    struct Entry {
        std::string beforePath, afterPath;
        FileStamp before, after;
        std::string beforeBlob, afterBlob;
    };
    fs::path root, directory;
    std::string label, status{"ready"};
    std::vector<Entry> entries;
    std::vector<FileGuard> guards;
    std::vector<PathMove> moves;
    std::vector<std::shared_ptr<Handle>> pins;
    size_t progress{};
    bool durable{};
    void pinJournal() {
        for (const auto& part : std::vector<std::string>{"", ".proto", ".proto/transactions",
                                                         ".proto/transactions/" + relativeText(directory.filename())}) {
            auto path = projectPath(root, part, true);
            auto handle = std::make_shared<Handle>();
            handle->value =
                CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle->value == INVALID_HANDLE_VALUE)
                ioError("Не вдалося закріпити папку журналу");
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle->value, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Некоректна папка журналу");
            pins.push_back(std::move(handle));
        }
    }
    void save() const {
        projectPath(root, ".proto/transactions/" + relativeText(directory.filename()), true);
        std::string json = "{\"format\":\"proto.transaction\",\"formatVersion\":1,\"label\":" + jsonString(label) +
                           ",\"state\":" + jsonString(status) + ",\"progress\":" + std::to_string(progress) +
                           ",\"entries\":[";
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            if (i)
                json += ',';
            json += "{\"beforePath\":" + jsonString(e.beforePath) + ",\"afterPath\":" + jsonString(e.afterPath) +
                    ",\"before\":" + stampJson(e.before) + ",\"after\":" + stampJson(e.after) +
                    ",\"beforeBlob\":" + jsonString(e.beforeBlob) + ",\"afterBlob\":" + jsonString(e.afterBlob) + "}";
        }
        json += "],\"moves\":[";
        for (size_t i = 0; i < moves.size(); ++i) {
            if (i)
                json += ',';
            json += "{\"before\":" + jsonString(moves[i].before) + ",\"after\":" + jsonString(moves[i].after) + "}";
        }
        json += "]}";
        if (json.size() > 32u * 1024 * 1024)
            throw std::runtime_error("Журнал перевищує 32 MiB");
        atomicWrite(directory / "journal.json", json);
    }
    void check(bool after) const {
        for (const auto& e : entries) {
            const auto& path = after ? e.afterPath : e.beforePath;
            if (projectStamp(root, path) != (after ? e.after : e.before))
                throw std::runtime_error("Файловий конфлікт: " + path +
                                         " змінено після операції. Дані залишено без змін.");
        }
    }
    void transition(bool forward, bool recovering, const std::function<void(size_t)>& hook = {});
};

namespace {
std::string remapCase(const std::string& path, const std::vector<PathMove>& moves, bool reverse) {
    std::string result = path;
    size_t longest{};
    for (const auto& m : moves) {
        if (!samePath(m.before, m.after))
            continue;
        const auto& from = reverse ? m.after : m.before;
        const auto& to = reverse ? m.before : m.after;
        if (from.size() < longest)
            continue;
        const auto key = projectPathKey(path), prefix = projectPathKey(from);
        if (key == prefix || (key.starts_with(prefix + "/"))) {
            result = to + path.substr(from.size());
            longest = from.size();
        }
    }
    return result;
}
void removeJournal(const fs::path& root, const fs::path& directory) {
    const auto expected = projectPath(root, ".proto/transactions", true);
    if (directory.parent_path() != expected || directory.filename().empty())
        throw std::runtime_error("Unsafe journal cleanup path");
    Uuid::parse(relativeText(directory.filename()));
    noReparse(directory);
    if (!fs::exists(directory))
        return;
    for (const auto& e : fs::recursive_directory_iterator(directory))
        noReparse(e.path());
    fs::remove_all(directory);
}
} // namespace

FileTransaction::FileTransaction(std::shared_ptr<State> state) : state_(std::move(state)) {}
FileTransaction::~FileTransaction() {
    if (!state_ || (state_->status != "ready" && state_->status != "committed" && state_->status != "undone"))
        return;
    try {
        state_->pins.clear();
        removeJournal(state_->root, state_->directory);
    } catch (...) { /* Keep recoverable backups if cleanup is denied. */
    }
}
std::shared_ptr<FileTransaction> FileTransaction::prepare(const fs::path& inputRoot, const FilePlan& plan) {
    auto state = std::make_shared<State>();
    state->root = fs::absolute(inputRoot).lexically_normal();
    if (!fs::is_directory(projectPath(state->root, "")))
        throw std::runtime_error("Не знайдено корінь проєкту");
    state->label = plan.label;
    state->moves = plan.moves;
    state->guards = plan.guards;
    if (plan.edits.empty())
        throw std::runtime_error("Файлова операція порожня");
    if (plan.edits.size() > 50000)
        throw std::runtime_error("Операція перевищує 50000 шляхів");
    std::map<std::string, FileEdit> edits;
    for (const auto& edit : plan.edits) {
        if (edit.path.empty() || edit.path == ".")
            throw std::runtime_error("Не можна замінити корінь проєкту");
        projectPath(state->root, edit.path);
        const auto key = projectPathKey(edit.path);
        if (auto old = edits.find(key); old != edits.end()) {
            // A case-only move has one Win32 identity, not two files.
            const bool caseMove = std::any_of(plan.moves.begin(), plan.moves.end(), [&](const auto& m) {
                return samePath(m.before, m.after) && samePath(m.before, edit.path);
            });
            if (!caseMove || (old->second.kind != FileKind::Missing && edit.kind != FileKind::Missing))
                throw std::runtime_error("Конфлікт шляхів у плані: " + edit.path);
            if (edit.kind != FileKind::Missing)
                old->second = edit;
        } else
            edits.emplace(key, edit);
    }
    for (const auto& move : state->moves) {
        projectPath(state->root, move.before);
        projectPath(state->root, move.after);
    }
    // Add every ancestor as a namespace precondition and create missing parents
    // explicitly. Unknown files added later prevent Undo before anything changes.
    std::vector<std::string> parents;
    for (const auto& [key, edit] : edits)
        for (auto p = parentOf(edit.path);; p = parentOf(p)) {
            parents.push_back(p);
            if (p.empty())
                break;
        }
    for (const auto& p : parents)
        if (!edits.contains(projectPathKey(p)))
            edits.emplace(projectPathKey(p), FileEdit{p, FileKind::Directory, {}, {}});
    if (edits.size() > 50000)
        throw std::runtime_error("Операція з батьківськими папками перевищує 50000 шляхів");
    state->directory = projectPath(state->root, ".proto/transactions/" + Uuid::create().string(), true);
    fs::create_directories(state->directory);
    auto result = std::shared_ptr<FileTransaction>(new FileTransaction(state));
    state->pinJournal();
    size_t index{};
    for (const auto& [key, edit] : edits) {
        State::Entry entry;
        entry.afterPath = edit.path;
        entry.beforePath = remapCase(edit.path, state->moves, true);
        entry.before = projectStamp(state->root, entry.beforePath);
        entry.after.kind = edit.kind;
        if (entry.before.kind == FileKind::File) {
            entry.beforeBlob = "before-" + std::to_string(index);
            copyFlushed(projectPath(state->root, entry.beforePath), state->directory / entry.beforeBlob);
            if (fileStamp(state->directory / entry.beforeBlob) != entry.before)
                throw std::runtime_error("Файл змінився під час підготовки: " + entry.beforePath);
        }
        if (edit.kind == FileKind::File) {
            if (bool(edit.text) == !edit.source.empty())
                throw std::runtime_error("План файла повинен мати рівно одне джерело даних");
            entry.afterBlob = "after-" + std::to_string(index);
            if (edit.text)
                flushBytes(state->directory / entry.afterBlob, *edit.text);
            else {
                const auto source = projectPath(state->root, edit.source);
                const auto expected = projectStamp(state->root, edit.source);
                if (expected.kind != FileKind::File)
                    throw std::runtime_error("Не знайдено файл для копіювання: " + edit.source);
                copyFlushed(source, state->directory / entry.afterBlob);
                if (fileStamp(state->directory / entry.afterBlob) != expected)
                    throw std::runtime_error("Джерело копії змінилося");
                state->guards.push_back({edit.source, expected});
            }
            entry.after = fileStamp(state->directory / entry.afterBlob);
        } else if (edit.text || !edit.source.empty())
            throw std::runtime_error("Папка/видалення не може містити файлові дані");
        state->entries.push_back(std::move(entry));
        ++index;
    }
    for (auto& dir : state->entries)
        if (dir.after.kind == FileKind::Directory) {
            Children final;
            if (dir.before.kind == FileKind::Directory)
                final = children(state->root, dir.beforePath);
            for (const auto& entry : state->entries) {
                if (!entry.beforePath.empty() && samePath(parentOf(entry.beforePath), dir.beforePath)) {
                    const auto key = projectPathKey(nameOf(entry.beforePath));
                    std::erase_if(final, [&](const auto& child) { return projectPathKey(child.first) == key; });
                }
                if (!entry.afterPath.empty() && entry.after.kind != FileKind::Missing &&
                    samePath(parentOf(entry.afterPath), dir.afterPath))
                    final[nameOf(entry.afterPath)] = entry.after.kind;
            }
            dir.after = directoryStamp(final);
        }
    state->check(false);
    for (const auto& guard : state->guards)
        if (projectStamp(state->root, guard.path) != guard.stamp)
            throw std::runtime_error("Файли змінилися під час підготовки: " + guard.path);
    state->save();
    state->durable = true;
    return result;
}

void FileTransaction::State::transition(bool forward, bool recovering, const std::function<void(size_t)>& hook) {
    projectPath(root, ".proto/transactions/" + relativeText(directory.filename()), true);
    const auto desired = [&](const Entry& e) -> const FileStamp& { return forward ? e.after : e.before; };
    const auto targetPath = [&](const Entry& e) -> const std::string& { return forward ? e.afterPath : e.beforePath; };
    // Both sides are needed, including for compensation after a failed write.
    // Validate every payload before any authored path is changed.
    for (const auto& e : entries)
        for (bool side : {false, true}) {
            const auto& stamp = side ? e.after : e.before;
            const auto& name = side ? e.afterBlob : e.beforeBlob;
            if (stamp.kind != FileKind::File)
                continue;
            checkComponent(utf8Path(name).wstring());
            const auto prefix = side ? "after-" : "before-";
            if (!name.starts_with(prefix) || name.size() <= std::char_traits<char>::length(prefix) ||
                name.find_first_not_of("0123456789", std::char_traits<char>::length(prefix)) != std::string::npos)
                throw std::runtime_error("Некоректна назва резервної копії");
            if (fileStamp(directory / utf8Path(name)) != stamp)
                throw std::runtime_error("Пошкоджена резервна копія у журналі");
        }
    if (!recovering) {
        check(!forward);
        for (const auto& guard : guards) {
            // Edited identities already use direction-aware entry stamps.
            // Read-only dependency/topology assumptions must remain true on
            // Undo and later Redo as well as on initial publication.
            const bool edited = std::any_of(entries.begin(), entries.end(),
                                            [&](const Entry& e) { return samePath(e.beforePath, guard.path); });
            if (!edited && projectStamp(root, guard.path) != guard.stamp)
                throw std::runtime_error("Джерело змінено перед операцією: " + guard.path);
        }
    } else {
        // Atomic publication leaves each file entirely before or after. Never
        // interpret an unrelated external version as an interrupted own write.
        for (const auto& e : entries) {
            auto current = projectStamp(root, targetPath(e));
            if ((current.kind != e.before.kind && current.kind != e.after.kind) ||
                (current.kind != FileKind::Directory && current != e.before && current != e.after))
                throw std::runtime_error("Відновлення зупинено: файл змінено зовні: " + targetPath(e));
            if (current.kind == FileKind::Directory) {
                auto simulated = children(root, targetPath(e));
                for (const auto& changed : entries) {
                    for (const auto& path : {changed.beforePath, changed.afterPath})
                        if (!path.empty() && samePath(parentOf(path), targetPath(e))) {
                            const auto key = projectPathKey(nameOf(path));
                            std::erase_if(simulated,
                                          [&](const auto& item) { return projectPathKey(item.first) == key; });
                        }
                    if (!targetPath(changed).empty() && desired(changed).kind != FileKind::Missing &&
                        samePath(parentOf(targetPath(changed)), targetPath(e)))
                        simulated[nameOf(targetPath(changed))] = desired(changed).kind;
                }
                const bool matches = desired(e).kind == FileKind::Directory ? directoryStamp(simulated) == desired(e)
                                                                            : simulated.empty();
                if (!matches)
                    throw std::runtime_error("Відновлення зупинено: нові файли в папці " + targetPath(e));
            }
        }
    }
    const auto rollbackStatus = status;
    status = recovering ? (forward ? "recover-after" : "recover-before") : (forward ? "applying" : "undoing");
    progress = 0;
    save();
    try {
        // Case-only directory renames keep all descendants in place.
        for (const auto& m : moves)
            if (samePath(m.before, m.after) && m.before != m.after) {
                const auto& from = forward ? m.before : m.after;
                const auto& to = forward ? m.after : m.before;
                const auto path = projectPath(root, from);
                if (fs::is_directory(path) &&
                    !MoveFileExW(path.c_str(), projectPath(root, to).c_str(), MOVEFILE_WRITE_THROUGH))
                    ioError("Не вдалося змінити регістр назви папки");
            }
        std::vector<const Entry*> order;
        for (const auto& e : entries)
            order.push_back(&e);
        const auto category = [&](const Entry& e) {
            const auto kind = desired(e).kind;
            return kind == FileKind::Directory                                  ? 0
                   : kind == FileKind::File                                     ? 1
                   : (forward ? e.before : e.after).kind == FileKind::Directory ? 3
                                                                                : 2;
        };
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            const auto ca = category(*a), cb = category(*b);
            if (ca != cb)
                return ca < cb;
            return ca == 3 ? depthOf(targetPath(*a)) > depthOf(targetPath(*b))
                           : depthOf(targetPath(*a)) < depthOf(targetPath(*b));
        });
        for (const auto* e : order) {
            const auto& want = desired(*e);
            const auto path = projectPath(root, targetPath(*e));
            // Staging/publishing earlier files may take time. Detect an edit to
            // a later target again immediately before its own mutation.
            if (want.kind == FileKind::File ||
                (want.kind == FileKind::Missing && (forward ? e->before : e->after).kind != FileKind::Directory)) {
                const auto current = projectStamp(root, targetPath(*e));
                if (recovering ? current != e->before && current != e->after
                               : current != (forward ? e->before : e->after))
                    throw std::runtime_error("Файл змінився під час операції: " + targetPath(*e));
            }
            if (want.kind == FileKind::Directory) {
                if (!fs::exists(path))
                    fs::create_directory(path);
                else if (!fs::is_directory(path))
                    throw std::runtime_error("Папку замінено файлом: " + targetPath(*e));
            } else if (want.kind == FileKind::File) {
                const auto& name = forward ? e->afterBlob : e->beforeBlob;
                checkComponent(utf8Path(name).wstring());
                const auto blob = directory / utf8Path(name);
                if (fileStamp(blob) != want)
                    throw std::runtime_error("Пошкоджена резервна копія у журналі");
                const auto publish = directory / utf8Path("publish-" + Uuid::create().string());
                copyFlushed(blob, publish);
                noReparse(path);
                // Hold the current file against content writers during the last
                // hash check. Win32 replacement needs that reader closed. A new destination never overwrites
                // a file created by another program while staging was copied.
                Handle currentHandle{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                                 nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                if (currentHandle.value == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND &&
                    GetLastError() != ERROR_PATH_NOT_FOUND)
                    ioError("Файл зайнято іншою програмою");
                const auto current = projectStamp(root, targetPath(*e));
                if (recovering ? current != e->before && current != e->after
                               : current != (forward ? e->before : e->after))
                    throw std::runtime_error("Файл змінився під час публікації: " + targetPath(*e));
                const DWORD replace = current.kind == FileKind::Missing ? 0 : MOVEFILE_REPLACE_EXISTING;
                if (currentHandle.value != INVALID_HANDLE_VALUE) {
                    CloseHandle(currentHandle.value);
                    currentHandle.value = INVALID_HANDLE_VALUE;
                }
                if (!MoveFileExW(publish.c_str(), path.c_str(), replace | MOVEFILE_WRITE_THROUGH))
                    ioError("Не вдалося опублікувати файл: " + targetPath(*e));
            } else {
                if (targetPath(*e).empty())
                    throw std::runtime_error("Журнал не може видаляти корінь проєкту");
                if (fs::exists(path) && !fs::remove(path))
                    throw std::runtime_error("Не вдалося видалити шлях: " + targetPath(*e));
            }
            ++progress;
            if (hook)
                hook(progress);
            save();
        }
        check(forward);
        status = forward ? "committed" : "undone";
        save();
    } catch (...) {
        const auto failure = std::current_exception();
        if (!recovering) {
            try {
                transition(!forward, true);
                status = rollbackStatus;
                save();
            } catch (const std::exception& recovery) {
                throw std::runtime_error(std::string("Потрібне відновлення журналу: ") + recovery.what());
            }
        }
        std::rethrow_exception(failure);
    }
}
void FileTransaction::redo() {
    if (state_->status != "ready" && state_->status != "undone")
        throw std::runtime_error("Некоректний стан Redo");
    state_->transition(true, false, afterStep);
}
void FileTransaction::undo() {
    if (state_->status != "committed")
        throw std::runtime_error("Некоректний стан Undo");
    state_->transition(false, false, afterStep);
}
size_t FileTransaction::memoryBytes() const {
    size_t bytes = sizeof(State) + sizeof(*this) + state_->label.size() +
                   state_->pins.size() * (sizeof(Handle) + sizeof(std::shared_ptr<Handle>));
    for (const auto& e : state_->entries)
        bytes += sizeof(e) + e.beforePath.size() + e.afterPath.size() + e.beforeBlob.size() + e.afterBlob.size() +
                 e.before.hash.size() + e.after.hash.size();
    for (const auto& g : state_->guards)
        bytes += sizeof(g) + g.path.size() + g.stamp.hash.size();
    for (const auto& m : state_->moves)
        bytes += sizeof(m) + m.before.size() + m.after.size();
    return bytes;
}
uint64_t FileTransaction::diskBytes() const {
    uint64_t bytes{};
    for (const auto& e : state_->entries) {
        if (e.before.kind == FileKind::File)
            bytes += e.before.size;
        if (e.after.kind == FileKind::File)
            bytes += e.after.size;
    }
    return bytes;
}
const std::vector<PathMove>& FileTransaction::moves() const {
    return state_->moves;
}
const std::string& FileTransaction::label() const {
    return state_->label;
}
const fs::path& FileTransaction::journalPath() const {
    return state_->directory;
}
void FileTransaction::recover(const fs::path& inputRoot, const std::function<void(size_t)>& hook) {
    const auto root = fs::absolute(inputRoot).lexically_normal(),
               transactions = projectPath(root, ".proto/transactions", true);
    if (!fs::exists(transactions))
        return;
    for (const auto& folder : fs::directory_iterator(transactions)) {
        noReparse(folder.path());
        if (!folder.is_directory())
            continue;
        Uuid::parse(relativeText(folder.path().filename()));
        const auto journal = folder.path() / "journal.json";
        if (!fs::exists(journal)) {
            removeJournal(root, folder.path());
            continue;
        }
        noReparse(journal);
        JsonDoc doc(readDocument(journal));
        auto* j = doc.root();
        if (str(get(j, "format")) != "proto.transaction" || num(get(j, "formatVersion")) != 1)
            throw std::runtime_error("Невідома версія журналу");
        auto state = std::make_shared<State>();
        state->root = root;
        state->directory = folder.path();
        state->label = str(get(j, "label"));
        state->status = str(get(j, "state"));
        if (state->status == "committed" || state->status == "undone" || state->status == "ready") {
            removeJournal(root, folder.path());
            continue;
        }
        if (state->status != "applying" && state->status != "undoing" && state->status != "recover-before" &&
            state->status != "recover-after")
            throw std::runtime_error("Невідомий стан відновлення");
        state->pinJournal();
        const bool forward = state->status == "undoing" || state->status == "recover-after";
        auto* list = get(j, "entries");
        if (!yyjson_is_arr(list) || yyjson_arr_size(list) > 50000)
            throw std::runtime_error("Некоректний журнал");
        size_t i, n;
        Json* item;
        std::set<std::string> identities;
        yyjson_arr_foreach(list, i, n, item) {
            State::Entry e{str(get(item, "beforePath")),    str(get(item, "afterPath")),
                           parseStamp(get(item, "before")), parseStamp(get(item, "after")),
                           str(get(item, "beforeBlob")),    str(get(item, "afterBlob"))};
            projectPath(root, e.beforePath);
            projectPath(root, e.afterPath);
            if (!samePath(e.beforePath, e.afterPath) || !identities.insert(projectPathKey(e.beforePath)).second)
                throw std::runtime_error("Некоректні шляхи журналу");
            if (e.beforePath.empty() && (e.before.kind != FileKind::Directory || e.after.kind != FileKind::Directory))
                throw std::runtime_error("Некоректний корінь журналу");
            state->entries.push_back(std::move(e));
        }
        list = get(j, "moves");
        if (!yyjson_is_arr(list))
            throw std::runtime_error("Некоректний список переміщень");
        yyjson_arr_foreach(list, i, n, item) {
            PathMove m{str(get(item, "before")), str(get(item, "after"))};
            projectPath(root, m.before);
            projectPath(root, m.after);
            state->moves.push_back(std::move(m));
        }
        state->transition(forward, true, hook);
        state->pins.clear();
        removeJournal(root, folder.path());
    }
}
} // namespace proto
