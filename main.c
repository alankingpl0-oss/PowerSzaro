/* =========================================================================
   Główny moduł zarządzający procesem konwersji grafiki (PEŁNA WERSJA)
   Obsługiwane flagi:
     -z  / --min-size       Zapis w formacie PGM (P5) – mniejsza waga pliku
     -q  / --quality MODE   Jakość konwersji: fast | balanced | precise
     -i  / --invert         Negatyw (odwrócenie jasności)
     -t  / --threshold N    Binaryzacja do czarno-białego (próg 0–255)
     -b  / --brightness N   Korekcja jasności (zakres -255..+255)
     -B  / --benchmark      Pomiar i wyświetlenie czasu konwersji
         --info             Wyświetl metadane BMP bez konwersji
         --no-avx2          Wymuś skalarny fallback (bez AVX2)
   ========================================================================= */

#include <stdint.h>
#include <stddef.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
   Struktury konfiguracyjne
   ========================================================================= */

typedef enum {
    QUALITY_FAST     = 0,   /* Współczynniki całkowitoliczbowe, szybki shift */
    QUALITY_BALANCED = 1,   /* BT.601 (domyślny: 77/150/29) */
    QUALITY_PRECISE  = 2    /* BT.709 HDTV: 0.2126 / 0.7152 / 0.0722 */
} QualityMode;

typedef struct {
    /* Wejście / wyjście */
    const char* input_path;
    const char* output_path;

    /* Tryby przetwarzania */
    QualityMode  quality;       /* fast / balanced / precise              */
    int          min_size;      /* 1 = zapis PGM zamiast BMP              */
    int          invert;        /* 1 = negatyw                            */
    int          threshold;     /* -1 = wyłączony; 0–255 = próg           */
    int          brightness;    /* korekcja jasności: -255..+255          */
    int          benchmark;     /* 1 = wyświetl czas konwersji            */
    int          info_only;     /* 1 = tylko metadane, bez konwersji      */
    int          force_scalar;  /* 1 = wymuś skalarny fallback            */
    int niby_crt;    /* 1 = włącz efekt linii CRT */
} Config;

/* =========================================================================
   Współczynniki jakości (przeskalowane x256 dla FAST/BALANCED,
   x65536 dla PRECISE – by uniknąć float)
   ========================================================================= */

typedef struct { uint16_t r, g, b; int shift; } Coeffs;

static Coeffs get_coeffs(QualityMode q) {
    switch (q) {
        case QUALITY_FAST:
            /* Przybliżenie: 1/4 R + 1/2 G + 1/8 B ≈ grayscale */
            return (Coeffs){ 64, 128, 32, 8 };
        case QUALITY_PRECISE:
            /* BT.709: R*13933 + G*46871 + B*4732  (suma = 65536) */
            return (Coeffs){ 13933, 46871, 4732, 16 };
        default: /* QUALITY_BALANCED = BT.601 */
            return (Coeffs){ 77, 150, 29, 8 };
    }
}

/* =========================================================================
   Silnik konwersji RGBA → skala szarości (AVX2)
   ========================================================================= */

void convert_rgba_to_gray_avx2(
    const uint8_t* __restrict input,
          uint8_t* __restrict output,
    size_t pixel_count,
    const Coeffs* c)
{
    size_t i = 0;

    __m256i v_r = _mm256_set1_epi16((short)c->r);
    __m256i v_g = _mm256_set1_epi16((short)c->g);
    __m256i v_b = _mm256_set1_epi16((short)c->b);

    __m256i shuf_mask = _mm256_set_epi8(
        -1,-1,-1,-1,-1,-1,-1,-1, 14,10, 6, 2, 13, 9, 5, 1,
        -1,-1,-1,-1,-1,-1,-1,-1, 14,10, 6, 2, 13, 9, 5, 1
    );

    for (; i < (pixel_count & ~7UL); i += 8) {
        __m256i rgba     = _mm256_loadu_si256((const __m256i*)(input + i * 4));
        __m256i shuffled = _mm256_shuffle_epi8(rgba, shuf_mask);
        __m128i low      = _mm256_castsi256_si128(shuffled);

        __m256i r_w = _mm256_cvtepu8_epi16(low);
        __m256i g_w = _mm256_cvtepu8_epi16(_mm_srli_si128(low, 4));
        __m256i b_w = _mm256_cvtepu8_epi16(_mm_srli_si128(low, 8));

        __m256i sum = _mm256_add_epi16(
                        _mm256_mullo_epi16(r_w, v_r),
                        _mm256_add_epi16(
                            _mm256_mullo_epi16(g_w, v_g),
                            _mm256_mullo_epi16(b_w, v_b)));

        __m256i gray = _mm256_srli_epi16(sum, (int)c->shift);
        __m256i pack = _mm256_packus_epi16(gray, gray);

        uint64_t res = (uint64_t)_mm_cvtsi128_si64(_mm256_castsi256_si128(pack));
        *(uint64_t*)(output + i) = res;
    }

    for (; i < pixel_count; i++) {
        uint8_t r = input[i*4+0], g = input[i*4+1], b = input[i*4+2];
        uint32_t v = ((uint32_t)c->r*r + (uint32_t)c->g*g + (uint32_t)c->b*b) >> c->shift;
        output[i] = (v > 255) ? 255 : (uint8_t)v;
    }
}

/* =========================================================================
   Silnik skalarny (fallback / --no-avx2)
   ========================================================================= */

void convert_rgba_to_gray_scalar(
    const uint8_t* __restrict input,
          uint8_t* __restrict output,
    size_t pixel_count,
    const Coeffs* c)
{
    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t r = input[i*4+0], g = input[i*4+1], b = input[i*4+2];
        uint32_t v = ((uint32_t)c->r*r + (uint32_t)c->g*g + (uint32_t)c->b*b) >> c->shift;
        output[i] = (v > 255) ? 255 : (uint8_t)v;
    }
}

/* =========================================================================
   Postprocessing: invert / threshold / brightness
   ========================================================================= */

static inline uint8_t clamp_u8(int v) {
    return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

void apply_postprocess(uint8_t* buf, size_t n, const Config* cfg) {
    for (size_t i = 0; i < n; i++) {
        int v = buf[i];

        if (cfg->brightness != 0)
            v = clamp_u8(v + cfg->brightness);

        if (cfg->threshold >= 0)
            v = (v >= cfg->threshold) ? 255 : 0;

        if (cfg->invert)
            v = 255 - v;

        buf[i] = (uint8_t)v;
    }
}

/* =========================================================================
   Ładowanie BMP 24-bit → RGBA
   ========================================================================= */

uint8_t* load_bmp_to_rgba(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Błąd: nie można otworzyć '%s'\n", path); return NULL; }

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fprintf(stderr, "Błąd: nieprawidłowy nagłówek BMP\n"); fclose(f); return NULL;
    }

    int bpp = *(uint16_t*)&hdr[28];
    if (bpp != 24) {
        fprintf(stderr, "Błąd: obsługiwane są tylko pliki BMP 24-bit (wykryto %d-bit)\n", bpp);
        fclose(f); return NULL;
    }

    *w = *(int32_t*)&hdr[18];
    *h = *(int32_t*)&hdr[22];

    size_t pixels = (size_t)(*w) * (*h);
    uint8_t* buf  = (uint8_t*)malloc(pixels * 4);
    int row_sz    = ((*w) * 3 + 3) & ~3;
    uint8_t* row  = (uint8_t*)malloc(row_sz);

    if (!buf || !row) { free(buf); free(row); fclose(f); return NULL; }

    for (int y = (*h)-1; y >= 0; y--) {
        if (fread(row, 1, row_sz, f) != (size_t)row_sz) {
            fprintf(stderr, "Błąd: nie udało się odczytać wiersza %d\n", y);
            free(buf); free(row); fclose(f); return NULL;
        }
        for (int x = 0; x < *w; x++) {
            size_t d = ((size_t)y * (*w) + x) * 4;
            buf[d+0] = row[x*3+2];  /* R (BMP przechowuje BGR) */
            buf[d+1] = row[x*3+1];  /* G */
            buf[d+2] = row[x*3+0];  /* B */
            buf[d+3] = 255;
        }
    }
    free(row); fclose(f);
    return buf;
}

void apply_crt_effect(uint8_t* buf, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Co drugi piksel w poziomie przyciemniamy – efekt linii CRT */
            if (x % 2 == 0) {
                size_t idx = (size_t)y * w + x;
                buf[idx] = (uint8_t)(buf[idx] * 0.85); /* 15% przyciemnienia */
            }
        }
    }
}

/* =========================================================================
   Zapis BMP 8-bit (skala szarości)
   ========================================================================= */

void save_bmp_gray(const char* path, const uint8_t* data, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Błąd: nie można zapisać '%s'\n", path); return; }

    int row_sz    = (w + 3) & ~3;
    int file_size = 54 + 1024 + row_sz * h;

    uint8_t fh[14] = {
        'B','M',
        (uint8_t)file_size, (uint8_t)(file_size>>8),
        (uint8_t)(file_size>>16), (uint8_t)(file_size>>24),
        0,0,0,0,
        (uint8_t)(54+1024), (uint8_t)((54+1024)>>8), 0,0
    };
    uint8_t ih[40] = {
        40,0,0,0,
        (uint8_t)w,(uint8_t)(w>>8),(uint8_t)(w>>16),(uint8_t)(w>>24),
        (uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24),
        1,0, 8,0
    };

    fwrite(fh, 1, 14, f);
    fwrite(ih, 1, 40, f);
    for (int i = 0; i < 256; i++) {
        uint8_t p[4] = {(uint8_t)i,(uint8_t)i,(uint8_t)i,0};
        fwrite(p, 1, 4, f);
    }

    uint8_t pad[3] = {0,0,0};
    for (int y = h-1; y >= 0; y--) {
        fwrite(&data[(size_t)y*w], 1, w, f);
        if (row_sz > w) fwrite(pad, 1, row_sz - w, f);
    }
    fclose(f);
}

/* =========================================================================
   Zapis PGM (format Netpbm P5) – mniejsza waga pliku (--min-size)
   Brak palety, brak paddingu – ~4x mniejszy plik niż BMP
   ========================================================================= */

void save_pgm(const char* path, const uint8_t* data, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Błąd: nie można zapisać '%s'\n", path); return; }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(data, 1, (size_t)w * h, f);
    fclose(f);
}

/* =========================================================================
   Wyświetlenie metadanych BMP (--info)
   ========================================================================= */

void print_bmp_info(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Błąd: nie można otworzyć '%s'\n", path); return; }

    uint8_t h[54];
    if (fread(h, 1, 54, f) != 54 || h[0] != 'B' || h[1] != 'M') {
        fprintf(stderr, "Błąd: nieprawidłowy plik BMP\n"); fclose(f); return;
    }
    fclose(f);

    int w   = *(int32_t*)&h[18];
    int ht  = *(int32_t*)&h[22];
    int bpp = *(uint16_t*)&h[28];
    uint32_t fsz = *(uint32_t*)&h[2];
    uint32_t off = *(uint32_t*)&h[10];
    uint32_t comp= *(uint32_t*)&h[30];

    printf("=== Informacje o pliku BMP ===\n");
    printf("  Ścieżka     : %s\n", path);
    printf("  Rozmiar     : %u bajtów (%.1f KB)\n", fsz, fsz/1024.0);
    printf("  Wymiary     : %d x %d pikseli\n", w, ht);
    printf("  Głębia      : %d bpp\n", bpp);
    printf("  Offset danych: %u\n", off);
    printf("  Kompresja   : %s\n", comp == 0 ? "BI_RGB (brak)" : "skompresowany");
    printf("  Liczba px   : %d\n", w * ht);
    printf("  Rozmiar wiersza (z paddingiem): %d bajtów\n", (w * (bpp/8) + 3) & ~3);
}

/* =========================================================================
   Wykrywanie AVX2
   ========================================================================= */

int check_avx2(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_cpu_supports("avx2");
#endif
    return 0;
}

/* =========================================================================
   Parser argumentów
   ========================================================================= */

static void print_usage(const char* prog) {
    printf("PowerSzaro. Użycie: %s <wejście.bmp> <wyjście> [opcje]\n\n", prog);
    printf("Opcje:\n");
    printf("  -z,  --min-size          Zapis jako PGM zamiast BMP (mniejsza waga)\n");
    printf("  -q,  --quality MODE      Jakość konwersji: fast | balanced (domyślna) | precise\n");
    printf("  -i,  --invert            Negatyw obrazu\n");
    printf("  -t,  --threshold N       Binaryzacja (próg 0-255)\n");
    printf("  -b,  --brightness N      Korekcja jasności (-255..+255)\n");
    printf("  -B,  --benchmark         Wyświetl czas przetwarzania\n");
    printf("       --niby-crt          Włącz efekt linii CRT (symulacja monitora)\n");
    printf("       --info              Pokaż metadane pliku BMP (bez konwersji)\n");
    printf("       --no-avx2           Wymuś skalarny fallback (bez AVX2)\n");
    printf("  -h,  --help              Wyświetl tę pomoc\n\n");
    printf("Przykłady:\n");
    printf("  %s foto.bmp gray.bmp\n", prog);
    printf("  %s foto.bmp gray.pgm --min-size --niby-crt\n", prog);
    printf("  %s foto.bmp out.bmp --invert --brightness 30 --benchmark\n", prog);
}

/* W sekcji parse_args naprawiamy obsługę flagi */
static int parse_args(int argc, char* argv[], Config* cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->quality   = QUALITY_BALANCED;
    cfg->threshold = -1;
    cfg->niby_crt  = 0;

    int pos = 0;

    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        char* a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(a, "-z") == 0 || strcmp(a, "--min-size") == 0) {
            cfg->min_size = 1;
        }
        else if (strcmp(a, "-i") == 0 || strcmp(a, "--invert") == 0) {
            cfg->invert = 1;
        }
        else if (strcmp(a, "-B") == 0 || strcmp(a, "--benchmark") == 0) {
            cfg->benchmark = 1;
        }
        else if (strcmp(a, "--info") == 0) {
            cfg->info_only = 1;
        }
        else if (strcmp(a, "--no-avx2") == 0) {
            cfg->force_scalar = 1;
        }
        else if (strcmp(a, "--niby-crt") == 0) {
            cfg->niby_crt = 1;
        }
        else if ((strcmp(a, "-q") == 0 || strcmp(a, "--quality") == 0) && i + 1 < argc) {

            i++;

            if (strcmp(argv[i], "fast") == 0)
                cfg->quality = QUALITY_FAST;
            else if (strcmp(argv[i], "balanced") == 0)
                cfg->quality = QUALITY_BALANCED;
            else if (strcmp(argv[i], "precise") == 0)
                cfg->quality = QUALITY_PRECISE;
            else {
                fprintf(stderr,
                        "Błąd: nieznany tryb jakości '%s'\n",
                        argv[i]);
                return 0;
            }
        }
        else if ((strcmp(a, "-t") == 0 || strcmp(a, "--threshold") == 0) && i + 1 < argc) {

            cfg->threshold = atoi(argv[++i]);

            if (cfg->threshold < 0 || cfg->threshold > 255) {
                fprintf(stderr,
                        "Błąd: próg musi być z zakresu 0-255\n");
                return 0;
            }
        }
        else if ((strcmp(a, "-b") == 0 || strcmp(a, "--brightness") == 0) && i + 1 < argc) {

            cfg->brightness = atoi(argv[++i]);

            if (cfg->brightness < -255 || cfg->brightness > 255) {
                fprintf(stderr,
                        "Błąd: jasność musi być z zakresu -255..255\n");
                return 0;
            }
        }
        else if (a[0] != '-') {

            if (pos == 0) {
                cfg->input_path = a;
                pos++;
            }
            else if (pos == 1) {
                cfg->output_path = a;
                pos++;
            }
        }
        else {
            fprintf(stderr,
                    "Błąd: nieznana flaga '%s'\n",
                    a);
            return 0;
        }
    }

    if (!cfg->input_path) {
        fprintf(stderr,
                "Błąd: brak pliku wejściowego\n");
        return 0;
    }

    if (!cfg->info_only && !cfg->output_path) {
        fprintf(stderr,
                "Błąd: brak pliku wyjściowego\n");
        return 0;
    }

    return 1;
}

/* W sekcji main dodajemy podwójne zabezpieczenie przed 'duchami' */
/* W sekcji main dodajemy podwójne zabezpieczenie przed 'duchami' */


/* =========================================================================
   Main
   ========================================================================= */

int main(int argc, char* argv[]) {
    Config cfg;
    if (!parse_args(argc, argv, &cfg)) return 1;

    /* Tryb --info: tylko metadane */
    if (cfg.info_only) {
        print_bmp_info(cfg.input_path);
        return 0;
    }

    /* Wczytanie obrazu – musimy wiedzieć, jaki jest rozmiar, by zdecydować o AVX2 */
    int width, height;
    uint8_t* input = load_bmp_to_rgba(cfg.input_path, &width, &height);
    if (!input) return 1;

    /* Sprawdzenie dostępności AVX2 i wymuszenie skalarnego dla niepodzielnej szerokości */
    int use_avx2 = !cfg.force_scalar && check_avx2();
    if (use_avx2 && (width % 8 != 0)) {
        use_avx2 = 0; /* AVX2 przy niepodzielnej szerokości mogłoby coś zepsuć koncertowo */
    }

    size_t npx = (size_t)width * height;
    uint8_t* output = (uint8_t*)malloc(npx);
    if (!output) { 
        fprintf(stderr, "Błąd alokacji pamięci\n"); 
        free(input); 
        return 1; 
    }

    Coeffs coeffs = get_coeffs(cfg.quality);

    /* Konwersja z opcjonalnym benchmarkiem */
    struct timespec t0, t1;
    if (cfg.benchmark) clock_gettime(CLOCK_MONOTONIC, &t0);


convert_rgba_to_gray_scalar(input, output, npx, &coeffs);
/*
    if (use_avx2)
        convert_rgba_to_gray_avx2(input, output, npx, &coeffs);
    else
        convert_rgba_to_gray_scalar(input, output, npx, &coeffs);

*/

    if (cfg.benchmark) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                  + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        double mpx_s = (npx / 1e6) / (ms / 1000.0);
        printf("[benchmark] Czas konwersji : %.3f ms\n", ms);
        printf("[benchmark] Przepustowość  : %.1f Mpx/s\n", mpx_s);
        printf("[benchmark] Silnik         : %s\n", use_avx2 ? "AVX2" : "skalarny");
        printf("[benchmark] Jakość         : %s\n",
            cfg.quality == QUALITY_FAST ? "fast" :
            cfg.quality == QUALITY_PRECISE ? "precise" : "balanced");
    }

    /* Postprocessing */
    int needs_post = cfg.invert || cfg.threshold >= 0 || cfg.brightness != 0;
    if (needs_post)
        apply_postprocess(output, npx, &cfg);

    /* Opcjonalny efekt CRT */
    if (cfg.niby_crt) {
        apply_crt_effect(output, width, height);
    }

    /* Zapis */
    if (cfg.min_size)
        save_pgm(cfg.output_path, output, width, height);
    else
        save_bmp_gray(cfg.output_path, output, width, height);

    /* Podsumowanie */
    printf("Zapisano: %s (%dx%d, %s)\n",
        cfg.output_path, width, height,
        cfg.min_size ? "PGM" : "BMP 8-bit");

    free(input);
    free(output);
    return 0;
}