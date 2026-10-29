// Round-trip checks for lib7zip_apple.a: every call MacRAR makes, against
// archives this test creates itself.
//
//   clang -I7zip-apple/include 7zip-apple/test/roundtrip.c \
//         7zip-apple/build/<arch>/lib7zip_apple.a -lc++ -framework CoreFoundation \
//         -o roundtrip && ./roundtrip

#include "7zip_apple.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static char root[512];

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                   printf("  [last error: %s]\n", sevenzip_last_error()); } \
} while (0)

#define EXPECT(call, code) do { \
    int32_t got_ = (call); \
    CHECK(got_ == (code), "%s returned %d, expected %d\n", #call, got_, (code)); \
} while (0)

static const char *path(const char *name)
{
    static char buffers[8][1024];
    static int next = 0;
    char *p = buffers[next++ % 8];
    snprintf(p, 1024, "%s/%s", root, name);
    return p;
}

static void write_file(const char *name, const char *contents, size_t size)
{
    FILE *f = fopen(path(name), "wb");
    if (!f) { perror(name); exit(2); }
    fwrite(contents, 1, size, f);
    fclose(f);
}

static char *read_file(const char *name, size_t *size)
{
    FILE *f = fopen(path(name), "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)n + 1);
    *size = fread(data, 1, (size_t)n, f);
    fclose(f);
    return data;
}

static int exists(const char *name)
{
    struct stat st;
    return stat(path(name), &st) == 0;
}

static int same_file(const char *a, const char *b)
{
    size_t na = 0, nb = 0;
    char *da = read_file(a, &na), *db = read_file(b, &nb);
    int same = da && db && na == nb && memcmp(da, db, na) == 0;
    free(da);
    free(db);
    return same;
}

static void make_dir(const char *name)
{
    if (mkdir(path(name), 0755) != 0 && errno != EEXIST) { perror(name); exit(2); }
}

static int listed(const char *archive, const char *password, uint32_t *count, SevenZipListing **out)
{
    SevenZipListing *listing = NULL;
    int32_t rc = sevenzip_list(path(archive), password, &listing);
    if (count) *count = sevenzip_listing_count(listing);
    if (out) *out = listing; else sevenzip_listing_free(listing);
    return rc;
}

static int has_item(SevenZipListing *listing, const char *name)
{
    SevenZipItem item;
    for (uint32_t i = 0; sevenzip_listing_item(listing, i, &item); i++)
        if (strcmp(item.path, name) == 0) return 1;
    return 0;
}

static bool cancel_at_once(void *context, uint64_t completed, uint64_t total)
{
    (void)context; (void)completed; (void)total;
    return true;
}

static bool count_progress(void *context, uint64_t completed, uint64_t total)
{
    (void)completed; (void)total;
    (*(int *)context)++;
    return false;
}

static unsigned long crc32_of(const char *data, size_t size)
{
    unsigned long crc = 0xFFFFFFFFul;
    for (size_t i = 0; i < size; i++) {
        crc ^= (unsigned char)data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320ul & (0ul - (crc & 1)));
    }
    return ~crc & 0xFFFFFFFFul;
}

static void put16(unsigned char **p, unsigned long v) { *(*p)++ = v & 0xFF; *(*p)++ = (v >> 8) & 0xFF; }
static void put32(unsigned char **p, unsigned long v) { put16(p, v & 0xFFFF); put16(p, v >> 16); }

/* A ZIP of stored files as Windows' built-in zip writes it: no UTF-8 flag and
   no Unicode extra field, each name in the bytes given, each file holding its
   own name. host is the "made by" system: 0 FAT, 3 Unix, 14 VFAT. */
static void write_windows_zip(const char *name, const char *const *names, const unsigned *host, int count)
{
    static unsigned char zip[4096], central[2048];
    unsigned char *p = zip, *c = central;
    for (int i = 0; i < count; i++) {
        const size_t size = strlen(names[i]);
        const unsigned long crc = crc32_of(names[i], size), offset = (unsigned long)(p - zip);
        put32(&p, 0x04034b50); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0x6000); put16(&p, 0x5B4D);
        put32(&p, crc); put32(&p, size); put32(&p, size); put16(&p, size); put16(&p, 0);
        memcpy(p, names[i], size); p += size;
        memcpy(p, names[i], size); p += size;
        put32(&c, 0x02014b50); put16(&c, (host[i] << 8) | 20); put16(&c, 20); put16(&c, 0); put16(&c, 0);
        put16(&c, 0x6000); put16(&c, 0x5B4D); put32(&c, crc); put32(&c, size); put32(&c, size);
        put16(&c, size); put16(&c, 0); put16(&c, 0); put16(&c, 0); put16(&c, 0); put32(&c, 0x20); put32(&c, offset);
        memcpy(c, names[i], size); c += size;
    }
    const unsigned long central_offset = (unsigned long)(p - zip), central_size = (unsigned long)(c - central);
    memcpy(p, central, central_size); p += central_size;
    put32(&p, 0x06054b50); put16(&p, 0); put16(&p, 0); put16(&p, count); put16(&p, count);
    put32(&p, central_size); put32(&p, central_offset); put16(&p, 0);
    write_file(name, (const char *)zip, (size_t)(p - zip));
}

/* A tar written by hand: an entry for each name, a folder when it ends in
   "/", a file holding "x" otherwise; names such as "./a.txt" or "/abs.txt"
   stay as given, as tar -C dir . and other tools write them. */
static void write_tar(const char *name, const char *const *names, int count)
{
    static char tar[16384];
    memset(tar, 0, sizeof tar);
    char *p = tar;
    for (int i = 0; i < count; i++) {
        const size_t length = strlen(names[i]);
        const int folder = names[i][length - 1] == '/';
        memcpy(p, names[i], length);
        memcpy(p + 100, folder ? "0000755" : "0000644", 7);
        memcpy(p + 108, "0000000", 7);
        memcpy(p + 116, "0000000", 7);
        snprintf(p + 124, 12, "%011o", folder ? 0 : 1);
        snprintf(p + 136, 12, "%011o", 1700000000);
        p[156] = folder ? '5' : '0';
        memcpy(p + 257, "ustar", 6);
        memcpy(p + 263, "00", 2);
        memset(p + 148, ' ', 8);
        unsigned sum = 0;
        for (int j = 0; j < 512; j++) sum += (unsigned char)p[j];
        snprintf(p + 148, 7, "%06o", sum);
        p += 512;
        if (!folder) { p[0] = 'x'; p += 512; }
    }
    write_file(name, tar, (size_t)(p - tar) + 1024);
}

static SevenZipUpdateOptions options(const char *format)
{
    SevenZipUpdateOptions o;
    memset(&o, 0, sizeof o);
    o.format = format;
    o.level = 5;
    o.solid = -1;
    return o;
}

int main(void)
{
    snprintf(root, sizeof root, "%s/7zip-apple-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    printf("version %s, workspace %s\n", sevenzip_get_version(), root);
    CHECK(strstr(sevenzip_get_version(), "+MacRAR.Patch.") != NULL, "unexpected version string\n");

    // Sources: a text file, a binary file in a folder, and a Turkish file name.
    make_dir("src");
    make_dir("src/sub");
    write_file("src/a.txt", "hello archive\n", 14);
    size_t big = 1 << 20;
    char *noise = malloc(big);
    for (size_t i = 0; i < big; i++) noise[i] = (char)((i * 2654435761u) >> 13);
    write_file("src/sub/b.bin", noise, big);
    write_file("src/sub/çalışma ğüş.txt", "utf-8 name\n", 11);
    const char *sources[] = { strdup(path("src/a.txt")), strdup(path("src/sub")) };

    // 7z: create, list, extract, test
    {
        SevenZipUpdateOptions o = options("7z");
        o.solid = 1;
        int progress = 0;
        SevenZipCallbacks cb = { &progress, count_progress, NULL, NULL };
        EXPECT(sevenzip_update(path("plain.7z"), sources, 2, &o, &cb), SEVENZIP_OK);
        CHECK(progress > 0, "no progress reported\n");

        uint32_t count = 0;
        SevenZipListing *listing = NULL;
        EXPECT(listed("plain.7z", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 4, "7z lists %u items, expected 4\n", count);
        CHECK(strcmp(sevenzip_listing_type(listing), "7z") == 0, "type %s\n", sevenzip_listing_type(listing));
        CHECK(sevenzip_listing_is_solid(listing), "7z should be solid\n");
        CHECK(!sevenzip_listing_headers_encrypted(listing), "headers are not encrypted\n");
        CHECK(has_item(listing, "sub/çalışma ğüş.txt"), "UTF-8 name missing\n");
        sevenzip_listing_free(listing);

        make_dir("out7z");
        EXPECT(sevenzip_extract(path("plain.7z"), path("out7z"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(same_file("src/a.txt", "out7z/a.txt"), "a.txt differs\n");
        CHECK(same_file("src/sub/b.bin", "out7z/sub/b.bin"), "b.bin differs\n");
        CHECK(same_file("src/sub/çalışma ğüş.txt", "out7z/sub/çalışma ğüş.txt"), "UTF-8 file differs\n");
        EXPECT(sevenzip_test(path("plain.7z"), NULL, NULL), SEVENZIP_OK);

        // Selected items only
        make_dir("outsel");
        const char *only[] = { "sub" };
        EXPECT(sevenzip_extract(path("plain.7z"), path("outsel"), only, 1, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("outsel/sub/b.bin") && !exists("outsel/a.txt"), "selected extraction took the wrong items\n");

        // Overwrite policies
        write_file("out7z/a.txt", "changed", 7);
        EXPECT(sevenzip_extract(path("plain.7z"), path("out7z"), NULL, 0, SEVENZIP_SKIP, NULL, NULL), SEVENZIP_OK);
        size_t n = 0;
        char *kept = read_file("out7z/a.txt", &n);
        CHECK(kept && n == 7, "skip mode overwrote a.txt\n");
        free(kept);
        EXPECT(sevenzip_extract(path("plain.7z"), path("out7z"), NULL, 0, SEVENZIP_RENAME, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("out7z/a_1.txt"), "rename mode did not create a_1.txt\n");
        EXPECT(sevenzip_extract(path("plain.7z"), path("out7z"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(same_file("src/a.txt", "out7z/a.txt"), "overwrite mode left a.txt changed\n");

        // Delete an entry
        const char *gone[] = { "a.txt" };
        EXPECT(sevenzip_delete(path("plain.7z"), gone, 1, NULL, false, NULL), SEVENZIP_OK);
        EXPECT(listed("plain.7z", NULL, &count, NULL), SEVENZIP_OK);
        CHECK(count == 3, "after delete 7z lists %u items, expected 3\n", count);

        // Add to the existing archive
        const char *more[] = { path("src/a.txt") };
        EXPECT(sevenzip_update(path("plain.7z"), more, 1, &o, NULL), SEVENZIP_OK);
        EXPECT(listed("plain.7z", NULL, &count, NULL), SEVENZIP_OK);
        CHECK(count == 4, "after add 7z lists %u items, expected 4\n", count);
    }

    // 7z with encrypted headers
    {
        SevenZipUpdateOptions o = options("7z");
        o.password = "sır-parola";
        o.encrypt_headers = true;
        EXPECT(sevenzip_update(path("secret.7z"), sources, 2, &o, NULL), SEVENZIP_OK);
        EXPECT(listed("secret.7z", NULL, NULL, NULL), SEVENZIP_ERR_PASSWORD_REQUIRED);
        EXPECT(listed("secret.7z", "wrong", NULL, NULL), SEVENZIP_ERR_WRONG_PASSWORD);
        SevenZipListing *listing = NULL;
        uint32_t count = 0;
        EXPECT(listed("secret.7z", "sır-parola", &count, &listing), SEVENZIP_OK);
        CHECK(count == 4, "secret.7z lists %u items\n", count);
        CHECK(sevenzip_listing_headers_encrypted(listing), "headers should be reported encrypted\n");
        sevenzip_listing_free(listing);
        make_dir("outsecret");
        EXPECT(sevenzip_extract(path("secret.7z"), path("outsecret"), NULL, 0, SEVENZIP_OVERWRITE, "wrong", NULL),
               SEVENZIP_ERR_WRONG_PASSWORD);
        EXPECT(sevenzip_extract(path("secret.7z"), path("outsecret"), NULL, 0, SEVENZIP_OVERWRITE, "sır-parola", NULL),
               SEVENZIP_OK);
        CHECK(same_file("src/sub/b.bin", "outsecret/sub/b.bin"), "decrypted b.bin differs\n");
        EXPECT(sevenzip_test(path("secret.7z"), "sır-parola", NULL), SEVENZIP_OK);
    }

    // zip with AES-256: names readable, data encrypted
    {
        SevenZipUpdateOptions o = options("zip");
        o.password = "zip-pass";
        o.zip_aes256 = true;
        EXPECT(sevenzip_update(path("secret.zip"), sources, 2, &o, NULL), SEVENZIP_OK);
        SevenZipListing *listing = NULL;
        EXPECT(listed("secret.zip", NULL, NULL, &listing), SEVENZIP_OK);
        SevenZipItem item;
        int encrypted = 0;
        for (uint32_t i = 0; sevenzip_listing_item(listing, i, &item); i++)
            if (!item.is_dir && item.encrypted) encrypted++;
        CHECK(encrypted == 3, "zip reports %d encrypted files, expected 3\n", encrypted);
        CHECK(strstr(item.method, "AES") != NULL, "zip method %s is not AES\n", item.method);
        sevenzip_listing_free(listing);
        make_dir("outzip");
        EXPECT(sevenzip_extract(path("secret.zip"), path("outzip"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL),
               SEVENZIP_ERR_PASSWORD_REQUIRED);
        EXPECT(sevenzip_extract(path("secret.zip"), path("outzip"), NULL, 0, SEVENZIP_OVERWRITE, "nope", NULL),
               SEVENZIP_ERR_WRONG_PASSWORD);
        EXPECT(sevenzip_extract(path("secret.zip"), path("outzip"), NULL, 0, SEVENZIP_OVERWRITE, "zip-pass", NULL),
               SEVENZIP_OK);
        CHECK(same_file("src/sub/b.bin", "outzip/sub/b.bin"), "zip b.bin differs\n");
    }

    // tar.gz the way MacRAR builds it: tar first, then gzip the tar
    {
        SevenZipUpdateOptions tar = options("tar");
        tar.level = -1;
        EXPECT(sevenzip_update(path("pack.tar"), sources, 2, &tar, NULL), SEVENZIP_OK);
        SevenZipUpdateOptions gz = options("gzip");
        const char *tarball[] = { path("pack.tar") };
        EXPECT(sevenzip_update(path("pack.tar.gz"), tarball, 1, &gz, NULL), SEVENZIP_OK);
        SevenZipListing *listing = NULL;
        uint32_t count = 0;
        EXPECT(listed("pack.tar.gz", NULL, &count, &listing), SEVENZIP_OK);
        printf("tar.gz opens as type=%s container=%s with %u items\n",
               sevenzip_listing_type(listing), sevenzip_listing_container(listing), count);
        CHECK(strcmp(sevenzip_listing_type(listing), "tar") == 0, "tar.gz inner type %s\n", sevenzip_listing_type(listing));
        CHECK(strcmp(sevenzip_listing_container(listing), "gzip") == 0, "tar.gz container %s\n",
              sevenzip_listing_container(listing));
        CHECK(count == 4, "tar.gz lists %u items, expected 4\n", count);
        sevenzip_listing_free(listing);
        make_dir("outtgz");
        EXPECT(sevenzip_extract(path("pack.tar.gz"), path("outtgz"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(same_file("src/sub/b.bin", "outtgz/sub/b.bin"), "tar.gz b.bin differs\n");
    }

    // Split volumes
    {
        SevenZipUpdateOptions o = options("7z");
        o.level = 0;
        o.volume_size = 300000;
        EXPECT(sevenzip_update(path("vol.7z"), sources, 2, &o, NULL), SEVENZIP_OK);
        CHECK(exists("vol.7z.001") && exists("vol.7z.004"), "volumes were not written\n");
        SevenZipListing *listing = NULL;
        EXPECT(listed("vol.7z.001", NULL, NULL, &listing), SEVENZIP_OK);
        CHECK(sevenzip_listing_volumes(listing) >= 4, "volume count %u\n", sevenzip_listing_volumes(listing));
        sevenzip_listing_free(listing);
        make_dir("outvol");
        EXPECT(sevenzip_extract(path("vol.7z.001"), path("outvol"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(same_file("src/sub/b.bin", "outvol/sub/b.bin"), "volume b.bin differs\n");
    }

    // zip from Turkish Windows: names in CP857 (OEM, FAT host) and CP1254 (ANSI, VFAT host), beside a UTF-8
    // name without the flag (Unix host)
    {
        const char *names[] = { "\x9Eirket Raporu.txt", "A\xA7ustos \x99zetleri/\x98\x87" "erik \x8D\x9F\x8Dk.txt",
                                "\xDEube.txt", "Unix \xC5\x9F" "ark\xC4\xB1.txt" };
        const unsigned hosts[] = { 0, 0, 14, 3 };
        write_windows_zip("windows.zip", names, hosts, 4);

        SevenZipListing *listing = NULL;
        EXPECT(listed("windows.zip", NULL, NULL, &listing), SEVENZIP_OK);
        CHECK(has_item(listing, "\xEE\xBE\x9Eirket Raporu.txt"), "by default a CP857 name is not read as 7zz reads it\n");
        CHECK(has_item(listing, "Unix şarkı.txt"), "a UTF-8 name without the flag is not UTF-8\n");
        sevenzip_listing_free(listing);

        sevenzip_set_code_pages(857, 1254);
        EXPECT(listed("windows.zip", NULL, NULL, &listing), SEVENZIP_OK);
        CHECK(has_item(listing, "Şirket Raporu.txt"), "the OEM name is not read in CP857\n");
        CHECK(has_item(listing, "Ağustos Özetleri/İçerik ışık.txt"), "the OEM path is not read in CP857\n");
        CHECK(has_item(listing, "Şube.txt"), "the ANSI name is not read in CP1254\n");
        CHECK(has_item(listing, "Unix şarkı.txt"), "a UTF-8 name without the flag is read in a code page\n");
        sevenzip_listing_free(listing);
        make_dir("outcp");
        EXPECT(sevenzip_extract(path("windows.zip"), path("outcp"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("outcp/Şirket Raporu.txt") && exists("outcp/Ağustos Özetleri/İçerik ışık.txt")
              && exists("outcp/Şube.txt") && exists("outcp/Unix şarkı.txt"), "extracted names are not the listed ones\n");
        make_dir("outcpsel");
        const char *only[] = { "Ağustos Özetleri" };
        EXPECT(sevenzip_extract(path("windows.zip"), path("outcpsel"), only, 1, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("outcpsel/Ağustos Özetleri/İçerik ışık.txt") && !exists("outcpsel/Şube.txt"),
              "selected extraction by a CP857 name took the wrong items\n");
        EXPECT(sevenzip_test(path("windows.zip"), NULL, NULL), SEVENZIP_OK);
        sevenzip_set_code_pages(0, 0);
    }

    // Names with "*" and "?" in them name one item each, never the items they would match as wildcards
    {
        make_dir("wild");
        write_file("wild/a*.txt", "1", 1);
        write_file("wild/a1.txt", "2", 1);
        write_file("wild/b?.txt", "3", 1);
        write_file("wild/bx.txt", "4", 1);
        SevenZipUpdateOptions o = options("zip");
        uint32_t count = 0;
        SevenZipListing *listing = NULL;
        const char *star[] = { path("wild/a*.txt") };
        EXPECT(sevenzip_update(path("wild.zip"), star, 1, &o, NULL), SEVENZIP_OK);
        EXPECT(listed("wild.zip", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 1 && has_item(listing, "a*.txt"), "adding a*.txt added %u items\n", count);
        sevenzip_listing_free(listing);
        const char *rest[] = { path("wild/a1.txt"), path("wild/b?.txt"), path("wild/bx.txt") };
        EXPECT(sevenzip_update(path("wild.zip"), rest, 3, &o, NULL), SEVENZIP_OK);

        make_dir("outwild");
        const char *one[] = { "a*.txt" };
        EXPECT(sevenzip_extract(path("wild.zip"), path("outwild"), one, 1, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("outwild/a*.txt") && !exists("outwild/a1.txt"), "extracting a*.txt took a1.txt too\n");

        const char *gone[] = { "a*.txt", "b?.txt" };
        EXPECT(sevenzip_delete(path("wild.zip"), gone, 2, NULL, false, NULL), SEVENZIP_OK);
        EXPECT(listed("wild.zip", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 2 && has_item(listing, "a1.txt") && has_item(listing, "bx.txt"),
              "deleting a*.txt and b?.txt left %u items\n", count);
        sevenzip_listing_free(listing);

        // Renamed by names with wildcard characters, old and new
        const char *wildNew[] = { "a1.txt", "x?.txt" };
        EXPECT(sevenzip_rename(path("wild.zip"), wildNew, 1, NULL, NULL), SEVENZIP_OK);
        const char *wildOld[] = { "x?.txt", "y.txt" };
        EXPECT(sevenzip_rename(path("wild.zip"), wildOld, 1, NULL, NULL), SEVENZIP_OK);
        EXPECT(listed("wild.zip", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 2 && has_item(listing, "y.txt") && has_item(listing, "bx.txt"), "renaming by wildcard names\n");
        sevenzip_listing_free(listing);

        // Two names swapped at once: each new name is free once its item moves away
        const char *swap[] = { "y.txt", "bx.txt", "bx.txt", "y.txt" };
        EXPECT(sevenzip_rename(path("wild.zip"), swap, 2, NULL, NULL), SEVENZIP_OK);
        make_dir("outswap");
        EXPECT(sevenzip_extract(path("wild.zip"), path("outswap"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        size_t n = 0;
        char *swapped = read_file("outswap/y.txt", &n);
        CHECK(swapped && n == 1 && swapped[0] == '4', "the names were not swapped\n");
        free(swapped);
        const char *empty[] = { "y.txt", "" };
        EXPECT(sevenzip_rename(path("wild.zip"), empty, 1, NULL, NULL), SEVENZIP_ERR_INVALID_PARAM);
    }

    // Rename: an item into a folder, a folder with everything under it, and what is refused without writing
    {
        SevenZipUpdateOptions o = options("7z");
        o.solid = 1;
        EXPECT(sevenzip_update(path("ren.7z"), sources, 2, &o, NULL), SEVENZIP_OK);
        int progress = 0;
        SevenZipCallbacks cb = { &progress, count_progress, NULL, NULL };
        const char *into[] = { "a.txt", "sub/a.txt" };
        EXPECT(sevenzip_rename(path("ren.7z"), into, 1, NULL, &cb), SEVENZIP_OK);
        CHECK(progress > 0, "no progress reported while renaming\n");
        const char *folder[] = { "sub", "docs/sub" };
        EXPECT(sevenzip_rename(path("ren.7z"), folder, 1, NULL, NULL), SEVENZIP_OK);
        uint32_t count = 0;
        SevenZipListing *listing = NULL;
        EXPECT(listed("ren.7z", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 4 && has_item(listing, "docs/sub") && has_item(listing, "docs/sub/a.txt")
              && has_item(listing, "docs/sub/b.bin") && has_item(listing, "docs/sub/çalışma ğüş.txt")
              && !has_item(listing, "a.txt") && !has_item(listing, "sub/b.bin"), "renamed items are not where asked\n");
        sevenzip_listing_free(listing);
        make_dir("outren");
        EXPECT(sevenzip_extract(path("ren.7z"), path("outren"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(same_file("src/sub/b.bin", "outren/docs/sub/b.bin") && same_file("src/a.txt", "outren/docs/sub/a.txt"),
              "renamed items lost their data\n");

        struct stat before, after;
        stat(path("ren.7z"), &before);
        const char *missing[] = { "nothing.txt", "x.txt" };
        EXPECT(sevenzip_rename(path("ren.7z"), missing, 1, NULL, NULL), SEVENZIP_ERR_NOT_FOUND);
        const char *taken[] = { "docs/sub/a.txt", "docs/sub/b.bin" };
        EXPECT(sevenzip_rename(path("ren.7z"), taken, 1, NULL, NULL), SEVENZIP_ERR_EXISTS);
        const char *inside[] = { "docs", "docs/sub/docs" };
        EXPECT(sevenzip_rename(path("ren.7z"), inside, 1, NULL, NULL), SEVENZIP_ERR_INVALID_PARAM);
        const char *twice[] = { "docs/sub/a.txt", "c.txt", "docs/sub/b.bin", "c.txt" };
        EXPECT(sevenzip_rename(path("ren.7z"), twice, 2, NULL, NULL), SEVENZIP_ERR_INVALID_PARAM);
        const char *throughFile[] = { "docs/sub/a.txt", "docs/sub/b.bin/a.txt" };
        EXPECT(sevenzip_rename(path("ren.7z"), throughFile, 1, NULL, NULL), SEVENZIP_ERR_EXISTS);
        const char *shapes[][2] = { { "docs/", "papers" }, { "docs", "papers/" }, { "docs", "/papers" },
                                    { "docs", "../papers" }, { "docs", "a/./b" }, { "docs", "a//b" } };
        for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
            EXPECT(sevenzip_rename(path("ren.7z"), shapes[i], 1, NULL, NULL), SEVENZIP_ERR_INVALID_PARAM);
        SevenZipCallbacks cancel = { NULL, cancel_at_once, NULL, NULL };
        const char *cancelled[] = { "docs", "papers" };
        EXPECT(sevenzip_rename(path("ren.7z"), cancelled, 1, NULL, &cancel), SEVENZIP_ERR_CANCELLED);
        stat(path("ren.7z"), &after);
        CHECK(before.st_ino == after.st_ino && before.st_size == after.st_size, "a refused rename wrote the archive\n");

        // An item two pairs name: the folder and an item inside it
        const char *both[] = { "docs", "papers", "docs/sub/a.txt", "papers/a.txt" };
        EXPECT(sevenzip_rename(path("ren.7z"), both, 2, NULL, NULL), SEVENZIP_ERR_INVALID_PARAM);
    }

    // A compressed tar and an archive in volumes are not renamed in
    {
        const char *into[] = { "a.txt", "sub/a.txt" };
        EXPECT(sevenzip_rename(path("pack.tar.gz"), into, 1, NULL, NULL), SEVENZIP_ERR_UNSUPPORTED);
        EXPECT(sevenzip_rename(path("vol.7z.001"), into, 1, NULL, NULL), SEVENZIP_ERR_UNSUPPORTED);
    }

    // Rename in a 7z archive whose names are encrypted: only with its password
    {
        const char *into[] = { "a.txt", "sub/a.txt" };
        EXPECT(sevenzip_rename(path("secret.7z"), into, 1, NULL, NULL), SEVENZIP_ERR_PASSWORD_REQUIRED);
        EXPECT(sevenzip_rename(path("secret.7z"), into, 1, "wrong", NULL), SEVENZIP_ERR_WRONG_PASSWORD);
        EXPECT(sevenzip_rename(path("secret.7z"), into, 1, "sır-parola", NULL), SEVENZIP_OK);
        SevenZipListing *listing = NULL;
        EXPECT(listed("secret.7z", NULL, NULL, NULL), SEVENZIP_ERR_PASSWORD_REQUIRED);
        EXPECT(listed("secret.7z", "sır-parola", NULL, &listing), SEVENZIP_OK);
        CHECK(has_item(listing, "sub/a.txt") && sevenzip_listing_headers_encrypted(listing),
              "the renamed archive lost its encrypted names\n");
        sevenzip_listing_free(listing);
    }

    // Names and paths as they are written
    {
        // An item's whole path: "./a.txt" is not "a.txt", "." is the folder "." alone, ".." names nothing
        const char *entries[] = { "./", "./a.txt", "./sub/", "./sub/b.txt", "a.txt", "/abs.txt", "abs.txt" };
        write_tar("dots.tar", entries, 7);
        uint32_t count = 0;
        SevenZipListing *listing = NULL;
        EXPECT(listed("dots.tar", NULL, &count, NULL), SEVENZIP_OK);
        CHECK(count == 7, "dots.tar lists %u items\n", count);
        const char *dotted[] = { "./a.txt" };
        EXPECT(sevenzip_delete(path("dots.tar"), dotted, 1, NULL, false, NULL), SEVENZIP_OK);
        EXPECT(listed("dots.tar", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 6 && has_item(listing, "a.txt") && !has_item(listing, "./a.txt"),
              "deleting ./a.txt left %u items, a.txt %d\n", count, has_item(listing, "a.txt"));
        sevenzip_listing_free(listing);
        const char *up[] = { ".." };
        EXPECT(sevenzip_delete(path("dots.tar"), up, 1, NULL, false, NULL), SEVENZIP_OK);
        EXPECT(listed("dots.tar", NULL, &count, NULL), SEVENZIP_OK);
        CHECK(count == 6, "deleting .. left %u items\n", count);
        const char *absolute[] = { "/abs.txt" };
        EXPECT(sevenzip_delete(path("dots.tar"), absolute, 1, NULL, false, NULL), SEVENZIP_OK);
        EXPECT(listed("dots.tar", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 5 && has_item(listing, "abs.txt"), "deleting /abs.txt left %u items\n", count);
        sevenzip_listing_free(listing);
        const char *dot[] = { "." };
        EXPECT(sevenzip_delete(path("dots.tar"), dot, 1, NULL, false, NULL), SEVENZIP_OK);
        EXPECT(listed("dots.tar", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 2 && has_item(listing, "a.txt") && has_item(listing, "abs.txt"),
              "deleting . left %u items\n", count);
        sevenzip_listing_free(listing);

        // A folder's name still deletes everything under it
        SevenZipUpdateOptions z = options("zip");
        EXPECT(sevenzip_update(path("folder.zip"), sources, 2, &z, NULL), SEVENZIP_OK);
        const char *folder[] = { "sub" };
        EXPECT(sevenzip_delete(path("folder.zip"), folder, 1, NULL, false, NULL), SEVENZIP_OK);
        EXPECT(listed("folder.zip", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 1 && has_item(listing, "a.txt"), "deleting sub left %u items\n", count);
        sevenzip_listing_free(listing);

        // A "*" in the destination stays
        make_dir("dst*star");
        EXPECT(sevenzip_extract(path("folder.zip"), path("dst*star"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("dst*star/a.txt"), "the destination's * was replaced\n");

        // An archive without an extension is the file named, never one with an extension added
        EXPECT(sevenzip_update(path("noext"), sources, 2, &z, NULL), SEVENZIP_OK);
        CHECK(exists("noext") && !exists("noext.zip"), "the archive was written under another name\n");
        const char *one[] = { "a.txt" };
        EXPECT(sevenzip_delete(path("noext"), one, 1, NULL, false, NULL), SEVENZIP_OK);
        const char *move[] = { "sub", "moved" };
        EXPECT(sevenzip_rename(path("noext"), move, 1, NULL, NULL), SEVENZIP_OK);
        EXPECT(listed("noext", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 3 && has_item(listing, "moved/b.bin") && !has_item(listing, "a.txt"),
              "noext was not the archive changed: %u items\n", count);
        sevenzip_listing_free(listing);
        CHECK(!exists("noext.zip") && !exists("noext.7z"), "a change wrote another archive\n");

        // A missing name is refused, never read as "everything"
        const char *missing[] = { NULL };
        EXPECT(sevenzip_delete(path("folder.zip"), missing, 1, NULL, false, NULL), SEVENZIP_ERR_INVALID_PARAM);
        EXPECT(listed("folder.zip", NULL, &count, NULL), SEVENZIP_OK);
        CHECK(count == 1, "a missing name deleted items\n");
        EXPECT(sevenzip_update(path("folder.zip"), missing, 1, &z, NULL), SEVENZIP_ERR_INVALID_PARAM);
        make_dir("outmissing");
        EXPECT(sevenzip_extract(path("folder.zip"), path("outmissing"), missing, 1, SEVENZIP_OVERWRITE, NULL, NULL),
               SEVENZIP_ERR_INVALID_PARAM);
        CHECK(!exists("outmissing/a.txt"), "a missing name extracted everything\n");
    }

    // Failures
    EXPECT(listed("src/a.txt", NULL, NULL, NULL), SEVENZIP_ERR_NOT_ARCHIVE);
    {
        SevenZipCallbacks cancel = { NULL, cancel_at_once, NULL, NULL };
        make_dir("outcancel");
        EXPECT(sevenzip_extract(path("plain.7z"), path("outcancel"), NULL, 0, SEVENZIP_OVERWRITE, NULL, &cancel),
               SEVENZIP_ERR_CANCELLED);
    }
    EXPECT(sevenzip_extract(NULL, NULL, NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_ERR_INVALID_PARAM);

    free(noise);
    if (failures == 0)
        printf("all checks passed\n");
    else
        printf("%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
