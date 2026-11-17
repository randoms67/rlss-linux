#include "elf_scan.h"

#include <elf.h>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

template <class Ehdr, class Shdr, class Sym, class Dyn>
void parse(const std::string& data, ElfInfo& info) {
    if(data.size() < sizeof(Ehdr))
        return;
    auto eh = (const Ehdr*)data.data();
    info.machine = eh->e_machine;
    if(eh->e_shoff == 0 || eh->e_shentsize != sizeof(Shdr) ||
       eh->e_shoff + (size_t)eh->e_shnum * sizeof(Shdr) > data.size())
        return;
    auto sh = (const Shdr*)(data.data() + eh->e_shoff);
    auto inRange = [&](size_t off, size_t len) { return off <= data.size() && len <= data.size() - off; };
    for(int i = 0; i < eh->e_shnum; i++) {
        if(sh[i].sh_type == SHT_DYNSYM && sh[i].sh_link < eh->e_shnum) {
            auto& strh = sh[sh[i].sh_link];
            if(!inRange(sh[i].sh_offset, sh[i].sh_size) || !inRange(strh.sh_offset, strh.sh_size))
                continue;
            const char* strtab = data.data() + strh.sh_offset;
            size_t count = sh[i].sh_size / sizeof(Sym);
            auto syms = (const Sym*)(data.data() + sh[i].sh_offset);
            for(size_t s = 1; s < count; s++) {
                if(syms[s].st_name >= strh.sh_size)
                    continue;
                std::string name(strtab + syms[s].st_name);
                if(name.empty())
                    continue;
                int bind = syms[s].st_info >> 4;
                if(syms[s].st_shndx == SHN_UNDEF) {
                    if(bind == STB_WEAK)
                        info.undefinedWeak.insert(name);
                    else
                        info.undefinedStrong.insert(name);
                } else if(bind == STB_GLOBAL || bind == STB_WEAK || bind == 10 /* STB_GNU_UNIQUE */) {
                    info.defined.insert(name);
                }
            }
        } else if(sh[i].sh_type == SHT_DYNAMIC && sh[i].sh_link < eh->e_shnum) {
            auto& strh = sh[sh[i].sh_link];
            if(!inRange(sh[i].sh_offset, sh[i].sh_size) || !inRange(strh.sh_offset, strh.sh_size))
                continue;
            const char* strtab = data.data() + strh.sh_offset;
            auto dyn = (const Dyn*)(data.data() + sh[i].sh_offset);
            size_t count = sh[i].sh_size / sizeof(Dyn);
            for(size_t d = 0; d < count && dyn[d].d_tag != DT_NULL; d++) {
                if(dyn[d].d_un.d_val >= strh.sh_size)
                    continue;
                if(dyn[d].d_tag == DT_NEEDED)
                    info.needed.emplace_back(strtab + dyn[d].d_un.d_val);
                else if(dyn[d].d_tag == DT_SONAME)
                    info.soname = strtab + dyn[d].d_un.d_val;
            }
        }
    }
    info.valid = true;
}

}  // namespace

ElfInfo ElfInfo::read(const std::string& path) {
    ElfInfo info;
    std::ifstream in(path, std::ios::binary);
    if(!in)
        return info;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();
    if(data.size() < EI_NIDENT || memcmp(data.data(), ELFMAG, SELFMAG) != 0)
        return info;
    if(data[EI_CLASS] == ELFCLASS64) {
        info.is64 = true;
        parse<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym, Elf64_Dyn>(data, info);
    } else if(data[EI_CLASS] == ELFCLASS32) {
        parse<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym, Elf32_Dyn>(data, info);
    }
    return info;
}
