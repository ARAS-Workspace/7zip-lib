#ifndef SEVENZIP_APPLE_CALLBACKS_H
#define SEVENZIP_APPLE_CALLBACKS_H

#include "../../CPP/Common/Common.h"
#include "../../CPP/Common/MyCom.h"
#include "../../CPP/Common/MyString.h"

#include "../../CPP/7zip/Archive/IArchive.h"
#include "../../CPP/7zip/IPassword.h"
#include "../../CPP/7zip/UI/Common/ArchiveOpenCallback.h"
#include "../../CPP/7zip/UI/Common/IFileExtractCallback.h"
#include "../../CPP/7zip/UI/Common/Update.h"

#include "7zip_apple.h"

#include <string>

std::string Utf8(const wchar_t *s);
std::string Utf8(const UString &s);

/* What a callback saw during one call; the API maps it onto a return code. */
struct COutcome
{
    bool Cancelled = false;
    bool PasswordRequested = false;   // a password was needed and none was given
    bool WrongPassword = false;
    bool NotArchive = false;
    bool UnsupportedMethod = false;
    bool DataError = false;
    bool Warning = false;
    std::string FirstError;
};

/* Forwards progress, item names and messages to the C callbacks. */
class CReporter
{
public:
    CReporter(const SevenZipCallbacks *callbacks, COutcome &outcome);

    HRESULT CheckBreak();
    HRESULT Progress(UInt64 completed);
    void Item(const wchar_t *path);
    void Error(const std::string &message);
    void Warn(const std::string &message);

    UInt64 Total = 0;
    COutcome &Outcome;

    // A call that runs in stages (a compressed tar is unpacked, then read)
    // reports one bar: stage Stage of Stages, items only from the last.
    unsigned Stage = 0;
    unsigned Stages = 1;

private:
    const SevenZipCallbacks *_callbacks;
};

/* IOpenCallbackUI: archive opening, including the password for encrypted headers. */
class COpenCallback: public IOpenCallbackUI
{
public:
    COpenCallback(CReporter &reporter, const char *password);
    virtual ~COpenCallback() {}

    Z7_IFACE_IMP(IOpenCallbackUI)

protected:
    HRESULT ProvidePassword(BSTR *password);

    CReporter &_reporter;
    bool _passwordIsDefined;
    UString _password;
};

/* The 7zz extract/test callback without the console. */
class CExtractCallback Z7_final:
    public IFolderArchiveExtractCallback,
    public IExtractCallbackUI,
    public IFolderArchiveExtractCallback2,
    public ICryptoGetTextPassword,
    public IArchiveRequestMemoryUseCallback,
    public COpenCallback,
    public CMyUnknownImp
{
    Z7_COM_QI_BEGIN2(IFolderArchiveExtractCallback)
    Z7_COM_QI_ENTRY(IFolderArchiveExtractCallback2)
    Z7_COM_QI_ENTRY(ICryptoGetTextPassword)
    Z7_COM_QI_ENTRY(IArchiveRequestMemoryUseCallback)
    Z7_COM_QI_END
    Z7_COM_ADDREF_RELEASE

    Z7_IFACE_COM7_IMP(IProgress)
    Z7_IFACE_COM7_IMP(IFolderArchiveExtractCallback)
    Z7_IFACE_IMP(IExtractCallbackUI)
    Z7_IFACE_COM7_IMP(IFolderArchiveExtractCallback2)
    Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
    Z7_IFACE_COM7_IMP(IArchiveRequestMemoryUseCallback)

public:
    CExtractCallback(CReporter &reporter, const char *password, Int32 overwriteAnswer);

private:
    void ReportOperationResult(Int32 opRes, Int32 encrypted, const wchar_t *name);

    Int32 _overwriteAnswer;
    UString _currentName;
};

/* The 7zz add callback without the console. */
class CUpdateCallback Z7_final: public IUpdateCallbackUI2
{
    Z7_IFACE_IMP(IUpdateCallbackUI)
    Z7_IFACE_IMP(IDirItemsCallback)
    Z7_IFACE_IMP(IUpdateCallbackUI2)

public:
    CUpdateCallback(CReporter &reporter, const char *password);

    unsigned FailedFiles = 0;

private:
    CReporter &_reporter;
    bool _passwordIsDefined;
    UString _password;
};

#endif
