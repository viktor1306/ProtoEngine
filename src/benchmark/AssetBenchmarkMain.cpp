#include "AssetFixtures.hpp"

#include "assets/AssetIO.hpp"
#include "assets/AssetWorkspace.hpp"
#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include "editor/SceneDocument.hpp"
#include "project/AssetFilePlanner.hpp"
#include "project/BuildFileTree.hpp"
#include "project/FileTransactions.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace proto::asset_benchmark {
namespace {
namespace fs = std::filesystem;

struct ProcessMemoryCounters {
    DWORD cb{};
    DWORD pageFaultCount{};
    SIZE_T peakWorkingSetSize{};
    SIZE_T workingSetSize{};
    SIZE_T quotaPeakPagedPoolUsage{};
    SIZE_T quotaPagedPoolUsage{};
    SIZE_T quotaPeakNonPagedPoolUsage{};
    SIZE_T quotaNonPagedPoolUsage{};
    SIZE_T pagefileUsage{};
    SIZE_T peakPagefileUsage{};
    SIZE_T privateUsage{};
};
using QueryMemoryFn = BOOL(WINAPI*)(HANDLE, ProcessMemoryCounters*, DWORD);

template <class Function> Function functionAddress(FARPROC address) {
    static_assert(sizeof(Function) == sizeof(address));
    Function result{};
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

struct ProcessSample {
    bool valid{};
    double cpuMs{};
    uint64_t workingSetBytes{};
    uint64_t privateBytes{};
};

uint64_t fileTimeTicks(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

double processCpuMilliseconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(fileTimeTicks(kernel) + fileTimeTicks(user)) / 10000.0;
}

ProcessSample processSample(double cpuBaseline = std::numeric_limits<double>::quiet_NaN()) {
    ProcessSample result;
    const auto cpu = processCpuMilliseconds();
    if (std::isfinite(cpu) && std::isfinite(cpuBaseline)) {
        result.valid = true;
        result.cpuMs = std::max(0.0, cpu - cpuBaseline);
    }
    const auto module = GetModuleHandleW(L"kernel32.dll");
    const auto query = module ? functionAddress<QueryMemoryFn>(GetProcAddress(module, "K32GetProcessMemoryInfo"))
                              : nullptr;
    if (!query)
        return result;
    ProcessMemoryCounters counters{};
    counters.cb = sizeof(counters);
    if (!query(GetCurrentProcess(), &counters, sizeof(counters)))
        return result;
    result.valid = true;
    result.workingSetBytes = static_cast<uint64_t>(counters.workingSetSize);
    result.privateBytes = static_cast<uint64_t>(counters.privateUsage);
    return result;
}

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct StageResult {
    std::string name;
    double milliseconds{};
    ProcessSample start, end, peak;
    std::string sourceHash;
    bool fromCache{};
};

void accumulatePeak(ProcessSample& peak, const ProcessSample& sample) {
    peak.valid |= sample.valid;
    peak.cpuMs = std::max(peak.cpuMs, sample.cpuMs);
    peak.workingSetBytes = std::max(peak.workingSetBytes, sample.workingSetBytes);
    peak.privateBytes = std::max(peak.privateBytes, sample.privateBytes);
}

template <class Function> auto measured(StageResult& result, Function&& function) {
    result.start = processSample();
    const auto baseline = processCpuMilliseconds();
    const auto started = Clock::now();
    auto value = function();
    result.milliseconds = milliseconds(started);
    result.end = processSample(baseline);
    result.peak = result.end;
    accumulatePeak(result.peak, result.start);
    return value;
}

template <class Function> void measuredVoid(StageResult& result, Function&& function) {
    result.start = processSample();
    const auto baseline = processCpuMilliseconds();
    const auto started = Clock::now();
    function();
    result.milliseconds = milliseconds(started);
    result.end = processSample(baseline);
    result.peak = result.end;
    accumulatePeak(result.peak, result.start);
}

std::string relativePath(const fs::path& root, const fs::path& path) {
    return utf8(fs::absolute(path).lexically_normal().lexically_relative(fs::absolute(root).lexically_normal())
                    .generic_wstring());
}

void ensureOwnedPath(const fs::path& root, const fs::path& path) {
    const auto base = fs::weakly_canonical(root);
    const auto candidate = fs::weakly_canonical(path);
    auto it = base.begin();
    auto candidateIt = candidate.begin();
    for (; it != base.end(); ++it, ++candidateIt)
        check(candidateIt != candidate.end() && *candidateIt == *it, "Benchmark path escaped owned root");
}

fs::path reportJsonPath(const fs::path& output) {
    if (output.empty())
        throw std::invalid_argument("Asset benchmark output path is empty");
    auto path = fs::absolute(output);
    if (path.extension() == L".json")
        return path;
    if (path.extension() == L".csv") {
        path.replace_extension(L".json");
        return path;
    }
    return path.wstring() + L".json";
}

struct CycleResult {
    uint32_t cycle{};
    double moveRedoMs{}, moveUndoMs{}, moveRedoAgainMs{}, moveFinalUndoMs{};
    double copyRedoMs{}, copyUndoMs{}, copyRedoAgainMs{}, copyFinalUndoMs{};
    ProcessSample end, peak;
    std::string sourceHash;
    std::string sourceId;
    bool restored{};
};

struct RunResult {
    fs::path report;
    fs::path fixtureRoot;
    uint32_t cycles{};
    std::string sourceHash;
    std::string fixtureManifestHash;
    std::string importedId;
    std::string importedRevision;
    std::vector<StageResult> stages;
    std::vector<CycleResult> cycleResults;
    bool coldFromCache{};
    bool warmFromCache{};
    bool modelExpired{};
    bool meshExpired{};
    bool texturePixelsExpired{};
    bool operationHashesRestored{};
    bool operationIdsRestored{};
    bool cleanupAttempted{};
    bool cleanupSucceeded{};
    std::string cleanupError;
};

struct FixtureCleanupGuard {
    fs::path root;
    ~FixtureCleanupGuard() {
        if (root.empty())
            return;
        std::error_code error;
        (void)build_detail::removeTree(root, error);
    }
};

void writeJson(const RunResult& result) {
    ensureParent(result.report);
    std::ofstream out(result.report, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot open asset benchmark JSON output");
    out << "{\"format\":\"proto.asset-benchmark\",\"version\":1,\"cycles\":" << result.cycles
        << ",\"source_hash\":" << jsonString(result.sourceHash)
        << ",\"fixture_manifest_hash\":" << jsonString(result.fixtureManifestHash)
        << ",\"imported_id\":" << jsonString(result.importedId)
        << ",\"imported_revision\":" << jsonString(result.importedRevision)
        << ",\"cold_from_cache\":" << (result.coldFromCache ? "true" : "false")
        << ",\"warm_from_cache\":" << (result.warmFromCache ? "true" : "false")
        << ",\"cold_definition\":\"own cooked cache removed; Windows OS disk cache was not purged\""
        << ",\"os_disk_cache_purged\":false,\"weak_owner_checks\":{\"model_expired\":"
        << (result.modelExpired ? "true" : "false") << ",\"mesh_expired\":"
        << (result.meshExpired ? "true" : "false") << ",\"texture_pixels_expired\":"
        << (result.texturePixelsExpired ? "true" : "false") << "}"
        << ",\"file_operations\":{\"hashes_restored\":"
        << (result.operationHashesRestored ? "true" : "false") << ",\"ids_restored\":"
        << (result.operationIdsRestored ? "true" : "false") << "}"
        << ",\"stages\":[";
    for (size_t i = 0; i < result.stages.size(); ++i) {
        if (i)
            out << ',';
        const auto& stage = result.stages[i];
        out << "{\"name\":" << jsonString(stage.name) << ",\"duration_ms\":" << stage.milliseconds
            << ",\"from_cache\":" << (stage.fromCache ? "true" : "false")
            << ",\"source_hash\":" << jsonString(stage.sourceHash)
            << ",\"process\":{\"valid\":" << (stage.end.valid ? "true" : "false")
            << ",\"cpu_ms\":" << stage.end.cpuMs << ",\"working_set_bytes\":"
            << stage.end.workingSetBytes << ",\"private_bytes\":" << stage.end.privateBytes
            << ",\"peak_working_set_bytes\":" << stage.peak.workingSetBytes
            << ",\"peak_private_bytes\":" << stage.peak.privateBytes << "}}";
    }
    out << "],\"cycles_detail\":[";
    for (size_t i = 0; i < result.cycleResults.size(); ++i) {
        if (i)
            out << ',';
        const auto& cycle = result.cycleResults[i];
        out << "{\"cycle\":" << cycle.cycle << ",\"source_hash\":" << jsonString(cycle.sourceHash)
            << ",\"source_id\":" << jsonString(cycle.sourceId)
            << ",\"move_redo_ms\":" << cycle.moveRedoMs << ",\"move_undo_ms\":" << cycle.moveUndoMs
            << ",\"move_redo_again_ms\":" << cycle.moveRedoAgainMs
            << ",\"move_final_undo_ms\":" << cycle.moveFinalUndoMs
            << ",\"copy_redo_ms\":" << cycle.copyRedoMs << ",\"copy_undo_ms\":" << cycle.copyUndoMs
            << ",\"copy_redo_again_ms\":" << cycle.copyRedoAgainMs
            << ",\"copy_final_undo_ms\":" << cycle.copyFinalUndoMs
            << ",\"restored\":" << (cycle.restored ? "true" : "false")
            << ",\"process\":{\"valid\":" << (cycle.end.valid ? "true" : "false")
            << ",\"working_set_bytes\":" << cycle.end.workingSetBytes << ",\"private_bytes\":"
            << cycle.end.privateBytes << ",\"peak_working_set_bytes\":" << cycle.peak.workingSetBytes
            << ",\"peak_private_bytes\":" << cycle.peak.privateBytes << "}}";
    }
    out << "],\"cleanup\":{\"attempted\":" << (result.cleanupAttempted ? "true" : "false")
        << ",\"succeeded\":" << (result.cleanupSucceeded ? "true" : "false")
        << ",\"error\":" << jsonString(result.cleanupError) << "}}\n";
}

void writeCsv(const RunResult& result) {
    auto path = result.report;
    path.replace_extension(L".csv");
    ensureParent(path);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot open asset benchmark CSV output");
    out << "kind,index,name,duration_ms,from_cache,source_hash,source_id,working_set_bytes,private_bytes,"
           "peak_working_set_bytes,peak_private_bytes,restored\n";
    for (const auto& stage : result.stages)
        out << "stage,0," << stage.name << ',' << stage.milliseconds << ',' << (stage.fromCache ? 1 : 0) << ','
            << stage.sourceHash << ",," << stage.end.workingSetBytes << ',' << stage.end.privateBytes << ','
            << stage.peak.workingSetBytes << ',' << stage.peak.privateBytes << ",\n";
    for (const auto& cycle : result.cycleResults) {
        out << "cycle," << cycle.cycle << ",move_redo," << cycle.moveRedoMs << ",,," << cycle.sourceId << ','
            << cycle.end.workingSetBytes << ',' << cycle.end.privateBytes << ',' << cycle.peak.workingSetBytes << ','
            << cycle.peak.privateBytes << ',' << (cycle.restored ? 1 : 0) << "\n";
        out << "cycle," << cycle.cycle << ",copy_redo," << cycle.copyRedoMs << ",,," << cycle.sourceId << ','
            << cycle.end.workingSetBytes << ',' << cycle.end.privateBytes << ',' << cycle.peak.workingSetBytes << ','
            << cycle.peak.privateBytes << ',' << (cycle.restored ? 1 : 0) << "\n";
    }
}

struct Options {
    fs::path output;
    uint32_t cycles{12};
};

Options parse(int argc, wchar_t** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--output") {
            if (++i >= argc)
                throw std::runtime_error("--output requires PATH");
            result.output = argv[i];
        } else if (arg == L"--cycles") {
            if (++i >= argc)
                throw std::runtime_error("--cycles requires N");
            size_t parsed{};
            const auto value = std::stoull(argv[i], &parsed);
            if (parsed != std::wstring(argv[i]).size() || value < 1 || value > 10000)
                throw std::runtime_error("--cycles must be between 1 and 10000");
            result.cycles = static_cast<uint32_t>(value);
        } else if (arg == L"--help") {
            std::wcout << L"ProtoAssetBenchmark --output PATH [--cycles N]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + utf8(arg));
        }
    }
    if (result.output.empty())
        throw std::runtime_error("--output PATH is required");
    return result;
}

RunResult run(const Options& options) {
    RunResult result;
    result.cycles = options.cycles;
    result.report = reportJsonPath(options.output);
    const auto reportDirectory = result.report.parent_path();
    fs::create_directories(reportDirectory);
    // Keep fixture names short independently of the report basename. File
    // transaction backups add their own UUID directory below this root.
    result.fixtureRoot = reportDirectory / ("m7a-" + Uuid::create().string());
    ensureOwnedPath(reportDirectory, result.fixtureRoot);
    fs::create_directories(result.fixtureRoot);
    const auto sourceFixtures = result.fixtureRoot / "src";
    const auto projectRoot = result.fixtureRoot / "load";
    const auto operationRoot = result.fixtureRoot / "files";
    ensureOwnedPath(result.fixtureRoot, sourceFixtures);
    ensureOwnedPath(result.fixtureRoot, projectRoot);
    ensureOwnedPath(result.fixtureRoot, operationRoot);
    FixtureCleanupGuard cleanupGuard{result.fixtureRoot};

    fixtures::create(sourceFixtures);
    const auto source = sourceFixtures / "probes.gltf";
    result.sourceHash = sha256(assetBytes(source));
    result.fixtureManifestHash = sha256(assetBytes(sourceFixtures / "MANIFEST.md"));

    auto workspace = std::make_shared<AssetWorkspace>(projectRoot);
    std::shared_ptr<const ModelBundle> imported;
    std::weak_ptr<const ModelBundle> modelWeak;
    std::weak_ptr<const MeshAsset> meshWeak;
    std::weak_ptr<const TexturePixels> pixelsWeak;
    {
        StageResult stage{"initial_import"};
        imported = measured(stage, [&] { return workspace->importFile(source); });
        stage.sourceHash = result.sourceHash;
        stage.fromCache = imported->fromCache;
        result.stages.push_back(std::move(stage));
    }
    check(imported && !imported->fromCache, "Initial asset import unexpectedly came from cache");
    result.importedId = imported->id.string();
    result.importedRevision = imported->revision;
    const auto importedId = imported->id;
    const auto cachePath = projectRoot / ".proto/cache" / (sha256(imported->revision + imported->id.string()) + ".bin");
    ensureOwnedPath(projectRoot / ".proto/cache", cachePath);
    check(fs::exists(cachePath), "Cooked cache was not produced by initial import");

    std::shared_ptr<const ModelBundle> cold;
    {
        StageResult stage{"cooked_cold_load"};
        std::error_code error;
        fs::remove(cachePath, error);
        check(!error && !fs::exists(cachePath), "Could not remove the owned cooked cache");
        cold = measured(stage, [&] { return workspace->load(importedId); });
        stage.sourceHash = result.sourceHash;
        stage.fromCache = cold && cold->fromCache;
        result.stages.push_back(std::move(stage));
    }
    check(cold && !cold->fromCache, "Cooked cold load used a cooked cache");
    result.coldFromCache = cold->fromCache;

    std::shared_ptr<const ModelBundle> warm;
    {
        StageResult stage{"cooked_warm_load"};
        warm = measured(stage, [&] { return workspace->load(importedId); });
        stage.sourceHash = result.sourceHash;
        stage.fromCache = warm && warm->fromCache;
        result.stages.push_back(std::move(stage));
    }
    check(warm && warm->fromCache && warm->id == importedId, "Cooked warm load did not reuse the own cache");
    result.warmFromCache = warm->fromCache;
    check(!warm->meshes.empty() && !warm->textures.empty() && warm->meshes.front()->id == imported->meshes.front()->id,
          "Warm load changed stable resource IDs");

    modelWeak = warm;
    meshWeak = warm->meshes.front();
    pixelsWeak = warm->textures.front()->pixels;
    {
        StageResult stage{"material_document_asset_load_unload"};
        measuredVoid(stage, [&] {
            const auto scenePath = projectRoot / "Scenes" / "AssetBenchmark.scene.json";
            SceneDocument document;
            document.saveProject(scenePath, projectRoot);
            document.instantiate(warm);
            check(!document.scene.modelSources.empty() && !document.scene.assets->meshes.empty(),
                  "Document did not publish imported assets");
            if (!warm->materials.empty()) {
                auto values = warm->materials.front()->values;
                values.roughness = .31f;
                document.material(warm->materials.front()->id, values, false);
            }
            document.saveProject(scenePath, projectRoot);
            SceneDocument reopened;
            reopened.load(scenePath);
            check(!reopened.scene.modelSources.empty() && !reopened.scene.assets->models.empty(),
                  "Document reload did not load project assets");
            document.undo();
            document.redo();
        });
        stage.sourceHash = result.sourceHash;
        stage.fromCache = true;
        result.stages.push_back(std::move(stage));
    }
    imported.reset();
    cold.reset();
    warm.reset();
    result.modelExpired = modelWeak.expired();
    result.meshExpired = meshWeak.expired();
    result.texturePixelsExpired = pixelsWeak.expired();
    check(result.modelExpired && result.meshExpired && result.texturePixelsExpired,
          "Asset owners kept model/mesh/texture pixels alive after unload");

    auto operationWorkspace = std::make_shared<AssetWorkspace>(operationRoot);
    const auto operationModel = operationWorkspace->importFile(source);
    const auto operationId = operationModel->id;
    const auto operationSource = operationModel->source;
    const auto operationPath = resourcePath(operationRoot, operationSource);
    const auto operationHash = sha256(assetBytes(operationPath));
    result.operationHashesRestored = true;
    result.operationIdsRestored = true;
    result.cycleResults.reserve(options.cycles);
    for (uint32_t cycleIndex = 1; cycleIndex <= options.cycles; ++cycleIndex) {
        CycleResult cycle;
        cycle.cycle = cycleIndex;
        cycle.sourceHash = operationHash;
        cycle.sourceId = operationId.string();
        const auto moveDestination = "Assets/BenchmarkMoved/" + std::to_string(cycleIndex);
        const auto copyDestination = "Assets/BenchmarkCopied/" + std::to_string(cycleIndex);
        const auto movedSource = moveDestination + "/" + utf8(fs::path(operationSource).filename().wstring());
        const auto copiedSource = copyDestination + "/" + utf8(fs::path(operationSource).filename().wstring());
        auto moveTransaction = FileTransaction::prepare(
            operationRoot, planProjectFiles(operationRoot, FileOperation::Move, {operationSource}, moveDestination));
        StageResult stage{"file_move_redo_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { moveTransaction->redo(); });
        cycle.moveRedoMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(fs::exists(resourcePath(operationRoot, movedSource)), "File rename redo did not publish its target");
        result.operationIdsRestored &= !operationWorkspace->rebuildRegistry().empty();

        stage = StageResult{"file_move_undo_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { moveTransaction->undo(); });
        cycle.moveUndoMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(fs::exists(operationPath), "File rename undo did not restore the source");

        stage = StageResult{"file_move_redo_again_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { moveTransaction->redo(); });
        cycle.moveRedoAgainMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(fs::exists(resourcePath(operationRoot, movedSource)), "File rename redo did not repeat deterministically");

        stage = StageResult{"file_move_final_undo_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { moveTransaction->undo(); });
        cycle.moveFinalUndoMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(fs::exists(operationPath), "File rename final undo did not restore the source");

        auto copyTransaction = FileTransaction::prepare(
            operationRoot, planProjectFiles(operationRoot, FileOperation::Copy, {operationSource}, copyDestination));
        stage = StageResult{"file_copy_redo_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { copyTransaction->redo(); });
        cycle.copyRedoMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(fs::exists(resourcePath(operationRoot, copiedSource)), "File copy redo did not publish its target");
        stage = StageResult{"file_copy_undo_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { copyTransaction->undo(); });
        cycle.copyUndoMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(!fs::exists(resourcePath(operationRoot, copiedSource)), "File copy undo did not remove its target");
        stage = StageResult{"file_copy_redo_again_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { copyTransaction->redo(); });
        cycle.copyRedoAgainMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(fs::exists(resourcePath(operationRoot, copiedSource)), "File copy redo did not repeat deterministically");
        stage = StageResult{"file_copy_final_undo_" + std::to_string(cycleIndex)};
        measuredVoid(stage, [&] { copyTransaction->undo(); });
        cycle.copyFinalUndoMs = stage.milliseconds;
        result.stages.push_back(std::move(stage));
        check(!fs::exists(resourcePath(operationRoot, copiedSource)), "File copy final undo did not restore the tree");

        const auto finalHash = sha256(assetBytes(operationPath));
        const auto registry = operationWorkspace->rebuildRegistry();
        cycle.restored = finalHash == operationHash && registry.size() == 1 && registry.front().first == operationId;
        result.operationHashesRestored &= finalHash == operationHash;
        result.operationIdsRestored &= cycle.restored;
        cycle.end = processSample();
        cycle.peak = cycle.end;
        for (const auto& stageResult : result.stages)
            if (stageResult.name.ends_with("_" + std::to_string(cycleIndex)))
                accumulatePeak(cycle.peak, stageResult.peak);
        result.cycleResults.push_back(std::move(cycle));
    }
    cleanupGuard.root.clear();
    return result;
}

} // namespace
} // namespace proto::asset_benchmark

int wmain(int argc, wchar_t** argv) {
    using namespace proto::asset_benchmark;
    try {
        const auto options = parse(argc, argv);
        auto result = run(options);
        result.cleanupAttempted = true;
        std::error_code error;
        const auto removed = proto::build_detail::removeTree(result.fixtureRoot, error);
        result.cleanupSucceeded = !error && removed != static_cast<uintmax_t>(-1);
        if (error)
            result.cleanupError = error.message();
        writeJson(result);
        writeCsv(result);
        if (!result.cleanupSucceeded)
            throw std::runtime_error("Owned benchmark fixture cleanup failed: " + result.cleanupError);
        std::cout << "Asset benchmark completed: " << proto::utf8(result.report.wstring()) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Asset benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
