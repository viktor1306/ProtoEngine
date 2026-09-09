#include <Proto/Player.hpp>
#include <windows.h>
#include <shellapi.h>
int wmain(int argc, wchar_t** argv) {
    return proto::sdk::RunPlayer(argc, argv, &RegisterProjectBehaviors);
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 2;
    const auto result = wmain(argc, argv);
    LocalFree(argv);
    return result;
}
