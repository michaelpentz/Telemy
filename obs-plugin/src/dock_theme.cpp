#include "dock_theme.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)

#include <obs-module.h>
#include <QApplication>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QFont>
#include <QFile>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Theme cache globals — defined here, the single owner.
// ---------------------------------------------------------------------------
static std::mutex g_obs_dock_theme_mu;
static ObsDockThemeSlots g_obs_dock_theme_cache;
static std::string g_obs_dock_theme_signature;

// ---------------------------------------------------------------------------
// Qt color helpers
// ---------------------------------------------------------------------------

QString ColorToCssHex(const QColor& color) {
    if (!color.isValid()) {
        return QStringLiteral("#000000");
    }
    return color.name(QColor::HexRgb);
}

QColor BlendTowardWhite(const QColor& color, double ratio) {
    if (!color.isValid()) {
        return QColor(0, 0, 0);
    }
    const double clamped = std::max(0.0, std::min(1.0, ratio));
    const int r = static_cast<int>(color.red() + (255 - color.red()) * clamped);
    const int g = static_cast<int>(color.green() + (255 - color.green()) * clamped);
    const int b = static_cast<int>(color.blue() + (255 - color.blue()) * clamped);
    return QColor(r, g, b);
}

QColor BlendTowardBlack(const QColor& color, double ratio) {
    if (!color.isValid()) {
        return QColor(0, 0, 0);
    }
    const double clamped = std::max(0.0, std::min(1.0, ratio));
    const int r = static_cast<int>(color.red() * (1.0 - clamped));
    const int g = static_cast<int>(color.green() * (1.0 - clamped));
    const int b = static_cast<int>(color.blue() * (1.0 - clamped));
    return QColor(r, g, b);
}

QColor DerivedAccentLike(const QColor& base, double ratio) {
    int h = 0;
    int s = 0;
    int l = 0;
    int a = 255;
    if (!base.isValid()) {
        return QColor(96, 128, 160);
    }
    base.getHsl(&h, &s, &l, &a);
    if (l < 128) {
        return BlendTowardWhite(base, ratio);
    }
    return BlendTowardBlack(base, ratio);
}

double SrgbToLinear01(double c) {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return std::pow((c + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(const QColor& c) {
    if (!c.isValid()) {
        return 0.0;
    }
    const double r = SrgbToLinear01(static_cast<double>(c.red()) / 255.0);
    const double g = SrgbToLinear01(static_cast<double>(c.green()) / 255.0);
    const double b = SrgbToLinear01(static_cast<double>(c.blue()) / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

double ContrastRatio(const QColor& a, const QColor& b) {
    const double l1 = RelativeLuminance(a);
    const double l2 = RelativeLuminance(b);
    const double hi = std::max(l1, l2);
    const double lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

double MinContrastAgainst(const QColor& fg, const std::vector<QColor>& bgs) {
    if (!fg.isValid() || bgs.empty()) {
        return 0.0;
    }
    double best = 1e9;
    for (const auto& bg : bgs) {
        if (!bg.isValid()) {
            continue;
        }
        best = std::min(best, ContrastRatio(fg, bg));
    }
    return (best == 1e9) ? 0.0 : best;
}

QColor PickReadableTextColor(
    const std::vector<QColor>& candidates,
    const std::vector<QColor>& backgrounds,
    double min_ratio) {
    QColor best = QColor(0, 0, 0);
    double best_score = -1.0;
    for (const auto& c : candidates) {
        if (!c.isValid()) {
            continue;
        }
        const double score = MinContrastAgainst(c, backgrounds);
        if (score >= min_ratio) {
            return c;
        }
        if (score > best_score) {
            best_score = score;
            best = c;
        }
    }
    const QColor black(0, 0, 0);
    const QColor white(255, 255, 255);
    const double black_score = MinContrastAgainst(black, backgrounds);
    const double white_score = MinContrastAgainst(white, backgrounds);
    return (black_score >= white_score) ? black : white;
}

ObsDockThemeSlots qt_palette_to_theme() {
    ObsDockThemeSlots out;
    const QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) {
        return out;
    }

    const QPalette pal = app->palette();
    const QColor bg = pal.color(QPalette::Window);
    const QColor surface = pal.color(QPalette::Base);
    const QColor panel = pal.color(QPalette::Button);
    const QColor raw_window_text = pal.color(QPalette::WindowText);
    const QColor raw_text = pal.color(QPalette::Text);
    const QColor raw_button_text = pal.color(QPalette::ButtonText);
    const std::vector<QColor> text_bgs = {bg, surface, panel};
    const QColor text = PickReadableTextColor(
        {raw_window_text, raw_text, raw_button_text},
        text_bgs,
        4.5);
    QColor text_muted = pal.color(QPalette::PlaceholderText);
    if (!text_muted.isValid() || text_muted.alpha() == 0) {
        text_muted = text;
        text_muted.setAlpha(153); // ~60%
    }
    if (MinContrastAgainst(text_muted, text_bgs) < 2.4) {
        text_muted = (RelativeLuminance(text) < 0.5)
            ? BlendTowardWhite(text, 0.35)
            : BlendTowardBlack(text, 0.35);
    }
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor border = DerivedAccentLike(bg, 0.10);
    const QColor scrollbar = DerivedAccentLike(surface, 0.15);

    out.bg = ColorToCssHex(bg).toStdString();
    out.surface = ColorToCssHex(surface).toStdString();
    out.panel = ColorToCssHex(panel).toStdString();
    out.text = ColorToCssHex(text).toStdString();
    out.textMuted = ColorToCssHex(text_muted).toStdString();
    out.accent = ColorToCssHex(accent).toStdString();
    out.border = ColorToCssHex(border).toStdString();
    out.scrollbar = ColorToCssHex(scrollbar).toStdString();

    // Sample Qt application font family
    const QFont appFont = app->font();
    out.fontFamily = appFont.family().toStdString();

    // OBS doesn't propagate FontScale/Density to QApplication::font().
    // Read them directly from the OBS user config file (user.ini).
    int obsFontScale = 9;   // OBS default
    int obsDensity   = 0;   // OBS default
    {
        QString iniPath = QString::fromStdString(
            std::string(getenv("APPDATA") ? getenv("APPDATA") : "")) +
            "/obs-studio/user.ini";
        QFile iniFile(iniPath);
        if (iniFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            bool inAppearance = false;
            while (!iniFile.atEnd()) {
                QString line = iniFile.readLine().trimmed();
                if (line.startsWith('[')) {
                    inAppearance = (line.compare("[Appearance]", Qt::CaseInsensitive) == 0);
                    continue;
                }
                if (!inAppearance) continue;
                if (line.startsWith("FontScale=")) {
                    bool ok = false;
                    int v = line.mid(10).toInt(&ok);
                    if (ok && v > 0) obsFontScale = v;
                }
                if (line.startsWith("Density=")) {
                    bool ok = false;
                    int v = line.mid(8).toInt(&ok);
                    if (ok) obsDensity = v;
                }
            }
            iniFile.close();
        }
    }
    // Convert point size to pixel size at 96 DPI — density is a separate axis, not added here
    out.fontSizePx = qRound(static_cast<double>(obsFontScale) * 96.0 / 72.0);
    if (out.fontSizePx < 8) out.fontSizePx = 8; // floor
    out.densityLevel = obsDensity;

    blog(LOG_INFO, "[aegis-obs-plugin] font sample: family=%s sizePx=%d densityLevel=%d (obsFontScale=%d obsDensity=%d)",
         out.fontFamily.c_str(), out.fontSizePx, out.densityLevel, obsFontScale, obsDensity);
    out.valid = true;
    return out;
}

QJsonObject QtThemeToJsonObject(const ObsDockThemeSlots& theme) {
    QJsonObject obj;
    if (!theme.valid) {
        return obj;
    }
    obj.insert(QStringLiteral("bg"), QString::fromStdString(theme.bg));
    obj.insert(QStringLiteral("surface"), QString::fromStdString(theme.surface));
    obj.insert(QStringLiteral("panel"), QString::fromStdString(theme.panel));
    obj.insert(QStringLiteral("text"), QString::fromStdString(theme.text));
    obj.insert(QStringLiteral("textMuted"), QString::fromStdString(theme.textMuted));
    obj.insert(QStringLiteral("accent"), QString::fromStdString(theme.accent));
    obj.insert(QStringLiteral("border"), QString::fromStdString(theme.border));
    obj.insert(QStringLiteral("scrollbar"), QString::fromStdString(theme.scrollbar));
    obj.insert(QStringLiteral("fontFamily"), QString::fromStdString(theme.fontFamily));
    obj.insert(QStringLiteral("fontSizePx"), theme.fontSizePx);
    obj.insert(QStringLiteral("densityLevel"), theme.densityLevel);
    return obj;
}

ObsDockThemeSlots GetCachedObsDockTheme() {
    std::lock_guard<std::mutex> lock(g_obs_dock_theme_mu);
    return g_obs_dock_theme_cache;
}

void RefreshCachedObsDockThemeFromQt(const char* reason) {
    const ObsDockThemeSlots theme = qt_palette_to_theme();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_obs_dock_theme_mu);
        const std::string next_sig = theme.valid
            ? (theme.bg + "|" + theme.surface + "|" + theme.panel + "|" + theme.text + "|" +
               theme.textMuted + "|" + theme.accent + "|" + theme.border + "|" + theme.scrollbar + "|" + theme.fontFamily + "|" + std::to_string(theme.fontSizePx) + "|" + std::to_string(theme.densityLevel))
            : std::string();
        changed = (next_sig != g_obs_dock_theme_signature);
        g_obs_dock_theme_cache = theme;
        g_obs_dock_theme_signature = next_sig;
    }
    blog(
        (theme.valid && changed) ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-plugin] obs dock theme cache refresh: valid=%s changed=%s reason=%s",
        theme.valid ? "true" : "false",
        changed ? "true" : "false",
        reason ? reason : "unknown");
}

std::string AugmentSnapshotJsonWithTheme(const std::string& snapshot_json) {
    const ObsDockThemeSlots cached_theme = GetCachedObsDockTheme();
    if (!cached_theme.valid || snapshot_json.empty()) {
        return snapshot_json;
    }
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(snapshot_json));
    if (!doc.isObject()) {
        return snapshot_json;
    }
    QJsonObject obj = doc.object();
    obj.insert(QStringLiteral("theme"), QtThemeToJsonObject(cached_theme));
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

#endif // AEGIS_OBS_PLUGIN_BUILD
