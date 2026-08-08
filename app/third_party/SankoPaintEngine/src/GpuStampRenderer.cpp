#include "GpuStampRenderer.h"
#include "PixelCompositor.h"
#include "DualBrushCompositor.h"

#include <QFile>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QMutex>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QPainter>
#include <QResource>
#include <QVector2D>
#include <QVector4D>
#include <QtCore/qfloat16.h>
#include <QtMath>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <cmath>
#include <vector>

static void initializeBrushShaderResources()
{
    Q_INIT_RESOURCE(sankotv_brush_shaders);
    Q_INIT_RESOURCE(sankotv_publication_shaders);
}

namespace {
QShader loadShader(const QString &path)
{
    static QMutex mutex;
    static QHash<QString, QShader> cache;
    QMutexLocker lock(&mutex);
    const auto cached = cache.constFind(path);
    if (cached != cache.cend())
        return cached.value();
    QFile file(path);
    const QShader shader = file.open(QIODevice::ReadOnly)
        ? QShader::fromSerialized(file.readAll()) : QShader();
    cache.insert(path, shader);
    return shader;
}

struct InstanceData {
    float centerX, centerY, size, opacity;
    float angleRadians, compression, hardness, flow;
    float canvasCenterX, canvasCenterY, grainDepth, grainScale;
    float grainRotation, grainContrast, grainMode, grainColor;
};

struct ColorInstanceData {
    float centerX, centerY, size, opacity;
    float angleRadians, compression, hardness, flow;
    float red, green, blue, unused;
    float canvasCenterX, canvasCenterY, grainDepth, grainScale;
    float grainRotation, grainContrast, grainMode, grainColor;
};

QImage tipTexture(const Brush &brush)
{
    const QImage mask = brush.hasCustomShape()
        ? brush.customShape().convertToFormat(QImage::Format_Grayscale8)
        : QImage(1, 1, QImage::Format_Grayscale8);
    QImage source = mask;
    if (!brush.hasCustomShape()) source.fill(255);
    // Static tip flips ride on the uploaded texture: mirroring the texture
    // is byte-for-byte equivalent to sign-flipping the sample coordinates
    // (which is what the CPU reference does), and it happens in TIP-LOCAL
    // space — before the shader's rotation — matching the documented
    // flip-then-rotate order. The GPU texture cache key carries the flip
    // state (see tipKey), so this never bakes into a reused texture. The
    // procedural 1x1 tip is symmetric and skips it.
    if (brush.hasCustomShape() && (brush.tipFlipX() || brush.tipFlipY()))
        source = source.flipped(
            (brush.tipFlipX() ? Qt::Horizontal : Qt::Orientations())
            | (brush.tipFlipY() ? Qt::Vertical : Qt::Orientations()));
    QImage rgba(source.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        const uchar *input = source.constScanLine(y);
        QRgb *output = reinterpret_cast<QRgb *>(rgba.scanLine(y));
        for (int x = 0; x < source.width(); ++x)
            output[x] = qRgba(input[x], input[x], input[x], 255);
    }
    return rgba;
}

QImage grainTexture(const Brush &brush)
{
    const QImage source = !brush.hasGrain()
        ? QImage(1, 1, QImage::Format_Grayscale8)
        : brush.grainTexture().convertToFormat(QImage::Format_Grayscale8);
    QImage gray = source;
    if (!brush.hasGrain()) gray.fill(255);
    QImage rgba(gray.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *input = gray.constScanLine(y);
        QRgb *output = reinterpret_cast<QRgb *>(rgba.scanLine(y));
        for (int x = 0; x < gray.width(); ++x)
            output[x] = qRgba(input[x], input[x], input[x], 255);
    }
    return rgba;
}


inline uchar unorm16To8(quint16 value)
{
    return static_cast<uchar>((quint32(value) * 255u + 32767u) / 65535u);
}

QImage r16ReadbackToRgba(const QByteArray &data, const QSize &size, bool mirrorVertically)
{
    QImage source(reinterpret_cast<const uchar *>(data.constData()), size.width(), size.height(),
                  size.width() * int(sizeof(quint16)), QImage::Format_Grayscale16);
    return PixelCompositor::convertGrayscale16ToRgba(source, mirrorVertically);
}

QImage rgbaReadbackToArgb(const QByteArray &data, const QSize &size, bool mirrorVertically)
{
    // QRhi returns logical RGBA bytes. Preserve that native QImage format and
    // let Qt's vectorized copy/mirror paths move complete rows; converting each
    // pixel through qRgba() was the ~1.18 GB/s scalar bottleneck.
    QImage view(reinterpret_cast<const uchar *>(data.constData()), size.width(), size.height(),
                size.width() * 4, QImage::Format_RGBA8888);
    return mirrorVertically ? view.mirrored(false, true) : view.copy();
}

QImage publicationBaseTile(const QImage &baseRegion, const QPoint &regionCanvasOrigin,
                           const QPoint &tileCoordinate)
{
    QImage tile(TiledImage::TileSize, TiledImage::TileSize, QImage::Format_RGBA8888);
    tile.fill(Qt::transparent);
    const QRect tileRect = TiledImage::tileLayerRect(tileCoordinate);
    const QRect regionRect(regionCanvasOrigin, baseRegion.size());
    const QRect intersection = tileRect.intersected(regionRect);
    if (!intersection.isEmpty()) {
        QPainter painter(&tile);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(intersection.topLeft() - tileRect.topLeft(), baseRegion,
                          intersection.translated(-regionCanvasOrigin));
    }
    return tile;
}

QImage publicationRegion(const QHash<QPoint, QImage> &tiles, const QImage &baseRegion,
                         const QPoint &regionCanvasOrigin)
{
    QImage output = baseRegion.convertToFormat(QImage::Format_ARGB32);
    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it)
        painter.drawImage(TiledImage::tileLayerRect(it.key()).topLeft() - regionCanvasOrigin,
                          it.value());
    return output;
}

QImage colorReadbackToRgba(const QByteArray &data, const QSize &size, bool mirrorVertically,
                           qreal opacityCeiling)
{
    QImage output(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        const int sourceY = mirrorVertically ? size.height() - 1 - y : y;
        uchar *destination = output.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            qreal red, green, blue, alpha;
            const qfloat16 *source = reinterpret_cast<const qfloat16 *>(data.constData())
                + (sourceY * size.width() + x) * 4;
            // D3D11's RGBA16F staging readback is exposed in native BGRA
            // channel order by this QRhi path.
            red = float(source[2]);
            green = float(source[1]);
            blue = float(source[0]);
            alpha = float(source[3]);
            if (alpha > 0.0) {
                red /= alpha;
                green /= alpha;
                blue /= alpha;
            }
            const qreal publishedAlpha = alpha * opacityCeiling;
            const int alpha8 = qBound(0, qRound(publishedAlpha * 255.0), 255);
            destination[x * 4 + 0] = alpha8 ? uchar(qBound(0, qRound(red * 255.0), 255)) : 0;
            destination[x * 4 + 1] = alpha8 ? uchar(qBound(0, qRound(green * 255.0), 255)) : 0;
            destination[x * 4 + 2] = alpha8 ? uchar(qBound(0, qRound(blue * 255.0), 255)) : 0;
            destination[x * 4 + 3] = uchar(alpha8);
        }
    }
    return output;
}
}

GpuStampRenderer::GpuStampRenderer()
{
    initializeBrushShaderResources();
    createRhi();
}

GpuStampRenderer::~GpuStampRenderer()
{
    resetLayerTextures();
    delete m_linearClampSampler;
    delete m_linearRepeatSampler;
    delete m_nearestClampSampler;
}

QRhiSampler *GpuStampRenderer::linearClampSampler()
{
    if (!m_linearClampSampler) {
        m_linearClampSampler = m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        m_linearClampSampler->create();
    }
    return m_linearClampSampler;
}

QRhiSampler *GpuStampRenderer::linearRepeatSampler()
{
    if (!m_linearRepeatSampler) {
        m_linearRepeatSampler = m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::Repeat, QRhiSampler::Repeat);
        m_linearRepeatSampler->create();
    }
    return m_linearRepeatSampler;
}

QRhiSampler *GpuStampRenderer::nearestClampSampler()
{
    if (!m_nearestClampSampler) {
        m_nearestClampSampler = m_rhi->newSampler(
            QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        m_nearestClampSampler->create();
    }
    return m_nearestClampSampler;
}

void GpuStampRenderer::resetLayerTextures()
{
    qDeleteAll(m_layerTextures);
    m_layerTextures.clear();
    qDeleteAll(m_grainTexturesGpu);
    m_grainTexturesGpu.clear();
    qDeleteAll(m_tipTexturesGpu);
    m_tipTexturesGpu.clear();
}

bool GpuStampRenderer::createRhi()
{
    const QRhi::Flags timingFlags = qEnvironmentVariableIsSet("SANKOTV_GPU_TIMESTAMPS")
        ? QRhi::EnableTimestamps : QRhi::Flags{};
#ifdef Q_OS_WIN
    QRhiD3D11InitParams d3dParameters;
    m_rhi.reset(QRhi::create(QRhi::D3D11, &d3dParameters, timingFlags));
    if (m_rhi)
        return true;
#endif
#if QT_CONFIG(opengl)
    QRhiGles2InitParams glParameters;
    QOffscreenSurface *surface = QRhiGles2InitParams::newFallbackSurface(glParameters.format);
    m_fallbackSurface.reset(surface);
    glParameters.fallbackSurface = surface;
    m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &glParameters, timingFlags));
#endif
    return m_rhi != nullptr;
}

QString GpuStampRenderer::backendName() const
{
    return m_rhi ? QString::fromLatin1(m_rhi->backendName()) : QStringLiteral("CPU fallback");
}

GpuStampRenderer::Result GpuStampRenderer::renderStroke(
    const QSize &layerSize, const Brush &brush, const QVector<StrokeStamp> &stamps)
{
    if (brush.usesColorStrokeBuffer())
        return renderColorStrokeInternal(layerSize, brush, stamps, nullptr, {}, false);
    return renderStrokeInternal(layerSize, brush, stamps, nullptr, {}, false);
}

GpuStampRenderer::Result GpuStampRenderer::renderStrokeForCommit(
    const QSize &layerSize, const Brush &brush, const QVector<StrokeStamp> &stamps,
    const QImage &baseRegion, const QPoint &regionCanvasOrigin, bool deferredReadback)
{
    deferredReadback = deferredReadback
        || qEnvironmentVariableIntValue("SANKOTV_DEFER_COMMIT_READBACK") == 1;
    if (brush.usesColorStrokeBuffer())
        return renderColorStrokeInternal(layerSize, brush, stamps, &baseRegion,
                                         regionCanvasOrigin, deferredReadback);
    return renderStrokeInternal(layerSize, brush, stamps, &baseRegion,
                                regionCanvasOrigin, deferredReadback);
}

GpuStampRenderer::Result GpuStampRenderer::renderDualStrokeForCommit(
    const QSize &layerSize, const Brush &brush,
    const QVector<StrokeStamp> &primaryStamps,
    const QVector<StrokeStamp> &secondaryStamps,
    const QImage &baseRegion, const QPoint &regionCanvasOrigin)
{
    QElapsedTimer elapsed;
    elapsed.start();
    Result primary = renderStroke(layerSize, brush, primaryStamps);
    if (!primary.succeeded)
        return primary;
    const Brush &secondaryBrush = brush.secondaryBrush();
    Result secondary = renderStroke(layerSize, secondaryBrush, secondaryStamps);
    if (!secondary.succeeded)
        return secondary;

    Result result;
    result.dualBrush = true;
    result.succeeded = true;
    result.cpuBackingCoherent = true;
    result.primaryLogicalStamps = primary.logicalStamps;
    result.secondaryLogicalStamps = secondary.logicalStamps;
    result.logicalStamps = primary.logicalStamps + secondary.logicalStamps;
    result.primaryStampInstances = primary.stampInstances;
    result.secondaryStampInstances = secondary.stampInstances;
    result.stampInstances = primary.stampInstances + secondary.stampInstances;
    result.primaryStampDrawCalls = primary.instancedDrawCalls;
    result.secondaryStampDrawCalls = secondary.instancedDrawCalls;
    result.instancedDrawCalls = primary.instancedDrawCalls + secondary.instancedDrawCalls;
    result.primaryInstanceStride = primary.stampInstances > 0
        ? primary.instanceBufferBytes / primary.stampInstances : 0;
    result.secondaryInstanceStride = secondary.stampInstances > 0
        ? secondary.instanceBufferBytes / secondary.stampInstances : 0;
    result.instanceBufferBytes = primary.instanceBufferBytes + secondary.instanceBufferBytes;
    result.dirtyTiles = primary.dirtyTiles;
    result.dirtyTiles.unite(secondary.dirtyTiles);
    result.primaryStrokeBufferBytes = qint64(primary.dirtyTiles.size())
        * primary.strokeTargetBytesPerTile;
    result.secondaryStrokeBufferBytes = qint64(secondary.dirtyTiles.size())
        * secondary.strokeTargetBytesPerTile;
    result.strokeTargetBytesPerTile = primary.strokeTargetBytesPerTile
        + secondary.strokeTargetBytesPerTile;
    result.strokeBufferFormat = QStringLiteral("A: %1; B: %2")
        .arg(primary.strokeBufferFormat, secondary.strokeBufferFormat);
    result.baseTipUploads = primary.baseTipUploads + secondary.baseTipUploads;
    result.grainTextureUploads = primary.grainTextureUploads + secondary.grainTextureUploads;
    result.grainTextureBytes = primary.grainTextureBytes + secondary.grainTextureBytes;
    result.resourcePipelineMs = primary.resourcePipelineMs + secondary.resourcePipelineMs;
    result.instanceBuildUploadMs = primary.instanceBuildUploadMs + secondary.instanceBuildUploadMs;
    result.renderSubmissionMs = primary.renderSubmissionMs + secondary.renderSubmissionMs;
    result.readbackSubmissionMs = primary.readbackSubmissionMs + secondary.readbackSubmissionMs;
    result.readbackConversionMs = primary.readbackConversionMs + secondary.readbackConversionMs;
    result.finalGpuWaitMs = primary.finalGpuWaitMs + secondary.finalGpuWaitMs;
    result.gpuFrameMs = primary.gpuFrameMs + secondary.gpuFrameMs;
    result.readbackBatches = primary.readbackBatches + secondary.readbackBatches;
    result.strokeBufferReadbackBytes = primary.readbackBytes + secondary.readbackBytes;
    result.readbackBytes = result.strokeBufferReadbackBytes;

    const QHash<QPoint, QImage> primaryTiles = brush.usesColorStrokeBuffer()
        ? primary.masks
        : DualBrushCompositor::colorizeCoverageTiles(primary.masks, brush.color());
    const QHash<QPoint, QImage> secondaryTiles = secondaryBrush.usesColorStrokeBuffer()
        ? secondary.masks
        : DualBrushCompositor::colorizeCoverageTiles(secondary.masks, secondaryBrush.color());
    Result publication = publishDualTiles(
        brush, primaryTiles, secondaryTiles, baseRegion, regionCanvasOrigin);
    if (!publication.succeeded)
        return publication;
    result.preparedLayer = publication.preparedLayer;
    result.masks = std::move(publication.masks);
    result.previewTiles = result.masks;
    result.publicationDrawCalls = publication.publicationDrawCalls;
    result.publicationSubmissionMs = publication.publicationSubmissionMs;
    result.resourcePipelineMs += publication.resourcePipelineMs;
    result.readbackSubmissionMs += publication.readbackSubmissionMs;
    result.readbackConversionMs += publication.readbackConversionMs;
    result.finalGpuWaitMs += publication.finalGpuWaitMs;
    result.gpuFrameMs += publication.gpuFrameMs;
    result.readbackBatches += publication.readbackBatches;
    result.readbackBytes += publication.readbackBytes;
    result.publicationReadbackBytes = publication.readbackBytes;
    result.compositedOnGpu = true;
    result.totalPreciseMs = elapsed.nsecsElapsed() / 1000000.0;
    result.elapsedMs = elapsed.elapsed();
    const double accounted = result.resourcePipelineMs + result.instanceBuildUploadMs
        + result.renderSubmissionMs + result.readbackSubmissionMs
        + result.readbackConversionMs + result.finalGpuWaitMs;
    result.unattributedCpuMs = qMax(0.0, result.totalPreciseMs - accounted);
    return result;
}

GpuStampRenderer::Result GpuStampRenderer::publishDualTiles(
    const Brush &brush, const QHash<QPoint, QImage> &primaryTiles,
    const QHash<QPoint, QImage> &secondaryTiles, const QImage &baseRegion,
    const QPoint &regionCanvasOrigin)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QElapsedTimer phase;
    Result result;
    result.dualBrush = true;
    result.dirtyTiles = QSet<QPoint>(primaryTiles.keyBegin(), primaryTiles.keyEnd());
    result.dirtyTiles.unite(QSet<QPoint>(secondaryTiles.keyBegin(), secondaryTiles.keyEnd()));
    if (result.dirtyTiles.isEmpty()) {
        result.succeeded = true;
        result.preparedLayer = baseRegion;
        return result;
    }
    if (!m_rhi) {
        result.error = QStringLiteral("No QRhi backend available for dual publication");
        return result;
    }

    QRhiCommandBuffer *commandBuffer = nullptr;
    if (m_rhi->beginOffscreenFrame(&commandBuffer) != QRhi::FrameOpSuccess) {
        result.error = QStringLiteral("Could not begin dual publication frame");
        return result;
    }
    phase.start();
    const float quad[] = {-1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1};
    std::unique_ptr<QRhiBuffer> vertexBuffer(m_rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
    vertexBuffer->create();
    std::unique_ptr<QRhiBuffer> uniformBuffer(m_rhi->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 80));
    uniformBuffer->create();
    QRhiSampler *sampler = nearestClampSampler();

    struct TileResources {
        QPoint coordinate;
        QImage basePixels;
        QImage primaryPixels;
        QImage secondaryPixels;
        std::unique_ptr<QRhiTexture> base;
        std::unique_ptr<QRhiTexture> primary;
        std::unique_ptr<QRhiTexture> secondary;
        std::unique_ptr<QRhiTexture> output;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiRenderPassDescriptor> pass;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
        std::unique_ptr<QRhiReadbackResult> readback;
    };
    std::vector<std::unique_ptr<TileResources>> resources;
    resources.reserve(size_t(result.dirtyTiles.size()));
    const QSize tileSize(TiledImage::TileSize, TiledImage::TileSize);
    const auto transparentTile = [tileSize] {
        QImage image(tileSize, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        return image;
    };
    for (const QPoint &coordinate : result.dirtyTiles) {
        auto tile = std::make_unique<TileResources>();
        tile->coordinate = coordinate;
        tile->basePixels = publicationBaseTile(baseRegion, regionCanvasOrigin, coordinate)
            .convertToFormat(QImage::Format_RGBA8888);
        tile->primaryPixels = primaryTiles.contains(coordinate)
            ? primaryTiles.value(coordinate).convertToFormat(QImage::Format_RGBA8888)
            : transparentTile();
        tile->secondaryPixels = secondaryTiles.contains(coordinate)
            ? secondaryTiles.value(coordinate).convertToFormat(QImage::Format_RGBA8888)
            : transparentTile();
        tile->base.reset(m_rhi->newTexture(QRhiTexture::RGBA8, tileSize));
        tile->primary.reset(m_rhi->newTexture(QRhiTexture::RGBA8, tileSize));
        tile->secondary.reset(m_rhi->newTexture(QRhiTexture::RGBA8, tileSize));
        tile->output.reset(m_rhi->newTexture(
            QRhiTexture::RGBA8, tileSize, 1, QRhiTexture::RenderTarget));
        if (!tile->base->create() || !tile->primary->create()
            || !tile->secondary->create() || !tile->output->create()) {
            m_rhi->endOffscreenFrame();
            result.error = QStringLiteral("Could not create dual publication textures");
            return result;
        }
        tile->target.reset(m_rhi->newTextureRenderTarget(
            QRhiTextureRenderTargetDescription(QRhiColorAttachment(tile->output.get()))));
        tile->pass.reset(tile->target->newCompatibleRenderPassDescriptor());
        tile->target->setRenderPassDescriptor(tile->pass.get());
        tile->target->create();
        tile->bindings.reset(m_rhi->newShaderResourceBindings());
        tile->bindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage
                    | QRhiShaderResourceBinding::FragmentStage, uniformBuffer.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage, tile->base.get(), sampler),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage, tile->primary.get(), sampler),
            QRhiShaderResourceBinding::sampledTexture(
                3, QRhiShaderResourceBinding::FragmentStage, tile->secondary.get(), sampler)});
        tile->bindings->create();
        resources.push_back(std::move(tile));
    }

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(m_rhi->newGraphicsPipeline());
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex,
         loadShader(QStringLiteral(":/shaders/shaders/dual_publish.vert.qsb"))},
        {QRhiShaderStage::Fragment,
         loadShader(QStringLiteral(":/shaders/shaders/dual_publish.frag.qsb"))}
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    pipeline->setVertexInputLayout(layout);
    pipeline->setShaderResourceBindings(resources.front()->bindings.get());
    pipeline->setRenderPassDescriptor(resources.front()->pass.get());
    if (!pipeline->create()) {
        m_rhi->endOffscreenFrame();
        result.error = QStringLiteral("Could not create dual publication pipeline");
        return result;
    }
    result.resourcePipelineMs = phase.nsecsElapsed() / 1000000.0;

    phase.restart();
    QMatrix4x4 clip = m_rhi->clipSpaceCorrMatrix();
    QByteArray uniforms(80, 0);
    memcpy(uniforms.data(), clip.constData(), 64);
    float *parameters = reinterpret_cast<float *>(uniforms.data() + 64);
    parameters[0] = float(int(brush.dualBlendMode()));
    parameters[1] = float(brush.dualMasterOpacity());
    QRhiResourceUpdateBatch *uploads = m_rhi->nextResourceUpdateBatch();
    uploads->uploadStaticBuffer(vertexBuffer.get(), quad);
    uploads->updateDynamicBuffer(uniformBuffer.get(), 0, uniforms.size(), uniforms.constData());
    for (auto &tile : resources) {
        uploads->uploadTexture(tile->base.get(), tile->basePixels);
        uploads->uploadTexture(tile->primary.get(), tile->primaryPixels);
        uploads->uploadTexture(tile->secondary.get(), tile->secondaryPixels);
    }
    commandBuffer->resourceUpdate(uploads);
    for (auto &tile : resources) {
        commandBuffer->beginPass(tile->target.get(), Qt::transparent, {1.0f, 0});
        commandBuffer->setGraphicsPipeline(pipeline.get());
        commandBuffer->setViewport({0, 0, float(TiledImage::TileSize), float(TiledImage::TileSize)});
        commandBuffer->setShaderResources(tile->bindings.get());
        const QRhiCommandBuffer::VertexInput input(vertexBuffer.get(), 0);
        commandBuffer->setVertexInput(0, 1, &input);
        commandBuffer->draw(6);
        commandBuffer->endPass();
        ++result.publicationDrawCalls;
    }
    result.publicationSubmissionMs = phase.nsecsElapsed() / 1000000.0;

    phase.restart();
    QRhiResourceUpdateBatch *readbacks = m_rhi->nextResourceUpdateBatch();
    for (auto &tile : resources) {
        tile->readback = std::make_unique<QRhiReadbackResult>();
        const bool mirror = !m_rhi->isYUpInFramebuffer();
        tile->readback->completed = [&result, readback = tile->readback.get(),
                                     coordinate = tile->coordinate, mirror] {
            QElapsedTimer conversion;
            conversion.start();
            result.masks.insert(coordinate, rgbaReadbackToArgb(
                readback->data, readback->pixelSize, mirror));
            result.readbackConversionMs += conversion.nsecsElapsed() / 1000000.0;
        };
        readbacks->readBackTexture(
            QRhiReadbackDescription(tile->output.get()), tile->readback.get());
    }
    commandBuffer->resourceUpdate(readbacks);
    result.readbackBatches = 1;
    result.readbackBytes = qint64(resources.size()) * TiledImage::TileSize
        * TiledImage::TileSize * 4;
    result.readbackSubmissionMs = phase.nsecsElapsed() / 1000000.0;
    phase.restart();
    const QRhi::FrameOpResult endResult = m_rhi->endOffscreenFrame();
    const double waitIncludingCallbacks = phase.nsecsElapsed() / 1000000.0;
    result.finalGpuWaitMs = qMax(0.0, waitIncludingCallbacks - result.readbackConversionMs);
    result.gpuFrameMs = commandBuffer->lastCompletedGpuTime() * 1000.0;
    result.succeeded = endResult == QRhi::FrameOpSuccess
        && result.masks.size() == result.dirtyTiles.size();
    if (result.succeeded)
        result.preparedLayer = publicationRegion(result.masks, baseRegion, regionCanvasOrigin);
    else
        result.error = QStringLiteral("Dual publication readback did not complete");
    result.compositedOnGpu = true;
    result.totalPreciseMs = elapsed.nsecsElapsed() / 1000000.0;
    result.elapsedMs = elapsed.elapsed();
    return result;
}

GpuStampRenderer::Result GpuStampRenderer::renderStrokeInternal(
    const QSize &layerSize, const Brush &brush, const QVector<StrokeStamp> &stamps,
    const QImage *baseRegion, const QPoint &regionCanvasOrigin, bool deferredReadback)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QElapsedTimer phase;
    phase.start();
    qint64 instanceBuildUploadNs = 0;
    qint64 resourcePipelineNs = 0;
    Result result;
    if (!m_rhi) {
        result.error = QStringLiteral("No QRhi backend available");
        return result;
    }
    if (stamps.isEmpty()) {
        result.succeeded = true;
        return result;
    }

    QHash<QPoint, QVector<InstanceData>> tileInstances;
    for (const StrokeStamp &stamp : stamps) {
        if (stamp.effectiveSize < .5 || stamp.effectiveOpacity <= 0)
            continue;
        ++result.logicalStamps;
        const qreal tiltMagnitude = std::hypot(stamp.point.tiltX, stamp.point.tiltY);
        const qreal normalizedTilt = brush.tiltAffectsShape()
            ? std::clamp(tiltMagnitude / 60.0, 0.0, 1.0) : 0.0;
        const qreal elongation = 1.0 + (brush.maxTiltElongation() - 1.0) * normalizedTilt;
        // Same composition as StrokeBuilder::shapedTipForStamp (the
        // documented order lives there): ellipticity = tilt anisotropy x
        // static roundness x roundness jitter; rotation = static angle +
        // azimuth + barrel + angle jitter. Flips ride on the uploaded tip
        // texture (see tipTexture()), which is the pre-rotation order.
        const qreal compression = (1.0 / elongation)
            * brush.tipRoundness() * stamp.roundness;
        const qreal azimuth = normalizedTilt > 0
            ? std::atan2(stamp.point.tiltY, stamp.point.tiltX) : 0.0;
        const qreal barrel = brush.rotationAffectsShape() ? qDegreesToRadians(stamp.point.rotation) : 0.0;
        const bool physicalOrientation = normalizedTilt > 0.0
            || (brush.rotationAffectsShape()
                && qAbs(std::remainder(stamp.point.rotation, 360.0)) > 0.01);
        const qreal viewportRotation = physicalOrientation
            ? qDegreesToRadians(stamp.point.viewportRotation) : 0.0;
        // Match the reference renderer's integer raster footprint exactly.
        // Pressure spacing remains continuous, but both renderers evaluate a
        // stamp on the same pixel-center grid.
        const int rasterSize = std::clamp(qRound(stamp.effectiveSize), 1, 2048);
        const int left = qRound(stamp.point.position.x() - rasterSize * .5);
        const int top = qRound(stamp.point.position.y() - rasterSize * .5);
        const qreal rasterCenterX = left + rasterSize * .5;
        const qreal rasterCenterY = top + rasterSize * .5;
        const QRect bounds(left, top, rasterSize, rasterSize);
        for (const QPoint &tile : TiledImage::tilesForRect(bounds.intersected(QRect(QPoint(), layerSize)))) {
            const QPoint origin = TiledImage::tileLayerRect(tile).topLeft();
            tileInstances[tile].append({float(rasterCenterX - origin.x()),
                                        float(rasterCenterY - origin.y()),
                                        float(rasterSize), float(stamp.effectiveOpacity),
                                        float(qDegreesToRadians(brush.tipAngle())
                                              + azimuth + barrel
                                              + qDegreesToRadians(stamp.angleJitterDegrees)
                                              - viewportRotation),
                                        float(compression), float(stamp.effectiveHardness),
                                        float(stamp.effectiveFlow),
                                        float(rasterCenterX), float(rasterCenterY),
                                        float(stamp.effectiveGrainDepth),
                                        float(brush.grainScale()),
                                        float(qDegreesToRadians(brush.grainRotation())),
                                        float(brush.grainContrast()),
                                        brush.grainMode() == Brush::GrainMode::StaticCanvas ? 1.0f : 0.0f,
                                        brush.grainAffectsColor() ? 1.0f : 0.0f});
        }
    }
    instanceBuildUploadNs += phase.nsecsElapsed();

    // A tablet commonly reports a zero-pressure press before its first move.
    // That is a valid preview request but produces no drawable instances.
    // Return an empty successful result instead of constructing a pipeline
    // from an empty render-target vector (resources.front()).
    if (tileInstances.isEmpty()) {
        result.succeeded = true;
        result.elapsedMs = elapsed.elapsed();
        return result;
    }

    phase.restart();
    QRhiCommandBuffer *commandBuffer = nullptr;
    if (m_rhi->beginOffscreenFrame(&commandBuffer) != QRhi::FrameOpSuccess) {
        result.error = QStringLiteral("Could not begin QRhi offscreen frame");
        return result;
    }

    const float quad[] = {-1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1};
    std::unique_ptr<QRhiBuffer> vertexBuffer(m_rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
    vertexBuffer->create();
    int totalInstances = 0;
    for (auto it = tileInstances.cbegin(); it != tileInstances.cend(); ++it)
        totalInstances += it.value().size();
    std::unique_ptr<QRhiBuffer> instanceBuffer(m_rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
        qMax(1, totalInstances) * int(sizeof(InstanceData))));
    instanceBuffer->create();
    result.instanceBufferBytes = totalInstances * int(sizeof(InstanceData));
    std::unique_ptr<QRhiBuffer> uniformBuffer(m_rhi->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 96));
    uniformBuffer->create();

    // The flip state is part of the texture identity: a flipped upload must
    // never be served for the unflipped brush (or vice versa).
    const quint64 tipKey = brush.hasCustomShape()
        ? (quint64(brush.customShape().cacheKey())
           ^ (brush.tipFlipX() ? 0x8000000000000000ULL : 0)
           ^ (brush.tipFlipY() ? 0x4000000000000000ULL : 0))
        : 1;
    QRhiTexture *tipTextureGpu = m_tipTexturesGpu.value(tipKey, nullptr);
    const bool uploadTip = !tipTextureGpu;
    QImage tip;
    if (uploadTip) {
        tip = tipTexture(brush);
        tipTextureGpu = m_rhi->newTexture(QRhiTexture::RGBA8, tip.size());
        if (!tipTextureGpu->create()) {
            delete tipTextureGpu;
            m_rhi->endOffscreenFrame();
            result.error = QStringLiteral("Could not create GPU brush-tip texture");
            return result;
        }
        m_tipTexturesGpu.insert(tipKey, tipTextureGpu);
    }
    QRhiSampler *sampler = linearClampSampler();
    QRhiSampler *grainSampler = linearRepeatSampler();
    const QImage grain = grainTexture(brush);
    const quint64 grainKey = !brush.hasGrain()
        ? 1 : quint64(brush.grainTexture().cacheKey());
    QRhiTexture *grainTextureGpu = m_grainTexturesGpu.value(grainKey, nullptr);
    const bool uploadGrain = !grainTextureGpu;
    if (uploadGrain) {
        grainTextureGpu = m_rhi->newTexture(QRhiTexture::RGBA8, grain.size());
        grainTextureGpu->create();
        m_grainTexturesGpu.insert(grainKey, grainTextureGpu);
        result.grainTextureUploads = brush.hasGrain() ? 1 : 0;
    }
    result.grainTextureBytes = brush.hasGrain() ? grain.sizeInBytes() : 0;
    std::unique_ptr<QRhiShaderResourceBindings> bindings(m_rhi->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniformBuffer.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                   tipTextureGpu, sampler),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                   grainTextureGpu, grainSampler)
    });
    bindings->create();
    resourcePipelineNs += phase.nsecsElapsed();

    const bool highPrecisionFlow = brush.flow() < 0.999999;
    const QRhiTexture::Format strokeTargetFormat = highPrecisionFlow
        ? QRhiTexture::R16 : QRhiTexture::RGBA8;
    if (highPrecisionFlow
        && !m_rhi->isTextureFormatSupported(strokeTargetFormat, QRhiTexture::RenderTarget)) {
        m_rhi->endOffscreenFrame();
        result.error = QStringLiteral("R16 flow accumulator render targets are unavailable");
        return result;
    }
    result.strokeTargetBytesPerTile = highPrecisionFlow
        ? TiledImage::TileSize * TiledImage::TileSize * 2
        : TiledImage::TileSize * TiledImage::TileSize * 4;
    result.strokeBufferFormat = highPrecisionFlow
        ? QStringLiteral("R16 coverage-only") : QStringLiteral("RGBA8 max-coverage compatibility");

    struct TileResources {
        QPoint coordinate;
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiRenderPassDescriptor> pass;
        std::unique_ptr<QRhiReadbackResult> readback;
        std::unique_ptr<QRhiTexture> publicationBase;
        QImage publicationBasePixels;
        std::unique_ptr<QRhiTexture> publicationTexture;
        std::unique_ptr<QRhiTextureRenderTarget> publicationTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> publicationPass;
        std::unique_ptr<QRhiShaderResourceBindings> publicationBindings;
        quint32 instanceOffset = 0;
        quint32 instanceCount = 0;
    };
    std::vector<std::unique_ptr<TileResources>> resources;
    QVector<InstanceData> flattenedInstances;
    flattenedInstances.reserve(totalInstances);
    for (auto it = tileInstances.cbegin(); it != tileInstances.cend(); ++it) {
        phase.restart();
        auto tile = std::make_unique<TileResources>();
        tile->coordinate = it.key();
        tile->instanceOffset = flattenedInstances.size() * sizeof(InstanceData);
        tile->instanceCount = it.value().size();
        flattenedInstances.append(it.value());
        instanceBuildUploadNs += phase.nsecsElapsed();
        phase.restart();
        tile->texture.reset(m_rhi->newTexture(strokeTargetFormat,
                                               QSize(TiledImage::TileSize, TiledImage::TileSize), 1,
                                               QRhiTexture::RenderTarget));
        tile->texture->create();
        tile->target.reset(m_rhi->newTextureRenderTarget(
            QRhiTextureRenderTargetDescription(QRhiColorAttachment(tile->texture.get()))));
        tile->pass.reset(tile->target->newCompatibleRenderPassDescriptor());
        tile->target->setRenderPassDescriptor(tile->pass.get());
        tile->target->create();
        resources.push_back(std::move(tile));
        resourcePipelineNs += phase.nsecsElapsed();
    }

    phase.restart();
    std::unique_ptr<QRhiGraphicsPipeline> pipeline(m_rhi->newGraphicsPipeline());
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/shaders/stamp.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/shaders/stamp.frag.qsb"))}
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({{2 * sizeof(float)},
                             {sizeof(InstanceData), QRhiVertexInputBinding::PerInstance}});
    inputLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                               {1, 1, QRhiVertexInputAttribute::Float4, 0},
                               {1, 2, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
                               {1, 3, QRhiVertexInputAttribute::Float4, 8 * sizeof(float)},
                               {1, 4, QRhiVertexInputAttribute::Float4, 12 * sizeof(float)}});
    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(bindings.get());
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    const bool accumulateFlow = highPrecisionFlow;
    if (accumulateFlow) {
        blend.opColor = QRhiGraphicsPipeline::Add;
        blend.opAlpha = QRhiGraphicsPipeline::Add;
        blend.srcColor = QRhiGraphicsPipeline::One;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    } else {
        blend.opColor = QRhiGraphicsPipeline::Max;
        blend.opAlpha = QRhiGraphicsPipeline::Max;
        blend.srcColor = blend.dstColor = QRhiGraphicsPipeline::One;
        blend.srcAlpha = blend.dstAlpha = QRhiGraphicsPipeline::One;
    }
    pipeline->setTargetBlends({blend});
    pipeline->setRenderPassDescriptor(resources.front()->pass.get());
    if (!pipeline->create()) {
        m_rhi->endOffscreenFrame();
        result.error = QStringLiteral("Could not create GPU stamp pipeline");
        return result;
    }
    resourcePipelineNs += phase.nsecsElapsed();

    phase.restart();
    QMatrix4x4 clip = m_rhi->clipSpaceCorrMatrix();
    QByteArray uniforms(96, 0);
    memcpy(uniforms.data(), clip.constData(), 64);
    *reinterpret_cast<qint32 *>(uniforms.data() + 64) = brush.hasCustomShape() ? 1 : 0;
    *reinterpret_cast<qint32 *>(uniforms.data() + 68) = accumulateFlow ? 1 : 0;
    QRhiResourceUpdateBatch *initialUpdates = m_rhi->nextResourceUpdateBatch();
    initialUpdates->uploadStaticBuffer(vertexBuffer.get(), quad);
    initialUpdates->uploadStaticBuffer(instanceBuffer.get(), flattenedInstances.constData());
    if (uploadTip)
        initialUpdates->uploadTexture(tipTextureGpu, tip);
    if (uploadGrain)
        initialUpdates->uploadTexture(grainTextureGpu, grain);
    initialUpdates->updateDynamicBuffer(uniformBuffer.get(), 0, uniforms.size(), uniforms.constData());
    commandBuffer->resourceUpdate(initialUpdates);
    result.baseTipUploads = uploadTip ? 1 : 0;
    instanceBuildUploadNs += phase.nsecsElapsed();
    result.resourcePipelineMs = resourcePipelineNs / 1000000.0;
    result.instanceBuildUploadMs = instanceBuildUploadNs / 1000000.0;

    phase.restart();
    for (size_t index = 0; index < resources.size(); ++index) {
        TileResources &tile = *resources[index];
        commandBuffer->beginPass(tile.target.get(), Qt::transparent, {1.0f, 0});
        commandBuffer->setGraphicsPipeline(pipeline.get());
        commandBuffer->setViewport({0, 0, float(TiledImage::TileSize), float(TiledImage::TileSize)});
        commandBuffer->setShaderResources();
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {vertexBuffer.get(), 0}, {instanceBuffer.get(), tile.instanceOffset}
        };
        commandBuffer->setVertexInput(0, 2, inputs);
        commandBuffer->draw(6, tile.instanceCount);
        commandBuffer->endPass();
        ++result.instancedDrawCalls;
        result.stampInstances += tile.instanceCount;
    }
    result.renderSubmissionMs = phase.nsecsElapsed() / 1000000.0;

    const bool gpuPublication = baseRegion != nullptr;
    QRhiSampler *publicationSampler = nullptr;
    std::unique_ptr<QRhiBuffer> publicationUniformBuffer;
    std::unique_ptr<QRhiGraphicsPipeline> publicationPipeline;
    if (gpuPublication) {
        phase.restart();
        publicationUniformBuffer.reset(m_rhi->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 96));
        publicationUniformBuffer->create();
        publicationSampler = nearestClampSampler();
        for (auto &resource : resources) {
            resource->publicationBase.reset(m_rhi->newTexture(
                QRhiTexture::RGBA8, QSize(TiledImage::TileSize, TiledImage::TileSize)));
            resource->publicationBase->create();
            resource->publicationTexture.reset(m_rhi->newTexture(
                QRhiTexture::RGBA8, QSize(TiledImage::TileSize, TiledImage::TileSize), 1,
                QRhiTexture::RenderTarget));
            resource->publicationTexture->create();
            resource->publicationTarget.reset(m_rhi->newTextureRenderTarget(
                QRhiTextureRenderTargetDescription(
                    QRhiColorAttachment(resource->publicationTexture.get()))));
            resource->publicationPass.reset(
                resource->publicationTarget->newCompatibleRenderPassDescriptor());
            resource->publicationTarget->setRenderPassDescriptor(
                resource->publicationPass.get());
            resource->publicationTarget->create();
            resource->publicationBindings.reset(m_rhi->newShaderResourceBindings());
            resource->publicationBindings->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::VertexStage
                        | QRhiShaderResourceBinding::FragmentStage,
                    publicationUniformBuffer.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage,
                    resource->publicationBase.get(), publicationSampler),
                QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage,
                    resource->texture.get(), publicationSampler)
            });
            resource->publicationBindings->create();
        }
        publicationPipeline.reset(m_rhi->newGraphicsPipeline());
        publicationPipeline->setShaderStages({
            {QRhiShaderStage::Vertex,
             loadShader(QStringLiteral(":/shaders/shaders/publish.vert.qsb"))},
            {QRhiShaderStage::Fragment,
             loadShader(QStringLiteral(":/shaders/shaders/publish.frag.qsb"))}
        });
        QRhiVertexInputLayout publicationLayout;
        publicationLayout.setBindings({{2 * sizeof(float)}});
        publicationLayout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0}
        });
        publicationPipeline->setVertexInputLayout(publicationLayout);
        publicationPipeline->setShaderResourceBindings(
            resources.front()->publicationBindings.get());
        publicationPipeline->setRenderPassDescriptor(
            resources.front()->publicationPass.get());
        if (!publicationPipeline->create()) {
            m_rhi->endOffscreenFrame();
            result.error = QStringLiteral("Could not create GPU publication pipeline");
            return result;
        }
        result.resourcePipelineMs += phase.nsecsElapsed() / 1000000.0;

        phase.restart();
        QRhiResourceUpdateBatch *publicationUpdates = m_rhi->nextResourceUpdateBatch();
        for (auto &resource : resources) {
            resource->publicationBasePixels = publicationBaseTile(
                *baseRegion, regionCanvasOrigin, resource->coordinate);
            publicationUpdates->uploadTexture(
                resource->publicationBase.get(),
                resource->publicationBasePixels);
        }
        QByteArray publicationUniforms(96, 0);
        memcpy(publicationUniforms.data(), clip.constData(), 64);
        const QColor brushColor = brush.color();
        float *color = reinterpret_cast<float *>(publicationUniforms.data() + 64);
        color[0] = float(brushColor.redF());
        color[1] = float(brushColor.greenF());
        color[2] = float(brushColor.blueF());
        color[3] = float(brushColor.alphaF());
        *reinterpret_cast<qint32 *>(publicationUniforms.data() + 80) = 0;
        publicationUpdates->updateDynamicBuffer(
            publicationUniformBuffer.get(), 0, publicationUniforms.size(),
            publicationUniforms.constData());
        commandBuffer->resourceUpdate(publicationUpdates);
        for (auto &resource : resources) {
            commandBuffer->beginPass(resource->publicationTarget.get(), Qt::transparent, {1.0f, 0});
            commandBuffer->setGraphicsPipeline(publicationPipeline.get());
            commandBuffer->setViewport(
                {0, 0, float(TiledImage::TileSize), float(TiledImage::TileSize)});
            commandBuffer->setShaderResources(resource->publicationBindings.get());
            const QRhiCommandBuffer::VertexInput input(vertexBuffer.get(), 0);
            commandBuffer->setVertexInput(0, 1, &input);
            commandBuffer->draw(6);
            commandBuffer->endPass();
            ++result.publicationDrawCalls;
        }
        result.publicationSubmissionMs = phase.nsecsElapsed() / 1000000.0;
        result.compositedOnGpu = true;
    }

    phase.restart();
    if (!deferredReadback) {
        QRhiResourceUpdateBatch *readbackUpdate = m_rhi->nextResourceUpdateBatch();
        for (size_t index = 0; index < resources.size(); ++index) {
            TileResources &tile = *resources[index];
            tile.readback = std::make_unique<QRhiReadbackResult>();
            const bool mirrorReadback = !m_rhi->isYUpInFramebuffer();
            tile.readback->completed = [&result, coordinate = tile.coordinate,
                                        readback = tile.readback.get(), mirrorReadback,
                                        highPrecisionFlow, gpuPublication] {
                QElapsedTimer conversion;
                conversion.start();
                if (gpuPublication) {
                    result.masks.insert(coordinate, rgbaReadbackToArgb(
                        readback->data, readback->pixelSize, mirrorReadback));
                } else if (highPrecisionFlow) {
                    result.masks.insert(coordinate, r16ReadbackToRgba(
                        readback->data, readback->pixelSize, mirrorReadback));
                } else {
                    QImage image(reinterpret_cast<const uchar *>(readback->data.constData()),
                                 readback->pixelSize.width(), readback->pixelSize.height(),
                                 QImage::Format_RGBA8888);
                    result.masks.insert(coordinate, mirrorReadback
                        ? image.mirrored(false, true) : image.copy());
                }
                result.readbackConversionMs += conversion.nsecsElapsed() / 1000000.0;
            };
            QRhiTexture *readbackTexture = gpuPublication
                ? tile.publicationTexture.get() : tile.texture.get();
            readbackUpdate->readBackTexture(
                QRhiReadbackDescription(readbackTexture), tile.readback.get());
        }
        commandBuffer->resourceUpdate(readbackUpdate);
        result.readbackBatches = 1;
        result.readbackBytes = qint64(resources.size()) * TiledImage::TileSize
            * TiledImage::TileSize * (gpuPublication ? 4 : (highPrecisionFlow ? 2 : 4));
        if (gpuPublication)
            result.publicationReadbackBytes = result.readbackBytes;
        else
            result.strokeBufferReadbackBytes = result.readbackBytes;
    } else {
        result.cpuBackingCoherent = false;
    }
    result.readbackSubmissionMs = phase.nsecsElapsed() / 1000000.0;
    phase.restart();
    const QRhi::FrameOpResult endResult = m_rhi->endOffscreenFrame();
    const double waitIncludingCallbacks = phase.nsecsElapsed() / 1000000.0;
    result.finalGpuWaitMs = qMax(0.0, waitIncludingCallbacks - result.readbackConversionMs);
    result.gpuFrameMs = commandBuffer->lastCompletedGpuTime() * 1000.0;
    result.succeeded = endResult == QRhi::FrameOpSuccess
        && (deferredReadback || result.masks.size() == tileInstances.size());
    for (auto it = tileInstances.cbegin(); it != tileInstances.cend(); ++it)
        result.dirtyTiles.insert(it.key());
    if (gpuPublication && !deferredReadback)
        result.preparedLayer = publicationRegion(
            result.masks, *baseRegion, regionCanvasOrigin);
    if (!result.succeeded)
        result.error = endResult == QRhi::FrameOpSuccess
            ? QStringLiteral("GPU tile readback did not complete")
            : QStringLiteral("GPU offscreen frame submission failed (%1)").arg(int(endResult));
    result.elapsedMs = elapsed.elapsed();
    result.totalPreciseMs = elapsed.nsecsElapsed() / 1000000.0;
    const double accounted = result.resourcePipelineMs + result.instanceBuildUploadMs
        + result.renderSubmissionMs + result.publicationSubmissionMs
        + result.readbackSubmissionMs + result.readbackConversionMs + result.finalGpuWaitMs;
    result.unattributedCpuMs = qMax(0.0, result.totalPreciseMs - accounted);
    return result;
}

GpuStampRenderer::Result GpuStampRenderer::renderColorStrokeInternal(
    const QSize &layerSize, const Brush &brush, const QVector<StrokeStamp> &stamps,
    const QImage *baseRegion,
    const QPoint &regionCanvasOrigin, bool deferredReadback)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QElapsedTimer phase;
    phase.start();
    qint64 instanceNs = 0;
    qint64 resourceNs = 0;
    Result result;
    if (!m_rhi) {
        result.error = QStringLiteral("No QRhi backend available");
        return result;
    }
    result.strokeBufferFormat = QStringLiteral("RGBA16F color/coverage target");

    QHash<QPoint, QVector<ColorInstanceData>> tileInstances;
    for (const StrokeStamp &stamp : stamps) {
        if (stamp.effectiveSize < .5 || stamp.effectiveOpacity <= 0.0)
            continue;
        ++result.logicalStamps;
        const qreal tiltMagnitude = std::hypot(stamp.point.tiltX, stamp.point.tiltY);
        const qreal normalizedTilt = brush.tiltAffectsShape()
            ? std::clamp(tiltMagnitude / 60.0, 0.0, 1.0) : 0.0;
        const qreal elongation = 1.0 + (brush.maxTiltElongation() - 1.0) * normalizedTilt;
        // Same composition as the R16 path above and the CPU reference.
        const qreal compression = (1.0 / elongation)
            * brush.tipRoundness() * stamp.roundness;
        const qreal azimuth = normalizedTilt > 0
            ? std::atan2(stamp.point.tiltY, stamp.point.tiltX) : 0.0;
        const qreal barrel = brush.rotationAffectsShape()
            ? qDegreesToRadians(stamp.point.rotation) : 0.0;
        const bool physicalOrientation = normalizedTilt > 0.0
            || (brush.rotationAffectsShape()
                && qAbs(std::remainder(stamp.point.rotation, 360.0)) > 0.01);
        const qreal viewportRotation = physicalOrientation
            ? qDegreesToRadians(stamp.point.viewportRotation) : 0.0;
        const int rasterSize = std::clamp(qRound(stamp.effectiveSize), 1, 2048);
        const int left = qRound(stamp.point.position.x() - rasterSize * .5);
        const int top = qRound(stamp.point.position.y() - rasterSize * .5);
        const qreal centerX = left + rasterSize * .5;
        const qreal centerY = top + rasterSize * .5;
        const QColor color = stamp.resolvedColor;
        const QRect bounds(left, top, rasterSize, rasterSize);
        for (const QPoint &tile : TiledImage::tilesForRect(
                 bounds.intersected(QRect(QPoint(), layerSize)))) {
            const QPoint origin = TiledImage::tileLayerRect(tile).topLeft();
            tileInstances[tile].append({float(centerX - origin.x()), float(centerY - origin.y()),
                                        float(rasterSize), float(brush.opacity() > 0.0
                                            ? stamp.effectiveOpacity / brush.opacity() : 0.0),
                                        float(qDegreesToRadians(brush.tipAngle())
                                              + azimuth + barrel
                                              + qDegreesToRadians(stamp.angleJitterDegrees)
                                              - viewportRotation),
                                        float(compression), float(stamp.effectiveHardness),
                                        float(stamp.effectiveFlow), float(color.redF()),
                                        float(color.greenF()), float(color.blueF()), 0.0f,
                                        float(centerX), float(centerY),
                                        float(stamp.effectiveGrainDepth), float(brush.grainScale()),
                                        float(qDegreesToRadians(brush.grainRotation())),
                                        float(brush.grainContrast()),
                                        brush.grainMode() == Brush::GrainMode::StaticCanvas ? 1.0f : 0.0f,
                                        brush.grainAffectsColor() ? 1.0f : 0.0f});
        }
    }
    instanceNs += phase.nsecsElapsed();
    for (auto it = tileInstances.cbegin(); it != tileInstances.cend(); ++it)
        result.stampInstances += it.value().size();
    result.instanceBufferBytes = result.stampInstances * int(sizeof(ColorInstanceData));
    result.strokeTargetBytesPerTile = TiledImage::TileSize * TiledImage::TileSize * 8;

    if (tileInstances.isEmpty()) {
        result.succeeded = true;
        return result;
    }

    phase.restart();
    QRhiCommandBuffer *commandBuffer = nullptr;
    if (m_rhi->beginOffscreenFrame(&commandBuffer) != QRhi::FrameOpSuccess) {
        result.error = QStringLiteral("Could not begin color-stroke offscreen frame");
        return result;
    }
    const QRhiTexture::Format format = QRhiTexture::RGBA16F;
    if (!m_rhi->isTextureFormatSupported(format, QRhiTexture::RenderTarget)) {
        m_rhi->endOffscreenFrame();
        result.error = QStringLiteral("RGBA16F color-stroke render target is unavailable");
        return result;
    }

    const float quad[] = {-1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1};
    std::unique_ptr<QRhiBuffer> vertexBuffer(m_rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
    vertexBuffer->create();
    std::unique_ptr<QRhiBuffer> instanceBuffer(m_rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
        qMax(1, result.stampInstances) * int(sizeof(ColorInstanceData))));
    instanceBuffer->create();
    std::unique_ptr<QRhiBuffer> uniformBuffer(m_rhi->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 80));
    uniformBuffer->create();
    // The flip state is part of the texture identity: a flipped upload must
    // never be served for the unflipped brush (or vice versa).
    const quint64 tipKey = brush.hasCustomShape()
        ? (quint64(brush.customShape().cacheKey())
           ^ (brush.tipFlipX() ? 0x8000000000000000ULL : 0)
           ^ (brush.tipFlipY() ? 0x4000000000000000ULL : 0))
        : 1;
    QRhiTexture *tipTextureGpu = m_tipTexturesGpu.value(tipKey, nullptr);
    const bool uploadTip = !tipTextureGpu;
    QImage tip;
    if (uploadTip) {
        tip = tipTexture(brush);
        tipTextureGpu = m_rhi->newTexture(QRhiTexture::RGBA8, tip.size());
        if (!tipTextureGpu->create()) {
            delete tipTextureGpu;
            m_rhi->endOffscreenFrame();
            result.error = QStringLiteral("Could not create GPU color brush-tip texture");
            return result;
        }
        m_tipTexturesGpu.insert(tipKey, tipTextureGpu);
    }
    QRhiSampler *dynamicsSampler = linearRepeatSampler();
    const QImage grain = grainTexture(brush);
    const quint64 grainKey = !brush.hasGrain()
        ? 1 : quint64(brush.grainTexture().cacheKey());
    QRhiTexture *grainTextureGpu = m_grainTexturesGpu.value(grainKey, nullptr);
    const bool uploadGrain = !grainTextureGpu;
    if (uploadGrain) {
        grainTextureGpu = m_rhi->newTexture(QRhiTexture::RGBA8, grain.size());
        grainTextureGpu->create();
        m_grainTexturesGpu.insert(grainKey, grainTextureGpu);
        result.grainTextureUploads = brush.hasGrain() ? 1 : 0;
    }
    result.grainTextureBytes = brush.hasGrain() ? grain.sizeInBytes() : 0;
    std::unique_ptr<QRhiShaderResourceBindings> bindings(m_rhi->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniformBuffer.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            tipTextureGpu, dynamicsSampler),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage,
            grainTextureGpu, dynamicsSampler)});
    bindings->create();

    struct TileResources {
        QPoint coordinate;
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiRenderPassDescriptor> pass;
        std::unique_ptr<QRhiReadbackResult> readback;
        std::unique_ptr<QRhiTexture> publicationBase;
        QImage publicationBasePixels;
        std::unique_ptr<QRhiTexture> publicationTexture;
        std::unique_ptr<QRhiTextureRenderTarget> publicationTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> publicationPass;
        std::unique_ptr<QRhiShaderResourceBindings> publicationBindings;
        quint32 offset = 0;
        quint32 count = 0;
    };
    std::vector<std::unique_ptr<TileResources>> resources;
    QVector<ColorInstanceData> flattened;
    flattened.reserve(result.stampInstances);
    for (auto it = tileInstances.cbegin(); it != tileInstances.cend(); ++it) {
        auto tile = std::make_unique<TileResources>();
        tile->coordinate = it.key();
        tile->offset = flattened.size() * sizeof(ColorInstanceData);
        tile->count = it.value().size();
        flattened.append(it.value());
        tile->texture.reset(m_rhi->newTexture(
            format, QSize(TiledImage::TileSize, TiledImage::TileSize), 1,
            QRhiTexture::RenderTarget));
        tile->texture->create();
        tile->target.reset(m_rhi->newTextureRenderTarget(
            QRhiTextureRenderTargetDescription(QRhiColorAttachment(tile->texture.get()))));
        tile->pass.reset(tile->target->newCompatibleRenderPassDescriptor());
        tile->target->setRenderPassDescriptor(tile->pass.get());
        tile->target->create();
        resources.push_back(std::move(tile));
    }

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(m_rhi->newGraphicsPipeline());
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/shaders/color_stamp.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/shaders/color_stamp.frag.qsb"))}
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)},
                        {sizeof(ColorInstanceData), QRhiVertexInputBinding::PerInstance}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                          {1, 1, QRhiVertexInputAttribute::Float4, 0},
                          {1, 2, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
                          {1, 3, QRhiVertexInputAttribute::Float4, 8 * sizeof(float)},
                          {1, 4, QRhiVertexInputAttribute::Float4, 12 * sizeof(float)},
                          {1, 5, QRhiVertexInputAttribute::Float4, 16 * sizeof(float)}});
    pipeline->setVertexInputLayout(layout);
    pipeline->setShaderResourceBindings(bindings.get());
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.opColor = QRhiGraphicsPipeline::Add;
    blend.opAlpha = QRhiGraphicsPipeline::Add;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    pipeline->setTargetBlends({blend});
    pipeline->setRenderPassDescriptor(resources.front()->pass.get());
    if (!pipeline->create()) {
        m_rhi->endOffscreenFrame();
        result.error = QStringLiteral("Could not create color-stroke pipeline");
        return result;
    }
    resourceNs += phase.nsecsElapsed();

    phase.restart();
    QMatrix4x4 clip = m_rhi->clipSpaceCorrMatrix();
    QByteArray uniforms(80, 0);
    memcpy(uniforms.data(), clip.constData(), 64);
    *reinterpret_cast<qint32 *>(uniforms.data() + 64) = brush.hasCustomShape() ? 1 : 0;
    QRhiResourceUpdateBatch *updates = m_rhi->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vertexBuffer.get(), quad);
    updates->uploadStaticBuffer(instanceBuffer.get(), flattened.constData());
    if (uploadTip)
        updates->uploadTexture(tipTextureGpu, tip);
    if (uploadGrain)
        updates->uploadTexture(grainTextureGpu, grain);
    updates->updateDynamicBuffer(uniformBuffer.get(), 0, uniforms.size(), uniforms.constData());
    commandBuffer->resourceUpdate(updates);
    instanceNs += phase.nsecsElapsed();
    result.resourcePipelineMs = resourceNs / 1000000.0;
    result.instanceBuildUploadMs = instanceNs / 1000000.0;
    result.baseTipUploads = uploadTip ? 1 : 0;

    phase.restart();
    for (const auto &resource : resources) {
        commandBuffer->beginPass(resource->target.get(), Qt::transparent, {1.0f, 0});
        commandBuffer->setGraphicsPipeline(pipeline.get());
        commandBuffer->setViewport({0, 0, float(TiledImage::TileSize), float(TiledImage::TileSize)});
        commandBuffer->setShaderResources();
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {vertexBuffer.get(), 0}, {instanceBuffer.get(), resource->offset}
        };
        commandBuffer->setVertexInput(0, 2, inputs);
        commandBuffer->draw(6, resource->count);
        commandBuffer->endPass();
        ++result.instancedDrawCalls;
    }
    result.renderSubmissionMs = phase.nsecsElapsed() / 1000000.0;

    const bool colorPublication = baseRegion != nullptr;
    QRhiSampler *publicationSampler = nullptr;
    std::unique_ptr<QRhiBuffer> publicationUniformBuffer;
    std::unique_ptr<QRhiGraphicsPipeline> publicationPipeline;
    if (colorPublication) {
        phase.restart();
        publicationUniformBuffer.reset(m_rhi->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 96));
        publicationUniformBuffer->create();
        publicationSampler = nearestClampSampler();
        for (auto &resource : resources) {
            resource->publicationBase.reset(m_rhi->newTexture(
                QRhiTexture::RGBA8, QSize(TiledImage::TileSize, TiledImage::TileSize)));
            resource->publicationBase->create();
            resource->publicationTexture.reset(m_rhi->newTexture(
                QRhiTexture::RGBA8, QSize(TiledImage::TileSize, TiledImage::TileSize), 1,
                QRhiTexture::RenderTarget));
            resource->publicationTexture->create();
            resource->publicationTarget.reset(m_rhi->newTextureRenderTarget(
                QRhiTextureRenderTargetDescription(
                    QRhiColorAttachment(resource->publicationTexture.get()))));
            resource->publicationPass.reset(
                resource->publicationTarget->newCompatibleRenderPassDescriptor());
            resource->publicationTarget->setRenderPassDescriptor(
                resource->publicationPass.get());
            resource->publicationTarget->create();
            resource->publicationBindings.reset(m_rhi->newShaderResourceBindings());
            resource->publicationBindings->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::VertexStage
                        | QRhiShaderResourceBinding::FragmentStage,
                    publicationUniformBuffer.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage,
                    resource->publicationBase.get(), publicationSampler),
                QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage,
                    resource->texture.get(), publicationSampler)
            });
            resource->publicationBindings->create();
        }
        publicationPipeline.reset(m_rhi->newGraphicsPipeline());
        publicationPipeline->setShaderStages({
            {QRhiShaderStage::Vertex,
             loadShader(QStringLiteral(":/shaders/shaders/publish.vert.qsb"))},
            {QRhiShaderStage::Fragment,
             loadShader(QStringLiteral(":/shaders/shaders/publish.frag.qsb"))}
        });
        QRhiVertexInputLayout publicationLayout;
        publicationLayout.setBindings({{2 * sizeof(float)}});
        publicationLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        publicationPipeline->setVertexInputLayout(publicationLayout);
        publicationPipeline->setShaderResourceBindings(
            resources.front()->publicationBindings.get());
        publicationPipeline->setRenderPassDescriptor(
            resources.front()->publicationPass.get());
        if (!publicationPipeline->create()) {
            m_rhi->endOffscreenFrame();
            result.error = QStringLiteral("Could not create color publication pipeline");
            return result;
        }
        result.resourcePipelineMs += phase.nsecsElapsed() / 1000000.0;

        phase.restart();
        QRhiResourceUpdateBatch *publicationUpdates = m_rhi->nextResourceUpdateBatch();
        for (auto &resource : resources) {
            resource->publicationBasePixels = publicationBaseTile(
                *baseRegion, regionCanvasOrigin, resource->coordinate);
            publicationUpdates->uploadTexture(
                resource->publicationBase.get(), resource->publicationBasePixels);
        }
        QByteArray publicationUniforms(96, 0);
        memcpy(publicationUniforms.data(), clip.constData(), 64);
        const QColor brushColor = brush.color();
        float *color = reinterpret_cast<float *>(publicationUniforms.data() + 64);
        color[0] = float(brushColor.redF());
        color[1] = float(brushColor.greenF());
        color[2] = float(brushColor.blueF());
        color[3] = float(brush.opacity() * (brush.smudgeActive()
            ? 1.0 : brushColor.alphaF()));
        *reinterpret_cast<qint32 *>(publicationUniforms.data() + 80) =
            m_rhi->backend() == QRhi::D3D11 ? 1 : 2;
        publicationUpdates->updateDynamicBuffer(
            publicationUniformBuffer.get(), 0, publicationUniforms.size(),
            publicationUniforms.constData());
        commandBuffer->resourceUpdate(publicationUpdates);
        for (auto &resource : resources) {
            commandBuffer->beginPass(resource->publicationTarget.get(), Qt::transparent, {1.0f, 0});
            commandBuffer->setGraphicsPipeline(publicationPipeline.get());
            commandBuffer->setViewport(
                {0, 0, float(TiledImage::TileSize), float(TiledImage::TileSize)});
            commandBuffer->setShaderResources(resource->publicationBindings.get());
            const QRhiCommandBuffer::VertexInput input(vertexBuffer.get(), 0);
            commandBuffer->setVertexInput(0, 1, &input);
            commandBuffer->draw(6);
            commandBuffer->endPass();
            ++result.publicationDrawCalls;
        }
        result.publicationSubmissionMs = phase.nsecsElapsed() / 1000000.0;
        result.compositedOnGpu = true;
    }

    phase.restart();
    if (!deferredReadback) {
        QRhiResourceUpdateBatch *readbackBatch = m_rhi->nextResourceUpdateBatch();
        for (const auto &resource : resources) {
            resource->readback = std::make_unique<QRhiReadbackResult>();
            const bool mirror = !m_rhi->isYUpInFramebuffer();
            resource->readback->completed = [&result, readback = resource->readback.get(),
                coordinate = resource->coordinate, mirror,
                colorPublication, opacity = brush.opacity()] {
                QElapsedTimer conversion;
                conversion.start();
                result.masks.insert(coordinate, colorPublication
                    ? rgbaReadbackToArgb(readback->data, readback->pixelSize, mirror)
                    : colorReadbackToRgba(readback->data, readback->pixelSize,
                                          mirror, opacity));
                result.readbackConversionMs += conversion.nsecsElapsed() / 1000000.0;
            };
            readbackBatch->readBackTexture(
                QRhiReadbackDescription(colorPublication
                    ? resource->publicationTexture.get() : resource->texture.get()),
                resource->readback.get());
        }
        commandBuffer->resourceUpdate(readbackBatch);
        result.readbackBatches = 1;
        result.readbackBytes = qint64(resources.size()) * TiledImage::TileSize
            * TiledImage::TileSize * (colorPublication ? 4 : 8);
        if (colorPublication)
            result.publicationReadbackBytes = result.readbackBytes;
        else
            result.strokeBufferReadbackBytes = result.readbackBytes;
    } else {
        result.cpuBackingCoherent = false;
    }
    result.readbackSubmissionMs = phase.nsecsElapsed() / 1000000.0;
    phase.restart();
    const QRhi::FrameOpResult endResult = m_rhi->endOffscreenFrame();
    const double waitIncludingCallbacks = phase.nsecsElapsed() / 1000000.0;
    result.finalGpuWaitMs = qMax(0.0, waitIncludingCallbacks - result.readbackConversionMs);
    result.gpuFrameMs = commandBuffer->lastCompletedGpuTime() * 1000.0;
    result.succeeded = endResult == QRhi::FrameOpSuccess
        && (deferredReadback || result.masks.size() == tileInstances.size());
    for (auto it = tileInstances.cbegin(); it != tileInstances.cend(); ++it)
        result.dirtyTiles.insert(it.key());
    if (colorPublication && !deferredReadback)
        result.preparedLayer = publicationRegion(result.masks, *baseRegion, regionCanvasOrigin);
    if (!result.succeeded)
        result.error = endResult == QRhi::FrameOpSuccess
            ? QStringLiteral("Color-stroke GPU tile readback did not complete")
            : QStringLiteral("Color GPU offscreen frame submission failed (%1)")
                .arg(int(endResult));
    result.totalPreciseMs = elapsed.nsecsElapsed() / 1000000.0;
    result.elapsedMs = elapsed.elapsed();
    const double accounted = result.resourcePipelineMs + result.instanceBuildUploadMs
        + result.renderSubmissionMs + result.publicationSubmissionMs
        + result.readbackSubmissionMs + result.readbackConversionMs + result.finalGpuWaitMs;
    result.unattributedCpuMs = qMax(0.0, result.totalPreciseMs - accounted);
    return result;
}

int GpuStampRenderer::uploadLayerTiles(const TiledImage &layer, const QSet<QPoint> &dirtyTiles)
{
    if (!m_rhi || dirtyTiles.isEmpty())
        return 0;
    QRhiCommandBuffer *commandBuffer = nullptr;
    if (m_rhi->beginOffscreenFrame(&commandBuffer) != QRhi::FrameOpSuccess)
        return 0;
    QRhiResourceUpdateBatch *updates = m_rhi->nextResourceUpdateBatch();
    int uploaded = 0;
    for (const QPoint &coordinate : dirtyTiles) {
        const QImage *cpuTile = layer.tile(coordinate);
        if (!cpuTile) {
            delete m_layerTextures.take(coordinate);
            continue;
        }
        QRhiTexture *texture = m_layerTextures.value(coordinate, nullptr);
        if (!texture) {
            texture = m_rhi->newTexture(QRhiTexture::RGBA8,
                                         QSize(TiledImage::TileSize, TiledImage::TileSize), 1,
                                         QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips);
            if (!texture->create()) {
                delete texture;
                continue;
            }
            m_layerTextures.insert(coordinate, texture);
        }
        updates->uploadTexture(texture, cpuTile->convertToFormat(QImage::Format_RGBA8888));
        updates->generateMips(texture);
        ++uploaded;
    }
    commandBuffer->resourceUpdate(updates);
    m_rhi->endOffscreenFrame();
    return uploaded;
}
