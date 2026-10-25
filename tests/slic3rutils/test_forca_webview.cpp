// Forca: which pages in the Home tab's web view may send requests to the app (src/slic3r/GUI/WebViewDialog.cpp).
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "catch2/catch_message.hpp"
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/WebViewDialog.hpp"

#include <wx/string.h>

using namespace Slic3r::GUI;

TEST_CASE("Forca's own local pages may send requests to the app", "[ForcaWebView]")
{
    const wxString url = GENERATE(as<wxString>{},
                                  "file:///C:/Program%20Files/Forca%20Slicer/resources/web/homepage/index.html",
                                  "File:///C:/Program Files/Forca Slicer/resources/web/homepage/index.html",
                                  "file:///usr/share/forca-slicer/resources/web/homepage/index.html",
                                  "  file:///C:/forca/resources/web/homepage/index.html");
    CAPTURE(url.ToStdString());
    CHECK(is_local_file_url(url));
}

TEST_CASE("Web sites and empty pages may not send requests to the app", "[ForcaWebView]")
{
    const wxString url = GENERATE(as<wxString>{},
                                  "https://example.com/",
                                  "http://192.168.1.12:7125/",
                                  "about:blank",
                                  "memory:page1.htm",
                                  "",
                                  "https://example.com/file:///trick");
    CAPTURE(url.ToStdString());
    CHECK_FALSE(is_local_file_url(url));
}
