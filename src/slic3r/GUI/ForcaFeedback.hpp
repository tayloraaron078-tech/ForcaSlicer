#pragma once

namespace Slic3r { namespace GUI {

// Forca: asks once for feedback. Counts successful slices in the app config ("forca_feedback_slices"); at the
// FORCA_FEEDBACK_AFTER_SLICES-th it shows a notification whose link opens Forca's GitHub Discussions, and sets
// "forca_feedback_asked" so it never shows again. Nothing is sent anywhere: only the user's click opens the page.
constexpr int FORCA_FEEDBACK_AFTER_SLICES = 5;

// Call after a slice finished successfully.
void forca_feedback_note_slice();

}} // namespace Slic3r::GUI
