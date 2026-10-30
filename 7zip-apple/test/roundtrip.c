// Round-trip checks for lib7zip_apple.a: every call of its C interface,
// against archives this test creates itself. build.sh gives it the version
// VERSION holds as EXPECTED_VERSION.
//
//   clang -DEXPECTED_VERSION='"26.05+PhantomArchiver.Patch.1"' \
//         -I7zip-apple/include 7zip-apple/test/roundtrip.c \
//         7zip-apple/build/<arch>/lib7zip_apple.a -lc++ -framework CoreFoundation \
//         -o roundtrip && ./roundtrip

#include "7zip_apple.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
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
/* A gzip stream of the file `name` that names what it packs `inner` (FNAME, RFC 1952), stored uncompressed. */
static void write_gzip_named(const char *gz, const char *name, const char *inner)
{
    size_t size = 0;
    char *data = read_file(name, &size);
    static unsigned char out[70000];
    unsigned char *p = out;
    const unsigned char head[] = { 0x1f, 0x8b, 8, 0x08, 0, 0, 0, 0, 0, 3 };
    memcpy(p, head, sizeof head); p += sizeof head;
    memcpy(p, inner, strlen(inner) + 1); p += strlen(inner) + 1;
    *p++ = 1;
    put16(&p, size); put16(&p, ~size & 0xFFFF);
    memcpy(p, data, size); p += size;
    put32(&p, crc32_of(data, size)); put32(&p, size);
    write_file(gz, (const char *)out, (size_t)(p - out));
    free(data);
}


/* RAR5's variable-length integer: seven bits a byte, the high bit set on all but the last. */
static void put_vint(unsigned char **p, unsigned long long v)
{
    do {
        unsigned char b = v & 0x7F;
        v >>= 7;
        *(*p)++ = b | (v ? 0x80 : 0);
    } while (v);
}

/* One RAR5 header block: the CRC32 of its size and fields, the size, the fields. */
static void put_rar5_block(unsigned char **p, const unsigned char *fields, size_t size)
{
    unsigned char sized[300];
    unsigned char *q = sized;
    put_vint(&q, size);
    memcpy(q, fields, size); q += size;
    put32(p, crc32_of((const char *)sized, (size_t)(q - sized)));
    memcpy(*p, sized, (size_t)(q - sized)); *p += q - sized;
}

/* A RAR5 archive of one file stored as it is, under `name`, whose header asks for a dictionary of
   (32 << (12 + dictionary)) bytes: algorithm version 1 (RAR 7), so dictionary 16 asks for 8 GB, past 7-Zip's
   default limit of 4 GB, and 0 for 128 KB. */
static void write_rar5_stored(const char *archive, const char *name, const char *data, unsigned dictionary)
{
    static unsigned char out[1024];
    unsigned char *p = out;
    const unsigned char signature[] = { 'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00 };
    memcpy(p, signature, sizeof signature); p += sizeof signature;
    unsigned char fields[300], *f = fields;
    put_vint(&f, 1); put_vint(&f, 0); put_vint(&f, 0);                    // main header: type, flags, archive flags
    put_rar5_block(&p, fields, (size_t)(f - fields));
    const size_t size = strlen(data);
    f = fields;
    put_vint(&f, 2); put_vint(&f, 0x0002); put_vint(&f, size);            // file header with a data area of `size`
    put_vint(&f, 0x0004); put_vint(&f, size); put_vint(&f, 0100644);      // data CRC32 present, unpacked size, mode
    put32(&f, crc32_of(data, size));
    put_vint(&f, 1 | (dictionary << 10));                                // algorithm 1, method 0 (store), dictionary
    put_vint(&f, 1); put_vint(&f, strlen(name));                         // host Unix, the name
    memcpy(f, name, strlen(name)); f += strlen(name);
    put_rar5_block(&p, fields, (size_t)(f - fields));
    memcpy(p, data, size); p += size;
    f = fields;
    put_vint(&f, 5); put_vint(&f, 0); put_vint(&f, 0);                    // end of archive
    put_rar5_block(&p, fields, (size_t)(f - fields));
    write_file(archive, (const char *)out, (size_t)(p - out));
}

/* The bytes a compressed tar has unpacked to so far, in its scratch folders in TMPDIR. */
static uint64_t scratch_bytes(void)
{
    uint64_t bytes = 0;
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    DIR *d = opendir(tmp);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (strncmp(e->d_name, "7zip-apple-", 11) != 0 || strncmp(e->d_name, "7zip-apple-test-", 16) == 0) continue;
        char folder[1024];
        snprintf(folder, sizeof folder, "%s/%s", tmp, e->d_name);
        DIR *inner = opendir(folder);
        if (!inner) continue;
        for (struct dirent *f; (f = readdir(inner)); ) {
            char file[2048];
            struct stat st;
            snprintf(file, sizeof file, "%s/%s", folder, f->d_name);
            if (lstat(file, &st) == 0 && S_ISREG(st.st_mode)) bytes += (uint64_t)st.st_size;
        }
        closedir(inner);
    }
    closedir(d);
    return bytes;
}

/* The most a compressed tar's scratch folder held at any progress report. */
static bool record_furthest(void *context, uint64_t completed, uint64_t total)
{
    (void)completed; (void)total;
    uint64_t *most = context;
    const uint64_t now = scratch_bytes();
    if (now > *most) *most = now;
    return false;
}

/* The scratch folders a compressed tar unpacked into and left in TMPDIR. */
static int scratch_left(void)
{
    int left = 0;
    DIR *d = opendir(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)); )
        if (strncmp(e->d_name, "7zip-apple-", 11) == 0 && strncmp(e->d_name, "7zip-apple-test-", 16) != 0)
            left++;
    closedir(d);
    return left;
}

static int remove_entry(const char *entry, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    chmod(entry, 0755);
    return remove(entry);
}

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

static SevenZipCreateOptions options(const char *format)
{
    SevenZipCreateOptions o;
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
    CHECK(strcmp(sevenzip_get_version(), EXPECTED_VERSION) == 0, "the library reports %s, VERSION holds %s\n",
          sevenzip_get_version(), EXPECTED_VERSION);

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
        SevenZipCreateOptions o = options("7z");
        o.solid = 1;
        int progress = 0;
        SevenZipCallbacks cb = { &progress, count_progress, NULL, NULL };
        EXPECT(sevenzip_create(path("plain.7z"), sources, 2, &o, &cb), SEVENZIP_OK);
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

        // An archive already at the path is left as it is, and so is the first volume of a split one
        write_file("extra.txt", "extra", 5);
        const char *more[] = { path("extra.txt") };
        EXPECT(sevenzip_create(path("plain.7z"), more, 1, &o, NULL), SEVENZIP_ERR_EXISTS);
        EXPECT(listed("plain.7z", NULL, &count, NULL), SEVENZIP_OK);
        CHECK(count == 4, "creating over plain.7z changed it: %u items\n", count);
        SevenZipCreateOptions split = o;
        split.volume_size = 1 << 20;
        write_file("taken.7z.001", "x", 1);
        EXPECT(sevenzip_create(path("taken.7z"), more, 1, &split, NULL), SEVENZIP_ERR_EXISTS);
        n = 0;
        char *first = read_file("taken.7z.001", &n);
        CHECK(first && n == 1 && !exists("taken.7z.002"), "creating a split archive wrote over taken.7z.001\n");
        free(first);
    }

    // 7z with encrypted headers
    {
        SevenZipCreateOptions o = options("7z");
        o.password = "sır-parola";
        o.encrypt_headers = true;
        EXPECT(sevenzip_create(path("secret.7z"), sources, 2, &o, NULL), SEVENZIP_OK);
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
        SevenZipCreateOptions o = options("zip");
        o.password = "zip-pass";
        o.zip_aes256 = true;
        EXPECT(sevenzip_create(path("secret.zip"), sources, 2, &o, NULL), SEVENZIP_OK);
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

    // tar.gz the way an app builds it: tar first, then gzip the tar
    {
        SevenZipCreateOptions tar = options("tar");
        tar.level = -1;
        EXPECT(sevenzip_create(path("pack.tar"), sources, 2, &tar, NULL), SEVENZIP_OK);
        SevenZipCreateOptions gz = options("gzip");
        const char *tarball[] = { path("pack.tar") };
        EXPECT(sevenzip_create(path("pack.tar.gz"), tarball, 1, &gz, NULL), SEVENZIP_OK);
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
        SevenZipCreateOptions o = options("7z");
        o.level = 0;
        o.volume_size = 300000;
        EXPECT(sevenzip_create(path("vol.7z"), sources, 2, &o, NULL), SEVENZIP_OK);
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
        SevenZipCreateOptions o = options("zip");
        uint32_t count = 0;
        SevenZipListing *listing = NULL;
        const char *star[] = { path("wild/a*.txt") };
        EXPECT(sevenzip_create(path("star.zip"), star, 1, &o, NULL), SEVENZIP_OK);
        EXPECT(listed("star.zip", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 1 && has_item(listing, "a*.txt"), "adding a*.txt added %u items\n", count);
        sevenzip_listing_free(listing);
        const char *all[] = { path("wild/a*.txt"), path("wild/a1.txt"), path("wild/b?.txt"), path("wild/bx.txt") };
        EXPECT(sevenzip_create(path("wild.zip"), all, 4, &o, NULL), SEVENZIP_OK);

        make_dir("outwild");
        const char *one[] = { "a*.txt" };
        EXPECT(sevenzip_extract(path("wild.zip"), path("outwild"), one, 1, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("outwild/a*.txt") && !exists("outwild/a1.txt"), "extracting a*.txt took a1.txt too\n");
    }

    // Names and paths as they are written
    {
        // An item's whole path: "./a.txt" is not "a.txt", and each is listed as written
        const char *entries[] = { "./", "./a.txt", "./sub/", "./sub/b.txt", "a.txt", "/abs.txt", "abs.txt" };
        write_tar("dots.tar", entries, 7);
        uint32_t count = 0;
        SevenZipListing *listing = NULL;
        EXPECT(listed("dots.tar", NULL, &count, &listing), SEVENZIP_OK);
        CHECK(count == 7 && has_item(listing, "./a.txt") && has_item(listing, "a.txt"), "dots.tar lists %u items\n", count);
        sevenzip_listing_free(listing);

        SevenZipCreateOptions z = options("zip");
        EXPECT(sevenzip_create(path("folder.zip"), sources, 2, &z, NULL), SEVENZIP_OK);

        // A "*" in the destination stays
        make_dir("dst*star");
        EXPECT(sevenzip_extract(path("folder.zip"), path("dst*star"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        CHECK(exists("dst*star/a.txt"), "the destination's * was replaced\n");

        // An archive without an extension is the file named, never one with an extension added
        EXPECT(sevenzip_create(path("noext"), sources, 2, &z, NULL), SEVENZIP_OK);
        CHECK(exists("noext") && !exists("noext.zip"), "the archive was written under another name\n");

        // A missing name is refused, never read as "everything"
        const char *missing[] = { NULL };
        EXPECT(sevenzip_create(path("missing.zip"), missing, 1, &z, NULL), SEVENZIP_ERR_INVALID_PARAM);
        CHECK(!exists("missing.zip"), "a missing name wrote an archive\n");
        make_dir("outmissing");
        EXPECT(sevenzip_extract(path("folder.zip"), path("outmissing"), missing, 1, SEVENZIP_OVERWRITE, NULL, NULL),
               SEVENZIP_ERR_INVALID_PARAM);
        CHECK(!exists("outmissing/a.txt"), "a missing name extracted everything\n");
    }

    // A compressed tar is the tar it unpacks to, never the name its stream gives: "../trap.tar" opened the tar of
    // that name beside the scratch folder
    {
        const char *trapped[] = { "trap-entry.txt" };
        write_tar("trap.tar", trapped, 1);
        char trap[1024];
        snprintf(trap, sizeof trap, "%s/trap.tar", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        rename(path("trap.tar"), trap);
        const char *inner[] = { "inner.txt" };
        write_tar("inner.tar", inner, 1);
        write_gzip_named("named.tar.gz", "inner.tar", "../trap.tar");
        SevenZipListing *listing = NULL;
        EXPECT(listed("named.tar.gz", NULL, NULL, &listing), SEVENZIP_OK);
        CHECK(has_item(listing, "inner.txt") && !has_item(listing, "trap-entry.txt"), "the stream's name opened another tar\n");
        sevenzip_listing_free(listing);
        unlink(trap);
    }

    // A compressed tar unpacks to at most the scratch limit: past it the call stops with its own error and leaves no
    // scratch folder; 0 brings back the default, the free space less a margin
    {
        sevenzip_set_scratch_limit(64 * 1024);
        EXPECT(listed("pack.tar.gz", NULL, NULL, NULL), SEVENZIP_ERR_NO_SPACE);
        make_dir("outlimit");
        EXPECT(sevenzip_extract(path("pack.tar.gz"), path("outlimit"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL),
               SEVENZIP_ERR_NO_SPACE);
        CHECK(scratch_left() == 0, "a scratch folder was left\n");

        // A bomb is stopped while it unpacks, not once it has: 256 MiB of zeros in a gzip of a few hundred KiB
        make_dir("bomb");
        int fd = open(path("bomb/zeros.bin"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0 && ftruncate(fd, 256 << 20) == 0, "bomb/zeros.bin was not made\n");
        close(fd);
        char *zerosPath = strdup(path("bomb/zeros.bin"));
        const char *zeros[] = { zerosPath };
        SevenZipCreateOptions tar = options("tar");
        tar.level = -1;
        EXPECT(sevenzip_create(path("bomb.tar"), zeros, 1, &tar, NULL), SEVENZIP_OK);
        free(zerosPath);
        unlink(path("bomb/zeros.bin"));
        SevenZipCreateOptions gz = options("gzip");
        gz.level = 1;
        const char *bombTar[] = { path("bomb.tar") };
        EXPECT(sevenzip_create(path("bomb.tar.gz"), bombTar, 1, &gz, NULL), SEVENZIP_OK);
        unlink(path("bomb.tar"));
        sevenzip_set_scratch_limit(4 << 20);
        uint64_t most = 0;
        SevenZipCallbacks watch = { &most, record_furthest, NULL, NULL };
        EXPECT(sevenzip_test(path("bomb.tar.gz"), NULL, &watch), SEVENZIP_ERR_NO_SPACE);
        printf("the bomb stopped at %.1f MiB of 256\n", (double)most / 1048576.0);
        CHECK(most > 0 && most < (32u << 20), "the bomb unpacked to %llu bytes before it was stopped\n", (unsigned long long)most);
        CHECK(scratch_left() == 0, "the bomb's scratch folder was left\n");
        sevenzip_set_scratch_limit(0);
        EXPECT(listed("pack.tar.gz", NULL, NULL, NULL), SEVENZIP_OK);
    }

    // An item whose dictionary is past the default memory limit (4 GB) is not extracted: 7-Zip asks, and the answer is
    // no, as 7zz's. The same archive with a dictionary of 128 KB extracts.
    {
        write_rar5_stored("smalldict.rar", "small.txt", "stored data", 0);
        make_dir("outsmall");
        EXPECT(sevenzip_extract(path("smalldict.rar"), path("outsmall"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL), SEVENZIP_OK);
        size_t n = 0;
        char *small = read_file("outsmall/small.txt", &n);
        CHECK(small && n == 11 && memcmp(small, "stored data", 11) == 0, "the stored item did not extract as written\n");
        free(small);
        write_rar5_stored("bigdict.rar", "big.txt", "stored data", 16);
        make_dir("outbig");
        CHECK(sevenzip_extract(path("bigdict.rar"), path("outbig"), NULL, 0, SEVENZIP_OVERWRITE, NULL, NULL) != SEVENZIP_OK,
              "an archive past the memory limit extracted without a word\n");
        CHECK(!exists("outbig/big.txt"), "an item past the memory limit was extracted\n");
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
    // The workspace goes, as the scratch folders do: test after test it was left in TMPDIR
    nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    if (failures == 0)
        printf("all checks passed\n");
    else
        printf("%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
