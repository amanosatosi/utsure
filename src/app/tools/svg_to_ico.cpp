#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <array>
#include <cstddef>
#include <cstdio>

namespace {

struct IconImage final {
    int size{0};
    QByteArray png_bytes{};
};

void append_u16_le(QByteArray &bytes, const quint16 value) {
    bytes.append(static_cast<char>(value & 0xFFu));
    bytes.append(static_cast<char>((value >> 8u) & 0xFFu));
}

void append_u32_le(QByteArray &bytes, const quint32 value) {
    bytes.append(static_cast<char>(value & 0xFFu));
    bytes.append(static_cast<char>((value >> 8u) & 0xFFu));
    bytes.append(static_cast<char>((value >> 16u) & 0xFFu));
    bytes.append(static_cast<char>((value >> 24u) & 0xFFu));
}

bool render_svg_to_png(
    const QString &svg_path,
    const int icon_size,
    QByteArray &png_bytes,
    QString &error_message
) {
    QSvgRenderer renderer(svg_path);
    if (!renderer.isValid()) {
        error_message = QString("Failed to load SVG icon source: %1").arg(svg_path);
        return false;
    }

    QImage image(icon_size, icon_size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter);
    painter.end();

    QBuffer buffer(&png_bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        error_message = QString("Failed to open PNG buffer for icon size %1.").arg(icon_size);
        return false;
    }

    if (!image.save(&buffer, "PNG")) {
        error_message = QString("Failed to encode PNG icon size %1.").arg(icon_size);
        return false;
    }

    return true;
}

bool write_ico_file(
    const QString &svg_path,
    const QString &output_path,
    QString &error_message
) {
    static constexpr std::array<int, 7> kIconSizes{16, 24, 32, 48, 64, 128, 256};

    QByteArray ico_bytes;
    std::array<IconImage, kIconSizes.size()> icon_images{};

    for (std::size_t index = 0; index < kIconSizes.size(); ++index) {
        icon_images[index].size = kIconSizes[index];
        if (!render_svg_to_png(svg_path, kIconSizes[index], icon_images[index].png_bytes, error_message)) {
            return false;
        }
    }

    append_u16_le(ico_bytes, 0);
    append_u16_le(ico_bytes, 1);
    append_u16_le(ico_bytes, static_cast<quint16>(icon_images.size()));

    quint32 image_offset = 6 + static_cast<quint32>(icon_images.size()) * 16;
    for (const IconImage &icon_image : icon_images) {
        const quint8 entry_size = icon_image.size >= 256 ? 0 : static_cast<quint8>(icon_image.size);
        ico_bytes.append(static_cast<char>(entry_size));
        ico_bytes.append(static_cast<char>(entry_size));
        ico_bytes.append('\0');
        ico_bytes.append('\0');
        append_u16_le(ico_bytes, 1);
        append_u16_le(ico_bytes, 32);
        append_u32_le(ico_bytes, static_cast<quint32>(icon_image.png_bytes.size()));
        append_u32_le(ico_bytes, image_offset);
        image_offset += static_cast<quint32>(icon_image.png_bytes.size());
    }

    for (const IconImage &icon_image : icon_images) {
        ico_bytes.append(icon_image.png_bytes);
    }

    const QFileInfo output_info(output_path);
    if (!output_info.dir().exists() && !QDir().mkpath(output_info.dir().absolutePath())) {
        error_message = QString("Failed to create icon output directory: %1").arg(output_info.dir().absolutePath());
        return false;
    }

    QFile output_file(output_path);
    if (!output_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error_message = QString("Failed to open ICO output path: %1").arg(output_path);
        return false;
    }

    if (output_file.write(ico_bytes) != ico_bytes.size()) {
        error_message = QString("Failed to write ICO output path: %1").arg(output_path);
        return false;
    }

    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: svg_to_ico <input.svg> <output.ico>\n");
        return 1;
    }

    const QString svg_path = QString::fromLocal8Bit(argv[1]);
    const QString output_path = QString::fromLocal8Bit(argv[2]);
    QString error_message;

    if (!write_ico_file(svg_path, output_path, error_message)) {
        std::fprintf(stderr, "%s\n", error_message.toLocal8Bit().constData());
        return 1;
    }

    return 0;
}
