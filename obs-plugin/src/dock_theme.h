#pragma once

// dock_theme.h — OBS Qt palette theme detection and CSS variable generation.
// Extracted from obs_plugin_entry.cpp (RF-028).

#include <string>
#include <vector>

#if defined(TELEMY_OBS_PLUGIN_BUILD)
#include <QColor>
#include <QJsonObject>
#include <QString>
#endif

struct ObsDockThemeSlots {
    std::string bg;
    std::string surface;
    std::string panel;
    std::string text;
    std::string textMuted;
    std::string accent;
    std::string border;
    std::string scrollbar;
    std::string fontFamily;
    int fontSizePx = 0;
    int densityLevel = 0;
    bool valid = false;
};

#if defined(TELEMY_OBS_PLUGIN_BUILD)

// Qt color helpers
QString ColorToCssHex(const QColor& color);
QColor BlendTowardWhite(const QColor& color, double ratio);
QColor BlendTowardBlack(const QColor& color, double ratio);
QColor DerivedAccentLike(const QColor& base, double ratio);
double SrgbToLinear01(double c);
double RelativeLuminance(const QColor& c);
double ContrastRatio(const QColor& a, const QColor& b);
double MinContrastAgainst(const QColor& fg, const std::vector<QColor>& bgs);
QColor PickReadableTextColor(
    const std::vector<QColor>& candidates,
    const std::vector<QColor>& backgrounds,
    double min_ratio);

// Build theme slots from the current Qt application palette.
ObsDockThemeSlots qt_palette_to_theme();

// Convert theme slots to a QJsonObject for injection into status snapshots.
QJsonObject QtThemeToJsonObject(const ObsDockThemeSlots& theme);

// Return the most recently cached theme (thread-safe).
ObsDockThemeSlots GetCachedObsDockTheme();

// Re-sample the Qt palette and update the cached theme (thread-safe).
void RefreshCachedObsDockThemeFromQt(const char* reason);

// Merge current cached theme into a status snapshot JSON string.
std::string AugmentSnapshotJsonWithTheme(const std::string& snapshot_json);

#endif // TELEMY_OBS_PLUGIN_BUILD
