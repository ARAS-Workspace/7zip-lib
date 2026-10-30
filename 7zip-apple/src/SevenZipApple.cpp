#include "Callbacks.h"

#include "../../CPP/Common/MyException.h"
#include "../../CPP/Common/StringConvert.h"
#include "../../CPP/Common/UTFConvert.h"
#include "../../CPP/Windows/ErrorMsg.h"
#include "../../CPP/Windows/PropVariant.h"

#include "../../CPP/7zip/PropID.h"
#include "../../CPP/7zip/UI/Common/ArchiveCommandLine.h"
#include "../../CPP/7zip/UI/Common/Extract.h"
#include "../../CPP/7zip/UI/Common/HashCalc.h"
#include "../../CPP/7zip/UI/Common/LoadCodecs.h"
#include "../../CPP/7zip/UI/Common/OpenArchive.h"
#include "../../CPP/7zip/UI/Common/Update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ftw.h>
#include <new>
#include <unistd.h>
#include <vector>

static thread_local std::string g_lastError;

static UString Wide(const char *utf8)
{
    UString s;
    if (utf8)
        ConvertUTF8ToUnicode(AString(utf8), s);
    return s;
}

static int32_t Fail(int32_t code, const std::string &message)
{
    g_lastError = message;
    return code;
}

// Runs one 7zz-equivalent call: everything the C side can be handed back
// from, so that no C++ exception crosses the C boundary.
template <typename Body>
static int32_t Guarded(Body body)
{
    g_lastError.clear();
    try
    {
        return body();
    }
    catch (const CArcCmdLineException &e)
    {
        return Fail(SEVENZIP_ERR_INVALID_PARAM, Utf8(e));
    }
    catch (const CSystemException &e)
    {
        return Fail(e.ErrorCode == E_OUTOFMEMORY ? SEVENZIP_ERR_NO_MEMORY : SEVENZIP_ERR_FAILED,
                    Utf8(NWindows::NError::MyFormatMessage(e.ErrorCode)));
    }
    catch (const std::bad_alloc &)
    {
        return Fail(SEVENZIP_ERR_NO_MEMORY, "Can't allocate required memory");
    }
    catch (const UString &s)
    {
        return Fail(SEVENZIP_ERR_FAILED, Utf8(s));
    }
    catch (const AString &s)
    {
        return Fail(SEVENZIP_ERR_FAILED, std::string(s.Ptr()));
    }
    catch (const char *s)
    {
        return Fail(SEVENZIP_ERR_FAILED, s ? s : "");
    }
    catch (...)
    {
        return Fail(SEVENZIP_ERR_FAILED, "Unknown error");
    }
}

// The order mirrors 7zz's own exit-code decisions: an archive that could
// not be opened outranks per-item errors, which outrank warnings.
static int32_t Resolve(HRESULT hr, const COutcome &outcome, bool passwordGiven, const std::string &fallback)
{
    const std::string &message = outcome.FirstError.empty() ? fallback : outcome.FirstError;
    if (outcome.Cancelled)
        return Fail(SEVENZIP_ERR_CANCELLED, "Cancelled");
    if (outcome.PasswordRequested && !passwordGiven)
        return Fail(SEVENZIP_ERR_PASSWORD_REQUIRED, "A password is required");
    if (outcome.WrongPassword)
        return Fail(SEVENZIP_ERR_WRONG_PASSWORD, message.empty() ? "Wrong password" : message);
    if (outcome.NotArchive)
        return Fail(SEVENZIP_ERR_NOT_ARCHIVE, message);
    if (hr == E_OUTOFMEMORY)
        return Fail(SEVENZIP_ERR_NO_MEMORY, "Can't allocate required memory");
    if (hr == E_NOTIMPL || outcome.UnsupportedMethod)
        return Fail(SEVENZIP_ERR_UNSUPPORTED, message.empty() ? "Unsupported" : message);
    if (outcome.DataError)
        return Fail(SEVENZIP_ERR_DATA, message);
    if (hr != S_OK)
        return Fail(SEVENZIP_ERR_FAILED,
                    message.empty() ? Utf8(NWindows::NError::MyFormatMessage(hr)) : message);
    if (!outcome.FirstError.empty())
        return Fail(SEVENZIP_ERR_FAILED, message);
    return outcome.Warning ? SEVENZIP_WARNING : SEVENZIP_OK;
}

// The same steps 7zz's Main2() takes before running a command.
struct CSession
{
    CArcCmdLineOptions Options;
    CCodecs *Codecs;
    CMyComPtr<IUnknown> CodecsRef;
    CObjectVector<COpenType> Types;
    CIntVector ExcludedFormats;

    explicit CSession(const UStringVector &switches)
    {
        CArcCmdLineParser parser;
        parser.Parse1(switches, Options);
        parser.Parse2(Options);

        Codecs = new CCodecs;
        CodecsRef = Codecs;
        Codecs->CaseSensitive_Change = Options.CaseSensitive_Change;
        Codecs->CaseSensitive = Options.CaseSensitive;
        const HRESULT hr = Codecs->Load();
        if (hr != S_OK)
            throw CSystemException(hr);
        Codecs_AddHashArcHandler(Codecs);
        if (!ParseOpenTypes(*Codecs, Options.ArcType, Types))
            throw CArcCmdLineException("Unsupported archive type");
    }
};

static void AddPassword(UStringVector &switches, const char *password)
{
    if (password)
        switches.Add(UString(L"-p") + Wide(password));
}

// A missing name is refused: left out, a list of nothing but missing names became 7-Zip's "everything" ("*").
static void AddNames(UStringVector &switches, const char *archive, const char *const *names, size_t count)
{
    switches.Add(L"--");
    switches.Add(Wide(archive));
    for (size_t i = 0; i < count; i++)
    {
        if (!names[i])
            throw CArcCmdLineException("A name is missing");
        switches.Add(Wide(names[i]));
    }
}

static HRESULT ExtractWith(CReporter &reporter, bool test, const UString &archive, const UString &destination,
                           const char *const *items, size_t itemCount, int32_t overwrite, const char *password,
                           std::string &errorText)
{
    Int32 answer;
    const wchar_t *mode;
    switch (overwrite)
    {
        case SEVENZIP_OVERWRITE: mode = L"-aoa"; answer = NOverwriteAnswer::kYesToAll; break;
        case SEVENZIP_RENAME:    mode = L"-aou"; answer = NOverwriteAnswer::kAutoRename; break;
        case SEVENZIP_SKIP:      mode = L"-aos"; answer = NOverwriteAnswer::kNoToAll; break;
        default: throw CArcCmdLineException("Unknown overwrite mode");
    }

    UStringVector switches;
    switches.Add(test ? L"t" : L"x");
    switches.Add(L"-y");
    // Names are matched as they are written: "a*.txt" is that one item, not a wildcard
    switches.Add(L"-spd");
    if (!test)
    {
        // The destination is taken as written too: 7-Zip put the archive's name in place of a "*" in it
        switches.Add(L"-spod");
    }
    if (!test)
    {
        switches.Add(mode);
        switches.Add(UString(L"-o") + destination);
    }
    AddPassword(switches, password);
    switches.Add(L"--");
    switches.Add(archive);
    for (size_t i = 0; items && i < itemCount; i++)
    {
        if (!items[i])
            throw CArcCmdLineException("A name is missing");
        switches.Add(Wide(items[i]));
    }

    CSession session(switches);
    CArcCmdLineOptions &options = session.Options;

    CExtractCallback *callbackSpec = new CExtractCallback(reporter, password, answer);
    CMyComPtr<IFolderArchiveExtractCallback> callback = callbackSpec;

    CExtractOptions eo;
    (CExtractOptionsBase &)eo = options.ExtractOptions;
    eo.StdInMode = false;
    eo.StdOutMode = false;
    eo.YesToAll = true;
    eo.TestMode = test;
    eo.Properties = options.Properties;

    UStringVector paths, fullPaths;
    paths.Add(archive);
    fullPaths.Add(archive);

    UString errorMessage;
    CDecompressStat stat;
    const HRESULT hr = Extract(
        session.Codecs, session.Types, session.ExcludedFormats,
        paths, fullPaths,
        options.Censor.Pairs.Front().Head,
        eo, callbackSpec, callbackSpec, callbackSpec,
        NULL, errorMessage, stat);
    errorText = Utf8(errorMessage);
    return hr;
}

// ─── Compressed tar ─────────────────────────────────────────

// A compressed tar (tar.gz, tar.bz2, …) opens as a one-item stream whose item
// is the tar. 7-Zip only descends into it when that stream is seekable, so
// the tar is unpacked into a scratch directory first and read from there.
static const char *const kStreamFormats[] = { "gzip", "bzip2", "xz", "zstd", "lz4", "lzma", "lzma86", "Z", "lz" };

class CScratchDir
{
public:
    ~CScratchDir()
    {
        // Depth-first and without following links, so nothing outside the
        // directory is touched.
        if (!Path.IsEmpty())
            nftw(Utf8(Path).c_str(), [](const char *path, const struct stat *, int, struct FTW *) {
                return remove(path);
            }, 16, FTW_DEPTH | FTW_PHYS);
    }

    bool Create()
    {
        const char *tmp = getenv("TMPDIR");
        std::string pattern = (tmp && *tmp) ? tmp : "/tmp";
        if (pattern.back() != '/')
            pattern += '/';
        pattern += "7zip-apple-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (!mkdtemp(buffer.data()))
            return false;
        Path = Wide(buffer.data());
        return true;
    }

    UString Path;
};

struct CNestedTar
{
    UString Inner;       // the tar's name inside the stream
    std::string Outer;   // the stream's format name
};

static void FillOpenOptions(COpenOptions &op, CSession &session, CObjectVector<CProperty> &props, const UString &path)
{
    op.props = &props;
    op.codecs = session.Codecs;
    op.types = &session.Types;
    op.excludedFormats = &session.ExcludedFormats;
    op.stdInMode = false;
    op.stream = NULL;
    op.filePath = path;
}

static bool FindNestedTar(const UString &archive, CNestedTar &nested)
{
    UStringVector switches;
    switches.Add(L"l");
    switches.Add(L"--");
    switches.Add(archive);
    CSession session(switches);

    COutcome outcome;
    CReporter reporter(NULL, outcome);
    COpenCallback openCallback(reporter, NULL);
    CObjectVector<CProperty> props;
    COpenOptions op;
    FillOpenOptions(op, session, props, archive);

    CArchiveLink arcLink;
    if (arcLink.Open_Strict(op, &openCallback) != S_OK || arcLink.Arcs.Size() != 1)
        return false;
    const CArc &arc = arcLink.Arcs.Back();
    const std::string format = Utf8(session.Codecs->Formats[(unsigned)arc.FormatIndex].Name);
    if (std::find(std::begin(kStreamFormats), std::end(kStreamFormats), format) == std::end(kStreamFormats))
        return false;
    UInt32 numItems = 0;
    if (arc.Archive->GetNumberOfItems(&numItems) != S_OK || numItems != 1)
        return false;
    UString inner;
    if (arc.GetItem_Path2(0, inner) != S_OK)
        return false;
    std::string lower = Utf8(inner);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".tar") != 0)
        return false;
    nested.Inner = inner;
    nested.Outer = format;
    return true;
}

// Unpacks the tar of a compressed tar into scratch; on success tarPath names it.
static HRESULT UnpackNestedTar(CReporter &reporter, const UString &archive, const CNestedTar &nested,
                               CScratchDir &scratch, UString &tarPath, std::string &errorText)
{
    if (!scratch.Create())
        throw CSystemException(HRESULT_FROM_WIN32(errno));
    reporter.Stages = 2;
    reporter.Stage = 0;
    const HRESULT hr = ExtractWith(reporter, false, archive, scratch.Path, NULL, 0, SEVENZIP_OVERWRITE, NULL, errorText);
    tarPath = scratch.Path + L"/" + nested.Inner;
    reporter.Stage = 1;
    return hr;
}

static int32_t RunExtract(bool test, const char *archive, const char *destination,
                          const char *const *items, size_t itemCount, int32_t overwrite,
                          const char *password, const SevenZipCallbacks *callbacks)
{
    if (!archive || (!test && !destination))
        return Fail(SEVENZIP_ERR_INVALID_PARAM, "Archive and destination are required");

    COutcome outcome;
    CReporter reporter(callbacks, outcome);
    std::string errorText;
    UString source = Wide(archive);
    const UString target = destination ? Wide(destination) : UString();

    CScratchDir scratch;
    CNestedTar nested;
    if (FindNestedTar(source, nested))
    {
        const HRESULT hr = UnpackNestedTar(reporter, source, nested, scratch, source, errorText);
        if (hr != S_OK || !outcome.FirstError.empty() || outcome.Cancelled)
            return Resolve(hr, outcome, password != NULL, errorText);
    }
    const HRESULT hr = ExtractWith(reporter, test, source, target, items, itemCount, overwrite, password, errorText);
    return Resolve(hr, outcome, password != NULL, errorText);
}

static int32_t RunUpdate(UStringVector &switches, const char *password, const SevenZipCallbacks *callbacks)
{
    CSession session(switches);
    CArcCmdLineOptions &options = session.Options;

    COutcome outcome;
    CReporter reporter(callbacks, outcome);
    COpenCallback openCallback(reporter, password);
    CUpdateCallback callback(reporter, password);
    CUpdateErrorInfo errorInfo;

    const HRESULT hr = UpdateArchive(session.Codecs, session.Types, options.ArchiveName, options.Censor,
                                     options.UpdateOptions, errorInfo, &openCallback, &callback, true);

    if (errorInfo.ThereIsError() && outcome.FirstError.empty())
    {
        std::string message = std::string(errorInfo.Message.Ptr());
        FOR_VECTOR (i, errorInfo.FileNames)
            message += (message.empty() ? "" : " : ") + Utf8(fs2us(errorInfo.FileNames[i]));
        if (errorInfo.SystemError != 0)
            message += (message.empty() ? "" : " : ") + Utf8(NWindows::NError::MyFormatMessage(errorInfo.SystemError));
        outcome.FirstError = message;
    }
    return Resolve(hr, outcome, password != NULL, std::string());
}

// ─── Listing ────────────────────────────────────────────────

struct SevenZipListing
{
    struct Entry
    {
        std::string Path;
        std::string Method;
        SevenZipItem Item;
    };
    std::string Type;
    std::string Container;
    bool Solid = false;
    uint32_t Volumes = 0;
    bool HeadersEncrypted = false;
    std::vector<Entry> Entries;
};

static bool GetUInt64(IInArchive *archive, UInt32 index, PROPID id, UInt64 &value)
{
    NWindows::NCOM::CPropVariant prop;
    if (archive->GetProperty(index, id, &prop) != S_OK)
        return false;
    switch (prop.vt)
    {
        case VT_UI1: value = prop.bVal; return true;
        case VT_UI2: value = prop.uiVal; return true;
        case VT_UI4: value = prop.ulVal; return true;
        case VT_UI8: value = prop.uhVal.QuadPart; return true;
        default: return false;
    }
}

static int32_t ListPath(const UString &archive, const char *password, SevenZipListing *listing)
{
    UStringVector switches;
    switches.Add(L"l");
    AddPassword(switches, password);
    switches.Add(L"--");
    switches.Add(archive);
    CSession session(switches);

    COutcome outcome;
    CReporter reporter(NULL, outcome);
    COpenCallback openCallback(reporter, password);
    CObjectVector<CProperty> props;
    COpenOptions op;
    FillOpenOptions(op, session, props, archive);

    CArchiveLink arcLink;
    const HRESULT hr = arcLink.Open_Strict(op, &openCallback);
    if (hr != S_OK)
    {
        if (hr == S_FALSE)
        {
            if (arcLink.PasswordWasAsked)
            {
                if (password)
                    outcome.WrongPassword = true;
                outcome.FirstError = "Cannot open encrypted archive. Wrong password?";
            }
            else
            {
                outcome.NotArchive = true;
                outcome.FirstError = "Cannot open the file as archive";
            }
        }
        return Resolve(hr == S_FALSE ? S_OK : hr, outcome, password != NULL, std::string());
    }

    const CArc &arc = arcLink.Arcs.Back();
    listing->Type = Utf8(session.Codecs->Formats[(unsigned)arc.FormatIndex].Name);
    if (arcLink.Arcs.Size() > 1)
        listing->Container = Utf8(session.Codecs->Formats[(unsigned)arcLink.Arcs.Front().FormatIndex].Name);
    listing->HeadersEncrypted = arcLink.PasswordWasAsked;
    listing->Volumes = arcLink.VolumePaths.Size();

    FOR_VECTOR (level, arcLink.Arcs)
    {
        IInArchive *a = arcLink.Arcs[level].Archive;
        NWindows::NCOM::CPropVariant prop;
        if (a->GetArchiveProperty(kpidNumVolumes, &prop) == S_OK && prop.vt == VT_UI4)
            listing->Volumes = std::max<uint32_t>(listing->Volumes, prop.ulVal);
    }

    IInArchive *inArchive = arc.Archive;
    {
        NWindows::NCOM::CPropVariant prop;
        if (inArchive->GetArchiveProperty(kpidSolid, &prop) == S_OK && prop.vt == VT_BOOL)
            listing->Solid = VARIANT_BOOLToBool(prop.boolVal);
    }

    UInt32 numItems = 0;
    if (inArchive->GetNumberOfItems(&numItems) != S_OK)
        numItems = 0;
    listing->Entries.resize(numItems);
    for (UInt32 i = 0; i < numItems; i++)
    {
        SevenZipListing::Entry &entry = listing->Entries[i];
        SevenZipItem &item = entry.Item;
        item = SevenZipItem();

        UString path;
        if (arc.GetItem_Path2(i, path) == S_OK)
            entry.Path = Utf8(path);

        bool isDir = false;
        Archive_IsItem_Dir(inArchive, i, isDir);
        item.is_dir = isDir;

        bool encrypted = false;
        Archive_GetItemBoolProp(inArchive, i, kpidEncrypted, encrypted);
        item.encrypted = encrypted;

        UInt64 value = 0;
        if (GetUInt64(inArchive, i, kpidSize, value)) item.size = value;
        if (GetUInt64(inArchive, i, kpidPackSize, value)) item.packed_size = value;
        if (GetUInt64(inArchive, i, kpidCRC, value)) { item.crc = (uint32_t)value; item.has_crc = true; }
        if (GetUInt64(inArchive, i, kpidAttrib, value)) item.attributes = (uint32_t)value;

        {
            NWindows::NCOM::CPropVariant prop;
            if (inArchive->GetProperty(i, kpidMTime, &prop) == S_OK && prop.vt == VT_FILETIME)
            {
                const UInt64 ticks = ((UInt64)prop.filetime.dwHighDateTime << 32) | prop.filetime.dwLowDateTime;
                item.mtime = (int64_t)(ticks / 10000000) - (int64_t)11644473600LL;
            }
        }
        {
            NWindows::NCOM::CPropVariant prop;
            if (inArchive->GetProperty(i, kpidMethod, &prop) == S_OK && prop.vt == VT_BSTR)
                entry.Method = Utf8(prop.bstrVal);
        }
    }
    return SEVENZIP_OK;
}

static int32_t RunList(const char *archive, const char *password, SevenZipListing **result)
{
    if (!archive || !result)
        return Fail(SEVENZIP_ERR_INVALID_PARAM, "Archive and result are required");
    *result = NULL;

    SevenZipListing *listing = new SevenZipListing;
    UString source = Wide(archive);
    CScratchDir scratch;
    CNestedTar nested;
    int32_t rc = SEVENZIP_OK;
    if (FindNestedTar(source, nested))
    {
        COutcome outcome;
        CReporter reporter(NULL, outcome);
        std::string errorText;
        const HRESULT hr = UnpackNestedTar(reporter, source, nested, scratch, source, errorText);
        if (hr != S_OK || !outcome.FirstError.empty())
            rc = Resolve(hr, outcome, false, errorText);
    }
    if (rc == SEVENZIP_OK)
        rc = ListPath(source, password, listing);
    if (rc != SEVENZIP_OK && rc != SEVENZIP_WARNING)
    {
        delete listing;
        return rc;
    }
    if (!nested.Outer.empty())
        listing->Container = nested.Outer;
    for (SevenZipListing::Entry &entry : listing->Entries)
    {
        entry.Item.path = entry.Path.c_str();
        entry.Item.method = entry.Method.c_str();
    }
    *result = listing;
    return rc;
}

// ─── C API ──────────────────────────────────────────────────

int32_t sevenzip_list(const char *archive, const char *password, SevenZipListing **listing)
{
    return Guarded([&] { return RunList(archive, password, listing); });
}

void sevenzip_listing_free(SevenZipListing *listing)
{
    delete listing;
}

const char *sevenzip_listing_type(const SevenZipListing *listing)
{
    return listing ? listing->Type.c_str() : "";
}

const char *sevenzip_listing_container(const SevenZipListing *listing)
{
    return listing ? listing->Container.c_str() : "";
}

bool sevenzip_listing_is_solid(const SevenZipListing *listing)
{
    return listing && listing->Solid;
}

uint32_t sevenzip_listing_volumes(const SevenZipListing *listing)
{
    return listing ? listing->Volumes : 0;
}

bool sevenzip_listing_headers_encrypted(const SevenZipListing *listing)
{
    return listing && listing->HeadersEncrypted;
}

uint32_t sevenzip_listing_count(const SevenZipListing *listing)
{
    return listing ? (uint32_t)listing->Entries.size() : 0;
}

bool sevenzip_listing_item(const SevenZipListing *listing, uint32_t index, SevenZipItem *item)
{
    if (!listing || !item || index >= listing->Entries.size())
        return false;
    *item = listing->Entries[index].Item;
    return true;
}

int32_t sevenzip_extract(const char *archive, const char *destination, const char *const *items, size_t item_count,
                         int32_t overwrite, const char *password, const SevenZipCallbacks *callbacks)
{
    return Guarded([&] {
        return RunExtract(false, archive, destination, items, item_count, overwrite, password, callbacks);
    });
}

int32_t sevenzip_test(const char *archive, const char *password, const SevenZipCallbacks *callbacks)
{
    return Guarded([&] {
        return RunExtract(true, archive, NULL, NULL, 0, SEVENZIP_OVERWRITE, password, callbacks);
    });
}

int32_t sevenzip_update(const char *archive, const char *const *sources, size_t source_count,
                        const SevenZipUpdateOptions *options, const SevenZipCallbacks *callbacks)
{
    return Guarded([&] {
        if (!archive || !sources || source_count == 0 || !options || !options->format)
            return Fail(SEVENZIP_ERR_INVALID_PARAM, "Archive, sources and format are required");
        UStringVector switches;
        switches.Add(L"a");
        switches.Add(L"-y");
        switches.Add(L"-spd");
        // The archive is the file named, with or without an extension: 7-Zip added one and wrote another file
        switches.Add(L"-sae");
        switches.Add(UString(L"-t") + Wide(options->format));
        if (options->level >= 0)
        {
            UString s(L"-mx=");
            s.Add_UInt32((UInt32)options->level);
            switches.Add(s);
        }
        if (options->solid >= 0)
            switches.Add(options->solid ? L"-ms=on" : L"-ms=off");
        AddPassword(switches, options->password);
        if (options->password && options->encrypt_headers)
            switches.Add(L"-mhe=on");
        if (options->password && options->zip_aes256)
            switches.Add(L"-mem=AES256");
        if (options->volume_size > 0)
        {
            UString s(L"-v");
            s.Add_UInt64(options->volume_size);
            s += L"b";
            switches.Add(s);
        }
        AddNames(switches, archive, sources, source_count);
        return RunUpdate(switches, options->password, callbacks);
    });
}

void sevenzip_set_code_pages(uint32_t oem, uint32_t ansi)
{
    g_OemCodePage = oem;
    g_AnsiCodePage = ansi;
}

const char *sevenzip_last_error(void)
{
    return g_lastError.c_str();
}

const char *sevenzip_get_version(void)
{
    return SEVENZIP_APPLE_VERSION;
}
