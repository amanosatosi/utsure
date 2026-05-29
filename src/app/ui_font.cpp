#include "ui_font.hpp"

#include <QApplication>
#include <QFontDatabase>

#include <algorithm>

namespace {

constexpr int kDefaultPointSize = 10;
constexpr int kMinimumPointSize = 6;
constexpr int kMaximumPointSize = 24;
constexpr const char *kPyidaungsuFamily = "Pyidaungsu";
constexpr const char *kBundledRegularPath = ":/fonts/Pyidaungsu-2.5.3_Regular.ttf";
constexpr const char *kBundledBoldPath = ":/fonts/Pyidaungsu-2.5.3_Bold.ttf";

bool family_matches(const QString &candidate, const QString &family) {
    return candidate.compare(family, Qt::CaseInsensitive) == 0;
}

QString matching_family_or_empty(const QStringList &families, const QString &family) {
    const auto iterator = std::find_if(families.begin(), families.end(), [&family](const QString &candidate) {
        return family_matches(candidate, family);
    });
    return iterator == families.end() ? QString{} : *iterator;
}

int normalize_point_size(const int point_size) noexcept {
    return std::clamp(point_size, kMinimumPointSize, kMaximumPointSize);
}

QString load_bundled_pyidaungsu_family() {
    static bool attempted = false;
    static QString cached_family;
    if (attempted) {
        return cached_family;
    }
    attempted = true;

    const int regular_id = QFontDatabase::addApplicationFont(kBundledRegularPath);
    const int bold_id = QFontDatabase::addApplicationFont(kBundledBoldPath);

    QStringList loaded_families;
    if (regular_id >= 0) {
        loaded_families.append(QFontDatabase::applicationFontFamilies(regular_id));
    }
    if (bold_id >= 0) {
        loaded_families.append(QFontDatabase::applicationFontFamilies(bold_id));
    }

    const QString preferred = matching_family_or_empty(loaded_families, kPyidaungsuFamily);
    if (!preferred.isEmpty()) {
        cached_family = preferred;
        return cached_family;
    }

    cached_family = loaded_families.empty() ? QString{} : loaded_families.front();
    return cached_family;
}

UiFontResolution resolve_pyidaungsu(
    const AppSettings::UiFontSettings &settings,
    const QFont &system_fallback
) {
    const QStringList system_families = QFontDatabase::families();
    const QString system_pyidaungsu = matching_family_or_empty(system_families, kPyidaungsuFamily);
    if (!system_pyidaungsu.isEmpty()) {
        return UiFontResolution{
            .family = system_pyidaungsu,
            .point_size = normalize_point_size(settings.point_size),
            .source = UiFontSource::system_pyidaungsu,
            .diagnostic = "Using system Pyidaungsu for the app UI font."
        };
    }

    if (settings.use_bundled_myanmar_fallback) {
        const QString bundled_family = load_bundled_pyidaungsu_family();
        if (!bundled_family.isEmpty()) {
            return UiFontResolution{
                .family = bundled_family,
                .point_size = normalize_point_size(settings.point_size),
                .source = UiFontSource::bundled_pyidaungsu,
                .diagnostic = "Using bundled Pyidaungsu for the app UI font."
            };
        }
    }

    return UiFontResolution{
        .family = system_fallback.family(),
        .point_size = normalize_point_size(settings.point_size),
        .source = UiFontSource::fallback_system,
        .diagnostic = "Bundled Pyidaungsu could not be loaded; using the system UI font."
    };
}

}  // namespace

UiFontResolution UiFontManager::resolve(
    const AppSettings::UiFontSettings &settings,
    const QFont &system_fallback
) {
    const int point_size = normalize_point_size(settings.point_size);
    const auto pyidaungsu = resolve_pyidaungsu(settings, system_fallback);
    const QString requested_family = settings.family.trimmed();

    if (requested_family.isEmpty() || family_matches(requested_family, kPyidaungsuFamily)) {
        auto result = pyidaungsu;
        result.point_size = point_size;
        return result;
    }

    const QStringList families = QFontDatabase::families();
    const QString saved_family = matching_family_or_empty(families, requested_family);
    if (!saved_family.isEmpty()) {
        return UiFontResolution{
            .family = saved_family,
            .point_size = point_size,
            .source = UiFontSource::saved_family,
            .diagnostic = "Using saved app UI font family '" + saved_family + "'."
        };
    }

    auto result = pyidaungsu;
    result.point_size = point_size;
    result.diagnostic =
        QString("Saved app UI font family '%1' is not available; %2").arg(requested_family, result.diagnostic);
    return result;
}

UiFontResolution UiFontManager::apply(
    QApplication &application,
    const AppSettings::UiFontSettings &settings
) {
    auto resolution = resolve(settings, application.font());
    if (!resolution.family.trimmed().isEmpty()) {
        QFont font(resolution.family, resolution.point_size);
        application.setFont(font);
    }
    return resolution;
}

QStringList UiFontManager::available_font_families() {
    (void)load_bundled_pyidaungsu_family();
    QStringList families = QFontDatabase::families();
    families.removeDuplicates();
    families.sort(Qt::CaseInsensitive);
    return families;
}

AppSettings::UiFontSettings UiFontManager::default_settings() {
    return AppSettings::UiFontSettings{
        .family = kPyidaungsuFamily,
        .point_size = kDefaultPointSize,
        .use_bundled_myanmar_fallback = true
    };
}
