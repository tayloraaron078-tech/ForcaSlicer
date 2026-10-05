#pragma once

// Forca: the Device tab for stock-firmware Flashforge printers (Adventurer 5M / 5X ...). Their firmware has no web
// interface, so the Device tab loads Forca's own page (resources/web/forca/flashforge/index.html) and this handler
// feeds it the printer's status through the local API and carries its Pause / Resume / Stop / light commands back.

#include "PrinterWebViewHandler.hpp"

#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

class PrinterWebView;

// The Device tab page for a Flashforge printer without its own web interface: a file:// URL.
std::string forca_flashforge_page_url();

std::unique_ptr<PrinterWebViewHandler> forca_make_flashforge_handler(PrinterWebView& owner);

}} // namespace Slic3r::GUI
