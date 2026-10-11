// Reads and writes files inside an emulator DSi NAND image (with its no$gba
// footer) through melonDS's DSi_NAND, for unlaunch-nand.sh. Only ever use it on a copy.
//   nandtool biosdsi7.bin nand.bin titles CATEGORY       list title IDs (hex), e.g. 00030017
//   nandtool biosdsi7.bin nand.bin export NANDPATH OUT   e.g. 0:/title/00030017/484e4145/content/title.tmd
//   nandtool biosdsi7.bin nand.bin import NANDPATH IN
// Links against the core of the melonds-headless build (see unlaunch-nand.sh).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "DSi_NAND.h"
#include "Platform.h"

using namespace melonDS;
// platform.cpp of the headless runner calls these
void hl_log(Platform::LogLevel, const char* msg) { std::fputs(msg, stderr); }
void hl_signal_stop() {}
int hl_net_send(u8*, int len) { return len; }
int hl_net_recv(u8*) { return 0; }

int main(int argc, char** argv)
{
    if (argc < 5) { std::fprintf(stderr, "usage: nandtool biosdsi7.bin nand.bin export|import|titles ...\n"); return 2; }
    std::vector<u8> bios(0x10000);
    FILE* bf = std::fopen(argv[1], "rb");
    if (!bf || std::fread(bios.data(), 1, bios.size(), bf) != bios.size()) { std::fprintf(stderr, "bad bios\n"); return 2; }
    std::fclose(bf);
    Platform::FileHandle* nf = Platform::OpenFile(argv[2], Platform::ReadWriteExisting);
    if (!nf) { std::fprintf(stderr, "cannot open nand\n"); return 2; }
    DSi_NAND::NANDImage nand(nf, &bios[0x8308]);
    if (!nand) { std::fprintf(stderr, "NAND did not parse\n"); return 2; }
    DSi_NAND::NANDMount mount(nand);
    if (!mount) { std::fprintf(stderr, "NAND did not mount\n"); return 2; }
    std::string cmd = argv[3];
    if (cmd == "export" && argc == 6) return mount.ExportFile(argv[4], argv[5]) ? 0 : 1;
    if (cmd == "import" && argc == 6) return mount.ImportFile(argv[4], argv[5]) ? 0 : 1;
    if (cmd == "titles")
    {
        std::vector<u32> ids;
        mount.ListTitles((u32)std::strtoul(argv[4], nullptr, 16), ids);
        for (u32 id : ids) std::printf("%08x\n", id);
        return 0;
    }
    std::fprintf(stderr, "bad command\n");
    return 2;
}
