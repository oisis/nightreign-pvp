// Decompress the Kraken body of a FromSoftware DCX file with the game's own Oodle library.
//   unkrak.exe <oo2core_9_win64.dll> <in.dcx> <out>
// The DLL is the user's own copy from the game folder; it is never shipped with the mod.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long (*decompress_fn)(const void *src, long long src_len, void *dst, long long dst_len,
                                   int fuzz_safe, int check_crc, int verbosity, void *dec_base,
                                   long long dec_size, void *cb, void *cb_user, void *mem,
                                   long long mem_size, int thread_phase);

static unsigned be32(const unsigned char *p) { return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(int argc, char **argv) {
    if (argc != 4) return fprintf(stderr, "usage: unkrak <oo2core dll> <in.dcx> <out>\n"), 2;
    HMODULE dll = LoadLibraryA(argv[1]);
    decompress_fn decompress = dll ? (decompress_fn)GetProcAddress(dll, "OodleLZ_Decompress") : NULL;
    if (!decompress) return fprintf(stderr, "cannot load OodleLZ_Decompress from %s\n", argv[1]), 1;

    FILE *f = fopen(argv[2], "rb");
    if (!f) return fprintf(stderr, "cannot open %s\n", argv[2]), 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    if (n < 0x4c || memcmp(d, "DCX", 3) || memcmp(d + 0x28, "KRAK", 4))
        return fprintf(stderr, "%s is not a Kraken DCX\n", argv[2]), 1;

    unsigned usize = be32(d + 0x1c), csize = be32(d + 0x20);
    if ((long)csize > n - 0x4c) return fprintf(stderr, "truncated file\n"), 1;
    unsigned char *out = malloc(usize + 64);
    long long got = decompress(d + 0x4c, csize, out, usize, 1, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 3);
    if (got != usize) return fprintf(stderr, "decompress returned %lld, expected %u\n", got, usize), 1;

    f = fopen(argv[3], "wb");
    if (!f || fwrite(out, 1, usize, f) != usize) return fprintf(stderr, "cannot write %s\n", argv[3]), 1;
    fclose(f);
    return 0;
}
