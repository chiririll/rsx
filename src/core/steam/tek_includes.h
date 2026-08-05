#pragma once

// Vendored tek-steamclient headers use GCC/Clang attribute syntax that MSVC
// does not understand. Treat unrecognized attributes as non-fatal here.
#pragma warning(push)
#pragma warning(disable: 5030) // unrecognized attribute
#pragma warning(disable: 4201) // nameless struct/union

#include <thirdparty/tek-steamclient/include/tek-steamclient/base.h>
#include <thirdparty/tek-steamclient/include/tek-steamclient/error.h>
#include <thirdparty/tek-steamclient/include/tek-steamclient/os.h>
#include <thirdparty/tek-steamclient/include/tek-steamclient/cm.h>
#include <thirdparty/tek-steamclient/include/tek-steamclient/content.h>
#include <thirdparty/tek-steamclient/include/tek-steamclient/sp.h>

#pragma warning(pop)

#if _DEBUG
#pragma comment(lib, "thirdparty/tek-steamclient/libtek-steamclient-2.lib")
#else
#pragma comment(lib, "thirdparty/tek-steamclient/libtek-steamclient-2.lib")
#endif
