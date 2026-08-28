#include "PixelCompositor.h"
#include "TiledImage.h"

#include <algorithm>
#include <cmath>

#if defined(Q_PROCESSOR_X86_64) || defined(Q_PROCESSOR_X86)
#include <intrin.h>
#include <emmintrin.h>
#define SANKOTV_X86_SIMD 1
#endif

namespace {
inline void sourceOver(QRgb &destination, int maskAlpha, const QColor &color)
{
    const qreal sourceAlpha = (maskAlpha / 255.0) * color.alphaF();
    if (sourceAlpha <= 0.0) return;
    const QRgb old = destination;
    const qreal oldAlpha = qAlpha(old) / 255.0;
    const qreal outputAlpha = sourceAlpha + oldAlpha * (1.0 - sourceAlpha);
    const auto channel = [&](int sourceChannel, int oldChannel) {
        return outputAlpha <= 0 ? 0 : std::clamp(qRound(
            (sourceChannel * sourceAlpha + oldChannel * oldAlpha * (1.0 - sourceAlpha))
                / outputAlpha), 0, 255);
    };
    destination = qRgba(channel(color.red(), qRed(old)),
                        channel(color.green(), qGreen(old)),
                        channel(color.blue(), qBlue(old)),
                        qRound(outputAlpha * 255.0));
}

// The erase composite: the SAME published coverage that sourceOver would
// deposit instead REMOVES alpha - dst.a *= (1 - sourceAlpha) - and colour
// channels stay untouched (straight-alpha ARGB32: an invisible pixel's RGB
// is inert). Mirrors publish.frag's eraseMode branch line for line, and
// shares its arithmetic shape with sourceOver above so the two blends
// quantize identically.
inline void eraseOut(QRgb &destination, int maskAlpha, qreal strength)
{
    const qreal sourceAlpha = (maskAlpha / 255.0) * strength;
    if (sourceAlpha <= 0.0) return;
    const QRgb old = destination;
    const int outputAlpha =
        qRound((qAlpha(old) / 255.0) * (1.0 - sourceAlpha) * 255.0);
    // Fully erased pixels canonicalise to ALL-ZERO bytes: the GPU's
    // premultiplied render-target roundtrip zeroes RGB wherever alpha is
    // zero, and the commit stores exact bytes - without this, invisible
    // pixels would differ across the two paths and across undo replays.
    destination = outputAlpha == 0
        ? qRgba(0, 0, 0, 0)
        : qRgba(qRed(old), qGreen(old), qBlue(old), outputAlpha);
}

bool useSimd(PixelCompositor::Path path)
{
    return path != PixelCompositor::Path::Scalar && PixelCompositor::simdAvailable();
}
}

bool PixelCompositor::simdAvailable()
{
#if defined(SANKOTV_X86_SIMD)
    int cpu[4] = {};
    __cpuid(cpu, 1);
    return (cpu[3] & (1 << 26)) != 0; // SSE2; mandatory on Windows x64.
#else
    return false;
#endif
}

QString PixelCompositor::activePathName()
{
    return simdAvailable() ? QStringLiteral("SSE2 (scalar-exact arithmetic)")
                           : QStringLiteral("scalar");
}

void PixelCompositor::compositeGrayscaleTile(QImage &destination, const QPoint &origin,
                                             const QImage &mask, const QColor &color,
                                             Path path)
{
    const QRect clipped(QRect(origin, mask.size()).intersected(destination.rect()));
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        QRgb *output = reinterpret_cast<QRgb *>(destination.scanLine(y));
        const uchar *input = mask.constScanLine(y - origin.y());
        int x = clipped.left();
#if defined(SANKOTV_X86_SIMD)
        if (useSimd(path)) {
            for (; x + 15 <= clipped.right(); x += 16) {
                const int sourceX = x - origin.x();
                const __m128i alpha = _mm_loadu_si128(
                    reinterpret_cast<const __m128i *>(input + sourceX));
                alignas(16) uchar lanes[16];
                _mm_store_si128(reinterpret_cast<__m128i *>(lanes), alpha);
                for (int lane = 0; lane < 16; ++lane)
                    sourceOver(output[x + lane], lanes[lane], color);
            }
        }
#endif
        for (; x <= clipped.right(); ++x)
            sourceOver(output[x], input[x - origin.x()], color);
    }
}

void PixelCompositor::eraseGrayscaleTile(QImage &destination, const QPoint &origin,
                                         const QImage &mask, qreal strength)
{
    const QRect clipped(QRect(origin, mask.size()).intersected(destination.rect()));
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        QRgb *output = reinterpret_cast<QRgb *>(destination.scanLine(y));
        const uchar *input = mask.constScanLine(y - origin.y());
        for (int x = clipped.left(); x <= clipped.right(); ++x)
            eraseOut(output[x], input[x - origin.x()], strength);
    }
}

void PixelCompositor::compositeRgbaMaskTile(QImage &destination, const QPoint &origin,
                                            const QImage &mask, const QColor &color,
                                            Path path)
{
    const QRect clipped(QRect(origin, mask.size()).intersected(destination.rect()));
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        QRgb *output = reinterpret_cast<QRgb *>(destination.scanLine(y));
        const QRgb *input = reinterpret_cast<const QRgb *>(mask.constScanLine(y - origin.y()));
        int x = clipped.left();
#if defined(SANKOTV_X86_SIMD)
        if (useSimd(path)) {
            for (; x + 3 <= clipped.right(); x += 4) {
                const int sourceX = x - origin.x();
                const __m128i pixels = _mm_loadu_si128(
                    reinterpret_cast<const __m128i *>(input + sourceX));
                const __m128i reds = _mm_and_si128(_mm_srli_epi32(pixels, 16),
                                                    _mm_set1_epi32(0xff));
                alignas(16) int lanes[4];
                _mm_store_si128(reinterpret_cast<__m128i *>(lanes), reds);
                for (int lane = 0; lane < 4; ++lane)
                    sourceOver(output[x + lane], lanes[lane], color);
            }
        }
#endif
        for (; x <= clipped.right(); ++x)
            sourceOver(output[x], qRed(input[x - origin.x()]), color);
    }
}

QImage PixelCompositor::compositeRgbaMasks(const QImage &base,
                                           const QHash<QPoint, QImage> &masks,
                                           const QColor &color, Path path)
{
    QImage output = base.convertToFormat(QImage::Format_ARGB32);
    for (auto tile = masks.constBegin(); tile != masks.constEnd(); ++tile)
        compositeRgbaMaskTile(output, TiledImage::tileLayerRect(tile.key()).topLeft(),
                              tile.value(), color, path);
    return output;
}

QImage PixelCompositor::compositeRgbaMasksRegion(
    const QImage &baseRegion, const QPoint &regionCanvasOrigin,
    const QHash<QPoint, QImage> &masks, const QColor &color, Path path)
{
    QImage output = baseRegion.convertToFormat(QImage::Format_ARGB32);
    for (auto tile = masks.constBegin(); tile != masks.constEnd(); ++tile) {
        const QPoint canvasOrigin = TiledImage::tileLayerRect(tile.key()).topLeft();
        compositeRgbaMaskTile(output, canvasOrigin - regionCanvasOrigin,
                              tile.value(), color, path);
    }
    return output;
}

QImage PixelCompositor::convertGrayscale16ToRgba(const QImage &input,
                                                 bool mirrorVertically, Path path)
{
    const QImage source = input.format() == QImage::Format_Grayscale16
        ? input : input.convertToFormat(QImage::Format_Grayscale16);
    QImage output(source.size(), QImage::Format_RGBA8888);
    output.fill(0);
    for (int y = 0; y < source.height(); ++y) {
        const int sourceY = mirrorVertically ? source.height() - 1 - y : y;
        const quint16 *pixels = reinterpret_cast<const quint16 *>(source.constScanLine(sourceY));
        QRgb *destination = reinterpret_cast<QRgb *>(output.scanLine(y));
        int x = 0;
#if defined(SANKOTV_X86_SIMD)
        if (useSimd(path)) {
            const __m128i zero = _mm_setzero_si128();
            const __m128i rounding = _mm_set1_epi32(128);
            for (; x + 7 < source.width(); x += 8) {
                const __m128i packed = _mm_loadu_si128(
                    reinterpret_cast<const __m128i *>(pixels + x));
                __m128i low = _mm_unpacklo_epi16(packed, zero);
                __m128i high = _mm_unpackhi_epi16(packed, zero);
                low = _mm_add_epi32(low, rounding);
                high = _mm_add_epi32(high, rounding);
                low = _mm_srli_epi32(_mm_sub_epi32(low, _mm_srli_epi32(low, 8)), 8);
                high = _mm_srli_epi32(_mm_sub_epi32(high, _mm_srli_epi32(high, 8)), 8);
                const __m128i words = _mm_packs_epi32(low, high);
                const __m128i bytes = _mm_packus_epi16(words, zero);
                alignas(16) uchar lanes[16];
                _mm_store_si128(reinterpret_cast<__m128i *>(lanes), bytes);
                for (int lane = 0; lane < 8; ++lane) {
                    const int alpha = lanes[lane];
                    destination[x + lane] = qRgba(alpha, alpha, alpha, alpha);
                }
            }
        }
#endif
        for (; x < source.width(); ++x) {
            const quint32 adjusted = quint32(pixels[x]) + 128u;
            const int alpha = int((adjusted - (adjusted >> 8)) >> 8);
            destination[x] = qRgba(alpha, alpha, alpha, alpha);
        }
    }
    return output;
}
