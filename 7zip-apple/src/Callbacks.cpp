#include "Callbacks.h"

#include "../../CPP/Common/StringConvert.h"
#include "../../CPP/Common/UTFConvert.h"
#include "../../CPP/Windows/ErrorMsg.h"

#include "../../CPP/7zip/UI/Common/OpenArchive.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

std::string Utf8(const UString &s)
{
    AString a;
    ConvertUnicodeToUTF8(s, a);
    return std::string(a.Ptr(), a.Len());
}

std::string Utf8(const wchar_t *s)
{
    return s ? Utf8(UString(s)) : std::string();
}

static std::string SystemErrorText(DWORD error)
{
    return Utf8(NWindows::NError::MyFormatMessage(error));
}

static std::string WithPath(const std::string &message, const FString &path)
{
    return message + " : " + Utf8(fs2us(path));
}

// ─── CReporter ──────────────────────────────────────────────

CReporter::CReporter(const SevenZipCallbacks *callbacks, COutcome &outcome):
    Outcome(outcome),
    _callbacks(callbacks)
{
}

HRESULT CReporter::CheckBreak()
{
    return Outcome.Cancelled || Outcome.NoSpace ? E_ABORT : S_OK;
}

// The bytes of the files in `dir`, not below it: a compressed tar unpacks into one.
static UInt64 BytesIn(const UString &dir)
{
    UInt64 bytes = 0;
    const std::string path = Utf8(dir);
    if (DIR *d = opendir(path.c_str()))
    {
        while (const dirent *entry = readdir(d))
        {
            struct stat st;
            if (fstatat(dirfd(d), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode))
                bytes += (UInt64)st.st_size;
        }
        closedir(d);
    }
    return bytes;
}

HRESULT CReporter::Progress(UInt64 completed)
{
    // A compressed tar can unpack to far more than it takes: stop it at its limit, before the disk is full
    if (ScratchLimit != 0 && !ScratchDir.IsEmpty() && BytesIn(ScratchDir) > ScratchLimit)
        Outcome.NoSpace = true;
    if (!_callbacks || !_callbacks->progress)
        return CheckBreak();
    UInt64 done = completed, total = Total;
    if (Stages > 1)
    {
        const UInt64 unit = 1u << 20;
        done = Stage * unit + (Total ? (UInt64)((double)completed / (double)Total * (double)unit) : 0);
        total = Stages * unit;
    }
    if (_callbacks->progress(_callbacks->context, done, total))
        Outcome.Cancelled = true;
    return CheckBreak();
}

void CReporter::Item(const wchar_t *path)
{
    if (_callbacks && _callbacks->item && path && Stage + 1 == Stages)
        _callbacks->item(_callbacks->context, Utf8(path).c_str());
}

void CReporter::Error(const std::string &message)
{
    if (Outcome.FirstError.empty())
        Outcome.FirstError = message;
    if (_callbacks && _callbacks->message)
        _callbacks->message(_callbacks->context, ("ERROR: " + message).c_str());
}

void CReporter::Warn(const std::string &message)
{
    Outcome.Warning = true;
    if (_callbacks && _callbacks->message)
        _callbacks->message(_callbacks->context, ("WARNING: " + message).c_str());
}

// Archive-level open result, shared by extract and update: the same
// distinctions 7zz draws between "is not an archive", "wrong password"
// and errors or warnings in an archive that did open.
static void ReportOpenResult(CReporter &reporter, const CArchiveLink &arcLink, const wchar_t *name, HRESULT result,
                             bool passwordGiven)
{
    FOR_VECTOR (level, arcLink.Arcs)
    {
        const CArcErrorInfo &er = arcLink.Arcs[level].ErrorInfo;
        if (er.GetErrorFlags() != 0 || !er.ErrorMessage.IsEmpty())
        {
            reporter.Outcome.DataError = true;
            reporter.Error(Utf8(er.ErrorMessage.IsEmpty() ? UString(L"Headers Error") : er.ErrorMessage)
                           + " : " + Utf8(name));
        }
        if (er.GetWarningFlags() != 0 || !er.WarningMessage.IsEmpty())
            reporter.Warn(Utf8(er.WarningMessage.IsEmpty() ? UString(L"Archive has warnings") : er.WarningMessage)
                          + " : " + Utf8(name));
    }
    if (result == S_OK || result == E_ABORT)
        return;
    if (result == S_FALSE)
    {
        if (arcLink.PasswordWasAsked)
        {
            if (passwordGiven)
                reporter.Outcome.WrongPassword = true;
            reporter.Error("Cannot open encrypted archive. Wrong password? : " + Utf8(name));
        }
        else
        {
            reporter.Outcome.NotArchive = true;
            reporter.Error("Cannot open the file as archive : " + Utf8(name));
        }
        return;
    }
    if (result == E_OUTOFMEMORY)
        reporter.Error("Can't allocate required memory : " + Utf8(name));
    else
        reporter.Error(Utf8(NWindows::NError::MyFormatMessage(result)) + " : " + Utf8(name));
}

// ─── COpenCallback ──────────────────────────────────────────

COpenCallback::COpenCallback(CReporter &reporter, const char *password):
    _reporter(reporter),
    _passwordIsDefined(password != NULL)
{
    if (password)
        ConvertUTF8ToUnicode(AString(password), _password);
}

HRESULT COpenCallback::ProvidePassword(BSTR *password)
{
    *password = NULL;
    RINOK(_reporter.CheckBreak())
    if (!_passwordIsDefined)
    {
        _reporter.Outcome.PasswordRequested = true;
        return E_ABORT;
    }
    return StringToBstr(_password, password);
}

HRESULT COpenCallback::Open_CheckBreak()
{
    return _reporter.CheckBreak();
}

HRESULT COpenCallback::Open_SetTotal(const UInt64 *, const UInt64 *)
{
    return _reporter.CheckBreak();
}

HRESULT COpenCallback::Open_SetCompleted(const UInt64 *, const UInt64 *)
{
    return _reporter.CheckBreak();
}

HRESULT COpenCallback::Open_Finished()
{
    return S_OK;
}

HRESULT COpenCallback::Open_CryptoGetTextPassword(BSTR *password)
{
    return ProvidePassword(password);
}

// ─── CExtractCallback ───────────────────────────────────────

CExtractCallback::CExtractCallback(CReporter &reporter, const char *password, Int32 overwriteAnswer):
    COpenCallback(reporter, password),
    _overwriteAnswer(overwriteAnswer)
{
}

Z7_COM7F_IMF(CExtractCallback::SetTotal(UInt64 total))
{
    _reporter.Total = total;
    return _reporter.CheckBreak();
}

Z7_COM7F_IMF(CExtractCallback::SetCompleted(const UInt64 *completeValue))
{
    return completeValue ? _reporter.Progress(*completeValue) : _reporter.CheckBreak();
}

Z7_COM7F_IMF(CExtractCallback::AskOverwrite(
    const wchar_t *, const FILETIME *, const UInt64 *,
    const wchar_t *, const FILETIME *, const UInt64 *,
    Int32 *answer))
{
    *answer = _overwriteAnswer;
    return _reporter.CheckBreak();
}

Z7_COM7F_IMF(CExtractCallback::PrepareOperation(const wchar_t *name, Int32, Int32, const UInt64 *))
{
    _currentName = name ? name : L"";
    _reporter.Item(name);
    return _reporter.CheckBreak();
}

Z7_COM7F_IMF(CExtractCallback::MessageError(const wchar_t *message))
{
    _reporter.Outcome.DataError = true;
    _reporter.Error(Utf8(message));
    return _reporter.CheckBreak();
}

void CExtractCallback::ReportOperationResult(Int32 opRes, Int32 encrypted, const wchar_t *name)
{
    namespace R = NArchive::NExtract::NOperationResult;
    if (opRes == R::kOK)
        return;
    const char *text = NULL;
    switch (opRes)
    {
        case R::kUnsupportedMethod: text = "Unsupported Method"; _reporter.Outcome.UnsupportedMethod = true; break;
        case R::kCRCError:          text = encrypted ? "CRC Failed in encrypted file. Wrong password?" : "CRC Failed"; break;
        case R::kDataError:         text = encrypted ? "Data Error in encrypted file. Wrong password?" : "Data Error"; break;
        case R::kUnavailable:       text = "Unavailable data"; break;
        case R::kUnexpectedEnd:     text = "Unexpected end of data"; break;
        case R::kDataAfterEnd:      text = "There are some data after the end of the payload data"; break;
        case R::kIsNotArc:          text = "Is not archive"; break;
        case R::kHeadersError:      text = "Headers Error"; break;
        case R::kWrongPassword:     text = "Wrong password"; break;
        default: break;
    }
    if (opRes == R::kWrongPassword
            || (encrypted && (opRes == R::kCRCError || opRes == R::kDataError || opRes == R::kHeadersError)))
        _reporter.Outcome.WrongPassword = true;
    else if (opRes != R::kUnsupportedMethod)
        _reporter.Outcome.DataError = true;
    std::string message = text ? text : "Error #" + std::to_string(opRes);
    const std::string path = Utf8(name ? UString(name) : _currentName);
    if (!path.empty())
        message += " : " + path;
    _reporter.Error(message);
}

Z7_COM7F_IMF(CExtractCallback::SetOperationResult(Int32 opRes, Int32 encrypted))
{
    ReportOperationResult(opRes, encrypted, NULL);
    return _reporter.CheckBreak();
}

Z7_COM7F_IMF(CExtractCallback::ReportExtractResult(Int32 opRes, Int32 encrypted, const wchar_t *name))
{
    ReportOperationResult(opRes, encrypted, name);
    return _reporter.CheckBreak();
}

HRESULT CExtractCallback::BeforeOpen(const wchar_t *, bool)
{
    return _reporter.CheckBreak();
}

HRESULT CExtractCallback::OpenResult(const CCodecs *, const CArchiveLink &arcLink, const wchar_t *name, HRESULT result)
{
    ReportOpenResult(_reporter, arcLink, name, result, _passwordIsDefined);
    return _reporter.CheckBreak();
}

HRESULT CExtractCallback::ThereAreNoFiles()
{
    return S_OK;
}

HRESULT CExtractCallback::ExtractResult(HRESULT result)
{
    if (result == S_OK || result == E_ABORT)
        return S_OK;
    if (result == E_OUTOFMEMORY)
        _reporter.Error("Can't allocate required memory");
    else
        _reporter.Error(Utf8(NWindows::NError::MyFormatMessage(result)));
    return S_OK;
}

HRESULT CExtractCallback::SetPassword(const UString &password)
{
    _passwordIsDefined = true;
    _password = password;
    return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword(BSTR *password))
{
    return ProvidePassword(password);
}

// Past the limit the answer is no: without one, 7-Zip's default answer, allow, stood and the item was extracted.
Z7_COM7F_IMF(CExtractCallback::RequestMemoryUse(
    UInt32 flags, UInt32, UInt32, const wchar_t *path,
    UInt64 requiredSize, UInt64 *allowedSize, UInt32 *answerFlags))
{
    if ((flags & NRequestMemoryUseFlags::k_IsReport) != 0 || requiredSize > *allowedSize)
    {
        std::string message = "Memory usage limit was exceeded: "
            + std::to_string((requiredSize + ((1u << 30) - 1)) >> 30) + " GB required, "
            + std::to_string((*allowedSize + ((1u << 30) - 1)) >> 30) + " GB allowed";
        if (path)
            message += " : " + Utf8(path);
        if ((flags & NRequestMemoryUseFlags::k_NoErrorMessage) == 0)
            _reporter.Error(message);
        *answerFlags = NRequestMemoryAnswerFlags::k_Limit_Exceeded;
    }
    return _reporter.CheckBreak();
}

// ─── CUpdateCallback ────────────────────────────────────────

CUpdateCallback::CUpdateCallback(CReporter &reporter, const char *password):
    _reporter(reporter),
    _passwordIsDefined(password != NULL)
{
    if (password)
        ConvertUTF8ToUnicode(AString(password), _password);
}

HRESULT CUpdateCallback::WriteSfx(const wchar_t *, UInt64) { return S_OK; }

HRESULT CUpdateCallback::SetTotal(UInt64 size)
{
    _reporter.Total = size;
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::SetCompleted(const UInt64 *completeValue)
{
    return completeValue ? _reporter.Progress(*completeValue) : _reporter.CheckBreak();
}

HRESULT CUpdateCallback::SetRatioInfo(const UInt64 *, const UInt64 *) { return S_OK; }

HRESULT CUpdateCallback::CheckBreak() { return _reporter.CheckBreak(); }

HRESULT CUpdateCallback::SetNumItems(const CArcToDoStat &) { return S_OK; }

HRESULT CUpdateCallback::GetStream(const wchar_t *name, bool, bool, UInt32)
{
    _reporter.Item(name);
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::OpenFileError(const FString &path, DWORD systemError)
{
    FailedFiles++;
    _reporter.Warn(WithPath(SystemErrorText(systemError), path));
    return S_FALSE;
}

HRESULT CUpdateCallback::ReadingFileError(const FString &path, DWORD systemError)
{
    _reporter.Error(WithPath(SystemErrorText(systemError), path));
    return HRESULT_FROM_WIN32(systemError);
}

HRESULT CUpdateCallback::SetOperationResult(Int32) { return _reporter.CheckBreak(); }

HRESULT CUpdateCallback::ReportExtractResult(Int32 opRes, Int32 isEncrypted, const wchar_t *name)
{
    if (opRes != NArchive::NExtract::NOperationResult::kOK)
    {
        if (isEncrypted)
            _reporter.Outcome.WrongPassword = true;
        else
            _reporter.Outcome.DataError = true;
        _reporter.Error("Error #" + std::to_string(opRes) + " : " + Utf8(name));
    }
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::ReportUpdateOperation(UInt32, const wchar_t *name, bool)
{
    _reporter.Item(name);
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *password)
{
    *password = NULL;
    *passwordIsDefined = BoolToInt(_passwordIsDefined);
    return StringToBstr(_password, password);
}

HRESULT CUpdateCallback::CryptoGetTextPassword(BSTR *password)
{
    *password = NULL;
    if (!_passwordIsDefined)
    {
        _reporter.Outcome.PasswordRequested = true;
        return E_ABORT;
    }
    return StringToBstr(_password, password);
}

HRESULT CUpdateCallback::ShowDeleteFile(const wchar_t *name, bool)
{
    _reporter.Item(name);
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::ScanError(const FString &path, DWORD systemError)
{
    FailedFiles++;
    _reporter.Warn(WithPath(SystemErrorText(systemError), path));
    return S_OK;
}

HRESULT CUpdateCallback::ScanProgress(const CDirItemsStat &, const FString &, bool)
{
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::OpenResult(const CCodecs *, const CArchiveLink &arcLink, const wchar_t *name, HRESULT result)
{
    ReportOpenResult(_reporter, arcLink, name, result, _passwordIsDefined);
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::StartScanning() { return _reporter.CheckBreak(); }
HRESULT CUpdateCallback::FinishScanning(const CDirItemsStat &) { return _reporter.CheckBreak(); }
HRESULT CUpdateCallback::StartOpenArchive(const wchar_t *) { return _reporter.CheckBreak(); }
HRESULT CUpdateCallback::StartArchive(const wchar_t *, bool) { return _reporter.CheckBreak(); }
HRESULT CUpdateCallback::FinishArchive(const CFinishArchiveStat &) { return S_OK; }

HRESULT CUpdateCallback::DeletingAfterArchiving(const FString &path, bool)
{
    _reporter.Item(fs2us(path).Ptr());
    return _reporter.CheckBreak();
}

HRESULT CUpdateCallback::FinishDeletingAfterArchiving() { return S_OK; }
HRESULT CUpdateCallback::MoveArc_Start(const wchar_t *, const wchar_t *, UInt64, Int32) { return S_OK; }
HRESULT CUpdateCallback::MoveArc_Progress(UInt64, UInt64) { return _reporter.CheckBreak(); }
HRESULT CUpdateCallback::MoveArc_Finish() { return S_OK; }
