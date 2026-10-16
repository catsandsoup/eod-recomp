// Explicit read-only source import harness. No UI or runtime dependencies.
#include "../runtime/app/disc_import.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr,"usage: disc_import_cli SOURCE DEST ELF_SIZE CRC_HEX\n"); return 2; }
    DiscId id{(uint32_t)strtoul(argv[3],nullptr,10),(uint32_t)strtoul(argv[4],nullptr,16)};
    auto r=disc_import(argv[1],argv[2],id);
    if(r.ok()) r=disc_check_folder(argv[2],id);
    if(!r.ok()) { fprintf(stderr,"%s\n",r.message.c_str());return 1; }
    printf("Import and installed ELF size/CRC verified.\n");return 0;
}
