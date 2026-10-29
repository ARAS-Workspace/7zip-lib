// Common/StringConvert.h
// Changed by ARAS-Workspace (https://github.com/ARAS-Workspace/7zip-lib) from 7-Zip 26.05:
//   2026-10-13: On Apple systems, declares g_OemCodePage, g_AnsiCodePage and
//               CodePageToUnicodeString().

#ifndef ZIP7_INC_COMMON_STRING_CONVERT_H
#define ZIP7_INC_COMMON_STRING_CONVERT_H

#include "MyString.h"
#include "MyWindows.h"

UString MultiByteToUnicodeString(const AString &src, UINT codePage = CP_ACP);
UString MultiByteToUnicodeString(const char *src, UINT codePage = CP_ACP);

// optimized versions that work faster for ASCII strings
void MultiByteToUnicodeString2(UString &dest, const AString &src, UINT codePage = CP_ACP);
// void UnicodeStringToMultiByte2(AString &dest, const UString &s, UINT codePage, char defaultChar, bool &defaultCharWasUsed);
void UnicodeStringToMultiByte2(AString &dest, const UString &src, UINT codePage);

AString UnicodeStringToMultiByte(const UString &src, UINT codePage, char defaultChar, bool &defaultCharWasUsed);
AString UnicodeStringToMultiByte(const UString &src, UINT codePage = CP_ACP);

inline const wchar_t* GetUnicodeString(const wchar_t *u Z7_lifetimebound)  { return u; }
inline const UString& GetUnicodeString(const UString &u Z7_lifetimebound)  { return u; }

inline UString GetUnicodeString(const AString &a)  { return MultiByteToUnicodeString(a); }
inline UString GetUnicodeString(const char *a)     { return MultiByteToUnicodeString(a); }

inline UString GetUnicodeString(const AString &a, UINT codePage)
  { return MultiByteToUnicodeString(a, codePage); }
inline UString GetUnicodeString(const char *a, UINT codePage)
  { return MultiByteToUnicodeString(a, codePage); }

inline const wchar_t* GetUnicodeString(const wchar_t *u Z7_lifetimebound, UINT) { return u; }
inline const UString& GetUnicodeString(const UString &u Z7_lifetimebound, UINT) { return u; }

inline const char*    GetAnsiString(const char    *a Z7_lifetimebound) { return a; }
inline const AString& GetAnsiString(const AString &a Z7_lifetimebound) { return a; }

inline AString GetAnsiString(const wchar_t *u) { return UnicodeStringToMultiByte(UString(u)); }
inline AString GetAnsiString(const UString &u) { return UnicodeStringToMultiByte(u); }

/*
inline const char* GetOemString(const char* oem)
  { return oem; }
inline const AString& GetOemString(const AString &oem)
  { return oem; }
*/
const char* GetOemString(const char* oem);
const AString& GetOemString(const AString &oem);
inline AString GetOemString(const UString &u)
  { return UnicodeStringToMultiByte(u, CP_OEMCP); }

#ifdef _UNICODE
  inline const wchar_t* GetSystemString(const wchar_t *u Z7_lifetimebound) { return u;}
  inline const UString& GetSystemString(const UString &u Z7_lifetimebound) { return u;}
  inline const wchar_t* GetSystemString(const wchar_t *u Z7_lifetimebound, UINT /* codePage */) { return u;}
  inline const UString& GetSystemString(const UString &u Z7_lifetimebound, UINT /* codePage */) { return u;}
  
  inline UString GetSystemString(const AString &a, UINT codePage) { return MultiByteToUnicodeString(a, codePage); }
  inline UString GetSystemString(const char    *a, UINT codePage) { return MultiByteToUnicodeString(a, codePage); }
  inline UString GetSystemString(const AString &a) { return MultiByteToUnicodeString(a); }
  inline UString GetSystemString(const char    *a) { return MultiByteToUnicodeString(a); }
#else
  inline const char*    GetSystemString(const char    *a Z7_lifetimebound) { return a; }
  inline const AString& GetSystemString(const AString &a Z7_lifetimebound) { return a; }
  inline const char*    GetSystemString(const char    *a Z7_lifetimebound, UINT) { return a; }
  inline const AString& GetSystemString(const AString &a Z7_lifetimebound, UINT) { return a; }
  
  inline AString GetSystemString(const wchar_t *u) { return UnicodeStringToMultiByte(UString(u)); }
  inline AString GetSystemString(const UString &u) { return UnicodeStringToMultiByte(u); }
  inline AString GetSystemString(const UString &u, UINT codePage) { return UnicodeStringToMultiByte(u, codePage); }



  /*
  inline AString GetSystemString(const wchar_t *u)
  {
    UString s;
    s = u;
    return UnicodeStringToMultiByte(s);
  }
  */

#endif

#ifndef UNDER_CE
AString SystemStringToOemString(const CSysString &src);
#endif


#ifdef _WIN32
/* we don't need locale functions in Windows
   but we can define ENV_HAVE_LOCALE here for debug purposes */
// #define ENV_HAVE_LOCALE
#else
#define ENV_HAVE_LOCALE
#endif

#ifdef ENV_HAVE_LOCALE
void MY_SetLocale();
const char *GetLocale(void);
#endif

#if !defined(_WIN32) || defined(ENV_HAVE_LOCALE)
bool IsNativeUTF8();
#endif

#ifndef _WIN32
extern bool g_ForceToUTF8;
#endif

#ifdef __APPLE__
/* macOS has no OEM or ANSI code page of its own. Names stored in one (ZIP
   archives made on Windows) are read with the code pages set here, as Windows
   reads them with the system's; 0 leaves such names to g_ForceToUTF8. */
extern UINT g_OemCodePage;
extern UINT g_AnsiCodePage;
/* src in codePage (CP_OEMCP and CP_ACP stand for the code pages above), read
   through CoreFoundation. false, and dest untouched, when there is no such
   code page or src is not in it. */
bool CodePageToUnicodeString(UString &dest, const AString &src, UINT codePage);
#endif

#endif
