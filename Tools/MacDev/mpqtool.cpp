// Development tool: lists and extracts files from the client's MPQ archives, with the client's patch
// order. Usage:
//   mpqtool <Data dir> list <mask>
//   mpqtool <Data dir> extract <file> <output>
//   mpqtool <Data dir> extractall <mask> <output dir>   (paths kept, lowercased)

#include <StormLib.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

static std::vector<std::string> Archives(const std::string& data) {
    // Base archives, then patches in name order (patch.mpq, patch-2.mpq ... patch-z.mpq), then the
    // locale's, as the client stacks them; later archives win.
    std::vector<std::string> base = { "common.mpq", "common-2.mpq", "expansion.mpq", "lichking.mpq" };
    std::vector<std::string> patches;
    std::vector<std::string> result;

    DIR* dir = opendir(data.c_str());

    if (!dir) {
        return result;
    }

    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".mpq") == 0 && lower.compare(0, 5, "patch") == 0) {
            patches.push_back(name);
        }
    }

    closedir(dir);
    std::sort(patches.begin(), patches.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });

    for (const std::string& name : base) {
        result.push_back(data + "/" + name);
    }

    for (const std::string& name : patches) {
        result.push_back(data + "/" + name);
    }

    return result;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <Data dir> list <mask> | extract <file> <output>\n", argv[0]);
        return 1;
    }

    std::vector<std::string> archives = Archives(argv[1]);
    HANDLE mpq = nullptr;

    for (const std::string& path : archives) {
        if (!mpq) {
            if (!SFileOpenArchive(path.c_str(), 0, STREAM_FLAG_READ_ONLY, &mpq)) {
                mpq = nullptr;
            }
        } else {
            SFileOpenPatchArchive(mpq, path.c_str(), nullptr, 0);
        }
    }

    if (!mpq) {
        fprintf(stderr, "no archives\n");
        return 1;
    }

    if (!strcmp(argv[2], "list")) {
        SFILE_FIND_DATA found;
        HANDLE find = SFileFindFirstFile(mpq, argv[3], &found, nullptr);

        if (find) {
            do {
                printf("%10u %s\n", found.dwFileSize, found.cFileName);
            } while (SFileFindNextFile(find, &found));

            SFileFindClose(find);
        }
    } else if (!strcmp(argv[2], "extractall") && argc >= 5) {
        SFILE_FIND_DATA found;
        HANDLE find = SFileFindFirstFile(mpq, argv[3], &found, nullptr);
        int count = 0;

        if (find) {
            do {
                std::string relative = found.cFileName;

                for (char& c : relative) {
                    c = c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
                }

                std::string output = std::string(argv[4]) + "/" + relative;

                for (size_t slash = output.find('/', strlen(argv[4]) + 1); slash != std::string::npos; slash = output.find('/', slash + 1)) {
                    std::string directory = output.substr(0, slash);
                    mkdir(directory.c_str(), 0755);
                }

                if (SFileExtractFile(mpq, found.cFileName, output.c_str(), 0)) {
                    count++;
                }
            } while (SFileFindNextFile(find, &found));

            SFileFindClose(find);
        }

        printf("%d files\n", count);
    } else if (!strcmp(argv[2], "extract") && argc >= 5) {
        if (!SFileExtractFile(mpq, argv[3], argv[4], 0)) {
            fprintf(stderr, "extract failed: %s\n", argv[3]);
            return 1;
        }
    }

    SFileCloseArchive(mpq);

    return 0;
}
