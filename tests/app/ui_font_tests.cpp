#include "app_settings.hpp"
#include "ui_font.hpp"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>

#include <iostream>
#include <string_view>

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(app_resources);

    const QString sample_text = QString::fromUtf8(
        "\xE1\x80\xA1"
        "\\N"
        "\xE1\x80\x95"
        "\xE1\x80\xAD"
        "\xE1\x80\xAF"
        "\xE1\x80\x84"
        "\xE1\x80\xBA"
        "\xE1\x80\xB8"
        "\\N"
        "\xE1\x81\x88"
    );
    const auto default_resolution = UiFontManager::apply(app, AppSettings::defaults().ui_font);
    if (default_resolution.family.trimmed().isEmpty()) {
        return fail("Default UI font resolution did not return a usable family.");
    }

    if (default_resolution.source == UiFontSource::fallback_system) {
        return fail("Default UI font resolution did not use system or bundled Pyidaungsu.");
    }

    QLabel label;
    label.setText(sample_text);
    label.setFont(app.font());
    QLineEdit line_edit;
    line_edit.setText(sample_text);
    line_edit.setFont(app.font());
    if (label.text() != sample_text || line_edit.text() != sample_text) {
        return fail("Myanmar UI text was not preserved when assigned to basic widgets.");
    }

    auto invalid_settings = AppSettings::defaults().ui_font;
    invalid_settings.family = "Definitely Missing Utsure Font Family";
    invalid_settings.point_size = 12;
    const auto invalid_resolution = UiFontManager::resolve(invalid_settings, app.font());
    if (invalid_resolution.family == invalid_settings.family ||
        invalid_resolution.family.trimmed().isEmpty() ||
        invalid_resolution.diagnostic.trimmed().isEmpty()) {
        return fail("Invalid saved UI font family did not fall back with a diagnostic.");
    }

    const QStringList families = UiFontManager::available_font_families();
    if (!families.contains(default_resolution.family, Qt::CaseInsensitive)) {
        return fail("Available UI font families did not include the resolved bundled/system family.");
    }

    std::cout << "ui_font.family=" << default_resolution.family.toStdString() << '\n';
    std::cout << "ui_font.sample=ok\n";
    return 0;
}
