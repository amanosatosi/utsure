#pragma once

#include "app_settings.hpp"

#include <QFont>
#include <QString>
#include <QStringList>

class QApplication;

enum class UiFontSource {
    system_pyidaungsu,
    bundled_pyidaungsu,
    saved_family,
    fallback_system
};

struct UiFontResolution final {
    QString family{};
    int point_size{10};
    UiFontSource source{UiFontSource::fallback_system};
    QString diagnostic{};
};

class UiFontManager final {
public:
    [[nodiscard]] static UiFontResolution resolve(
        const AppSettings::UiFontSettings &settings,
        const QFont &system_fallback
    );
    [[nodiscard]] static UiFontResolution apply(
        QApplication &application,
        const AppSettings::UiFontSettings &settings
    );
    [[nodiscard]] static QStringList available_font_families();
    [[nodiscard]] static AppSettings::UiFontSettings default_settings();
};
