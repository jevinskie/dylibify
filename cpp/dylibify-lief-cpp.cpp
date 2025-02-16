#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <LIEF/MachO.hpp>
#include <LIEF/logging.hpp>
#include <argparse/argparse.hpp>
#include <fmt/format.h>
#include <subprocess.hpp>

namespace fs = std::filesystem;
using namespace std::string_literals;
using namespace LIEF::MachO;

static uint8_t get_library_ordinal(uint16_t n_desc) {
    return n_desc >> 8;
}

static void set_library_ordinal(uint16_t &n_desc, uint8_t ordinal) {
    n_desc = (n_desc & 0x00FF) | (ordinal << 8);
}

static bool dylib_exists(const std::string &dylib_path) {
    if (auto *handle = dlopen(dylib_path.c_str(), RTLD_LAZY | RTLD_LOCAL)) {
        dlclose(handle);
        return true;
    } else {
        return false;
    }
}

static void write_string_to_file(const std::string &str, const fs::path file) {
    auto *fh = fopen(file.c_str(), "w");
    assert(fh);
    assert(fwrite(str.c_str(), str.size(), 1, fh) == 1);
    assert(!fclose(fh));
}

static std::string create_stub_objc(const std::set<std::string> &stub_syms) {
    std::string objc = R"objc(
#undef NDEBUG
#include <assert.h>
#import <Foundation/Foundation.h>
)objc";

    const auto objc_class_prefix = "_OBJC_CLASS_$_"s;
    const auto plain_prefix      = "_"s;

    for (const auto &sym : stub_syms) {
        if (sym.starts_with(objc_class_prefix)) {
            const auto objc_class_name = sym.substr(objc_class_prefix.size());
            objc += fmt::format(R"objc(
@interface {:s} : NSObject
@end
@implementation {:s}
@end
)objc",
                                objc_class_name, objc_class_name);
        } else if (sym.starts_with(plain_prefix)) {
            const auto sym_name = sym.substr(plain_prefix.size());
            objc += fmt::format(R"objc(
void {:s}(void) {{
    assert(!"unimplemented symbols '{:s}'");
}}
)objc",
                                sym_name, sym_name);
        } else {
            assert(!"this shouldn't happen");
        }
    }

    return objc;
}

static const std::map<CPU_TYPES, std::string> arch_map{
    {CPU_TYPES::CPU_TYPE_X86, "i386"},
    {CPU_TYPES::CPU_TYPE_X86_64, "x86_64"},
    {CPU_TYPES::CPU_TYPE_ARM, "armv7"},
    {CPU_TYPES::CPU_TYPE_ARM64, "arm64"},
};

static std::optional<fs::path> create_thin_stub_dylib(const fs::path &fat_stub_filename,
                                                      const fs::path &out_path,
                                                      const fs::path &stub_dylib_path,
                                                      const std::set<std::string> &stub_syms,
                                                      const CPU_TYPES cpu_type) {
    const auto objc = create_stub_objc(stub_syms);
    const auto arch = arch_map.at(cpu_type);

    const auto out_dir           = out_path.parent_path();
    auto thin_sub_dylib_filename = fat_stub_filename.stem();
    thin_sub_dylib_filename += "." + arch;
    auto thin_stub_src_filename{thin_sub_dylib_filename};
    thin_stub_src_filename += ".m";
    thin_sub_dylib_filename += fat_stub_filename.extension();
    const auto thin_stub_dylib_path = out_dir / thin_sub_dylib_filename;
    const auto thin_stub_src_path   = out_dir / thin_stub_src_filename;

    write_string_to_file(objc, thin_stub_src_filename);

    const auto install_name_opt = "-Wl,-install_name,"s + stub_dylib_path.string();
    int res{-1};
    try {
        res = subprocess::call({"clang", "-arch", arch.c_str(), "-o", thin_stub_dylib_path.c_str(),
                                thin_stub_src_filename.c_str(), "-shared", "-fobjc-arc",
                                "-framework", "Foundation", install_name_opt.c_str()});
    } catch (const std::runtime_error &e) {
        fmt::print("[-] Error when running stub dylib build: '{:s}'\n", e.what());
        return std::nullopt;
    }
    if (res) {
        fmt::print("[-] Error when running stub dylib build: return code {:d}\n", res);
        return std::nullopt;
    }

    return thin_stub_dylib_path;
}

static bool create_fat_stub_dylib(const fs::path &fat_stub_filename, const fs::path &out_path,
                                  const std::vector<fs::path> &thin_stubs) {
    const auto fat_stub_path = out_path.parent_path() / fat_stub_filename;
    std::vector<std::string> stub_path_strs;
    for (const auto &sp : thin_stubs) {
        stub_path_strs.emplace_back(sp.string());
    }
    const auto cmd = fmt::format("lipo -create -output {:s} {}", fat_stub_path.string(),
                                 fmt::join(stub_path_strs, " "));

    int res{-1};
    try {
        res = subprocess::call(cmd);
    } catch (const std::runtime_error &e) {
        fmt::print("[-] Error when running fat dylib lipo: '{:s}'\n", e.what());
        return false;
    }
    if (res) {
        fmt::print("[-] Error when running fat dylib lipo: return code {:d}\n", res);
        return false;
    }

    return true;
}

static bool dylibify(const std::string &in_path, const std::string &out_path,
                     const std::optional<std::string> dylib_path,
                     const std::vector<std::string> remove_dylibs,
                     const bool auto_remove_dylibs = false, const bool remove_info_plist = false,
                     const bool ios = false, const bool macos = false, const bool verbose = false) {
    assert(!(ios && macos));

    if (verbose) {
        LIEF::logging::set_level(LIEF::logging::LOGGING_LEVEL::LOG_TRACE);
    }

    auto binaries = Parser::parse(in_path);

    fs::path fat_stub_filename{"dylibify-stubs.dylib"};
    std::optional<fs::path> stub_path;
    std::vector<fs::path> thin_stubs;

    for (auto &binary : *binaries) {
        std::map<std::string, const DylibCommand *> orig_libraries;
        for (const auto &dylib_cmd : binary.libraries()) {
            if (dylib_cmd.command() == LOAD_COMMAND_TYPES::LC_ID_DYLIB) {
                continue;
            }
            orig_libraries.emplace(std::make_pair(dylib_cmd.name(), &dylib_cmd));
        }

        std::map<std::string, int32_t> orig_ordinal_map;
        int32_t orig_ordinal_idx{1};
        for (const auto &dylib_cmd : binary.libraries()) {
            if (dylib_cmd.command() == LOAD_COMMAND_TYPES::LC_ID_DYLIB) {
                continue;
            }
            orig_ordinal_map.emplace(std::make_pair(dylib_cmd.name(), orig_ordinal_idx));
            ++orig_ordinal_idx;
        }

        std::map<std::string, std::string> orig_syms_to_libs;
        for (auto &sym : binary.symbols()) {
            if (!sym.has_binding_info() || !sym.binding_info()->has_library()) {
                continue;
            }
            orig_syms_to_libs.emplace(sym.name(), sym.binding_info()->library()->name());
        }

        auto &hdr = binary.header();
        assert(hdr.file_type() == FILE_TYPES::MH_EXECUTE);
        if (verbose) {
            fmt::print("[-] Changing Mach-O type from executable to dylib\n");
        }
        hdr.file_type(FILE_TYPES::MH_DYLIB);
        if (verbose) {
            fmt::print("[-] Adding NO_REXPORTED_LIBS flag\n");
        }
        hdr.flags(hdr.flags() | (uint32_t)HEADER_FLAGS::MH_NO_REEXPORTED_DYLIBS);

        if (binary.code_signature()) {
            if (verbose) {
                fmt::print("[-] Removing code signature\n");
            }
            assert(binary.remove_signature());
        }

        if (const auto *pgz_seg = binary.get_segment("__PAGEZERO")) {
            if (verbose) {
                fmt::print("[-] Removing __PAGEZERO segment\n");
            }
            binary.remove(*pgz_seg);
        }

        fs::path new_dylib_path;
        if (dylib_path != std::nullopt) {
            new_dylib_path = *dylib_path;
        } else {
            fs::path dylib_path{out_path};
            new_dylib_path = fs::path{"@executable_path"} / dylib_path.filename();
        }
        if (verbose) {
            fmt::print("[-] Setting ID_DYLIB path to: '{:s}'\n", new_dylib_path.string());
        }
        const auto id_dylib_cmd = DylibCommand::id_dylib(new_dylib_path, 2, 0x00010000, 0x00010000);
        binary.add(id_dylib_cmd);

        if (remove_info_plist) {
            if (const auto *plist_sect = binary.get_section("__TEXT", "__info_plist")) {
                if (verbose) {
                    fmt::print("[-] Removing __TEXT,__info_plist\n");
                }
                binary.remove_section("__TEXT", "__info_plist", true);
            }
        }

        if (const auto *dylinker_cmd = binary.dylinker()) {
            if (verbose) {
                fmt::print("[-] Removing dynlinker command\n");
            }
            binary.remove(*dylinker_cmd);
        }

        if (const auto *main_cmd = binary.main_command()) {
            if (verbose) {
                fmt::print("[-] Removing MAIN command\n");
            }
            binary.remove(*main_cmd);
        }

        if (const auto *src_cmd = binary.source_version()) {
            if (verbose) {
                fmt::print("[-] Remvoing source version command\n");
            }
            binary.remove(*src_cmd);
        }

        if (ios || macos) {
            if (const auto *minver_cmd = binary.version_min()) {
                if (verbose) {
                    const auto &ver = minver_cmd->version();
                    const auto &sdk = minver_cmd->sdk();
                    fmt::print("[-] Removing old VERSION_MIN command (version: '{:d}.{:d}.{:d}' "
                               "SDK: '{:d}.{:d}.{:d}')\n",
                               ver[0], ver[1], ver[2], sdk[0], sdk[1], sdk[2]);
                }
                binary.remove(*minver_cmd);
            }
            if (const auto *buildver_cmd = binary.build_version()) {
                if (verbose) {
                    const auto *plat  = to_string(buildver_cmd->platform());
                    const auto &minos = buildver_cmd->minos();
                    const auto &sdk   = buildver_cmd->sdk();
                    fmt::print("[-] Removing old BUILD_VERSION command (platform: '{:s}' version: "
                               "'{:d}.{:d}.{:d}' SDK: '{:d}.{:d}.{:d}')\n",
                               plat, minos[0], minos[1], minos[2], sdk[0], sdk[1], sdk[2]);
                }
                binary.remove(*buildver_cmd);
            }
            const BuildVersion::version_t new_minos{11, 0, 0};
            const BuildVersion::version_t new_sdk{new_minos};
            BuildVersion::PLATFORMS new_plat;
            if (ios) {
                new_plat = BuildVersion::PLATFORMS::IOS;
            } else {
                new_plat = BuildVersion::PLATFORMS::MACOS;
            }
            if (verbose) {
                fmt::print("[-] Adding new BUILD_VERSION command (platform: '{:s}' version: "
                           "'{:d}.{:d}.{:d}' SDK: '{:d}.{:d}.{:d}')\n",
                           to_string(new_plat), new_minos[0], new_minos[1], new_minos[2],
                           new_sdk[0], new_sdk[1], new_sdk[2]);
            }
            auto new_buildver_cmd = BuildVersion{new_plat, new_minos, new_sdk, {}};
            binary.add(new_buildver_cmd);
        }

        std::set<std::string> remove_dylib_set;
        for (const auto &dylib : remove_dylibs) {
            if (!orig_libraries.contains(dylib)) {
                fmt::print("[!] Asked to remove dylib '{:s}' but it wasn't found in the imports\n");
                return false;
            }
            remove_dylib_set.emplace(dylib);
        }

        if (auto_remove_dylibs) {
            for (const auto &i : orig_libraries) {
                if (!dylib_exists(i.first)) {
                    if (verbose) {
                        fmt::print("[-] Marking unavailable dylib '{:s}' for removal\n", i.first);
                    }
                    remove_dylib_set.emplace(i.first);
                }
            }
        }

        std::set<std::string> remove_sym_set;
        for (const auto &sym_map : orig_syms_to_libs) {
            if (remove_dylib_set.contains(sym_map.second)) {
                if (verbose) {
                    fmt::print("[-] Marking symbol '{:s}' from dylib '{:s}' for stubbing\n",
                               sym_map.first, sym_map.second);
                }
                remove_sym_set.emplace(sym_map.first);
            }
        }

        std::set<int32_t> removed_ordinals;
        for (const auto &dylib : remove_dylib_set) {
            const auto *dylib_cmd = orig_libraries[dylib];
            if (verbose) {
                fmt::print("[-] Removing dependant dylib '{:s}'\n", dylib);
            }
            removed_ordinals.emplace(orig_ordinal_map[dylib_cmd->name()]);
            binary.remove(*dylib_cmd);
        }

        if (remove_sym_set.size()) {
            *stub_path = new_dylib_path.parent_path() / fat_stub_filename;
            if (verbose) {
                fmt::print("Creating stub library import '{:s}'\n", stub_path->string());
            }
            const auto stub_dylib_cmd =
                DylibCommand::load_dylib(*stub_path, 2, 0x00010000, 0x00010000);
            binary.add(stub_dylib_cmd);
        }

        std::map<std::string, int32_t> new_ordinal_map;
        int32_t new_ordinal_idx{1};
        for (const auto &dylib_cmd : binary.libraries()) {
            if (dylib_cmd.command() == LOAD_COMMAND_TYPES::LC_ID_DYLIB) {
                continue;
            }
            new_ordinal_map.emplace(std::make_pair(dylib_cmd.name(), new_ordinal_idx));
            ++new_ordinal_idx;
        }

        std::map<int32_t, int32_t> orig_to_new_ordinal_map;
        for (const auto &old_it : orig_ordinal_map) {
            const auto &orig_lib = old_it.first;
            const auto orig_ord  = old_it.second;
            if (new_ordinal_map.contains(orig_lib)) {
                orig_to_new_ordinal_map.emplace(
                    std::make_pair(orig_ord, new_ordinal_map[orig_lib]));
            } else {
                assert(remove_dylib_set.contains(orig_lib));
                orig_to_new_ordinal_map.emplace(
                    std::make_pair(orig_ord, new_ordinal_map[*stub_path]));
            }
        }

        if (verbose) {
            fmt::print("[-] Updating library ordinals in binding info\n");
        }
        for (auto &binding_info : binary.dyld_info()->bindings()) {
            binding_info.library_ordinal(
                orig_to_new_ordinal_map.at(binding_info.library_ordinal()));
        }

        if (verbose) {
            fmt::print("[-] Updating library ordinals in symtab\n");
        }
        for (auto &sym : binary.symbols()) {
            if (sym.origin() != SYMBOL_ORIGINS::SYM_ORIGIN_LC_SYMTAB) {
                continue;
            }
            const auto orig_ord = get_library_ordinal(sym.description());
            if (orig_ord == (uint8_t)SYMBOL_DESCRIPTIONS::SELF_LIBRARY_ORDINAL ||
                orig_ord == (uint8_t)SYMBOL_DESCRIPTIONS::DYNAMIC_LOOKUP_ORDINAL ||
                orig_ord == (uint8_t)SYMBOL_DESCRIPTIONS::EXECUTABLE_ORDINAL) {
                continue;
            }
            const auto new_ord = orig_to_new_ordinal_map.at(orig_ord);
            auto new_desc      = sym.description();
            set_library_ordinal(new_desc, new_ord);
            sym.description(new_desc);
        }

        if (remove_sym_set.size()) {
            const auto cpu_type = binary.header().cpu_type();
            if (verbose) {
                fmt::print("[-] Codegening and building stub dylib for arch {:s} '{:s}'\n",
                           to_string(cpu_type), stub_path->string());
            }
            const auto thin_stub_path = create_thin_stub_dylib(
                fat_stub_filename, out_path, *stub_path, remove_sym_set, cpu_type);
            if (thin_stub_path == std::nullopt) {
                fmt::print("[!] Error generating stub dylib for arch {:s}!\n", to_string(cpu_type));
                return false;
            } else {
                thin_stubs.emplace_back(*thin_stub_path);
            }
        }
    }

    if (thin_stubs.size()) {
        if (verbose) {
            fmt::print("[-] Generating fat stub dylib at '{:s}'\n", fat_stub_filename.string());
        }
        if (!create_fat_stub_dylib(fat_stub_filename, out_path, thin_stubs)) {
            fmt::print("[!] Error generating fat stub dylib!\n");
            return false;
        }
    }

    binaries->write(out_path);
    return true;
}

int main(int argc, const char **argv) {
    argparse::ArgumentParser parser(getprogname());
    parser.add_argument("-i", "--in").required().help("input Mach-O executable");
    parser.add_argument("-o", "--out").required().help("output Mach-O dylib");
    parser.add_argument("-d", "--dylib-path")
        .help("path for LC_ID_DYLIB command. e.g. @executable_path/Frameworks/libfoo.dylib");
    parser.add_argument("-r", "--remove-dylib")
        .nargs(argparse::nargs_pattern::any)
        .help("remove dylib dependency");
    parser.add_argument("-R", "--auto-remove-dylibs")
        .default_value(false)
        .implicit_value(true)
        .help("automatically remove unavailable dylib dependencies");
    parser.add_argument("-P", "--remove-info-plist")
        .default_value(false)
        .implicit_value(true)
        .help("remove __info_plist section");
    parser.add_argument("-I", "--ios")
        .default_value(false)
        .implicit_value(true)
        .help("patch platform to iOS");
    parser.add_argument("-M", "--macos")
        .default_value(false)
        .implicit_value(true)
        .help("patch platform to macOS");
    parser.add_argument("-V", "--verbose")
        .default_value(false)
        .implicit_value(true)
        .help("verbose mode");

    try {
        parser.parse_args(argc, argv);
    } catch (const std::runtime_error &err) {
        fmt::print(stderr, "Error parsing arguments: {:s}\n", err.what());
        return -1;
    }

    const auto res = dylibify(
        parser.get<std::string>("--in"), parser.get<std::string>("--out"),
        parser.present("--dylib-path"), parser.get<std::vector<std::string>>("--remove-dylib"),
        parser.get<bool>("--auto-remove-dylibs"), parser.get<bool>("--remove-info-plist"),
        parser.get<bool>("--ios"), parser.get<bool>("--macos"), parser.get<bool>("--verbose"));

    return res ? 0 : 1;
}
