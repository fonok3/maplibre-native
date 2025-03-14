#include <mbgl/renderer/layers/render_hillshade_layer.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/sources/render_raster_dem_source.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/programs/programs.hpp>
#include <mbgl/programs/hillshade_program.hpp>
#include <mbgl/programs/hillshade_prepare_program.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/style/layers/hillshade_layer_impl.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/math/angles.hpp>
#include <mbgl/util/geo.hpp>

namespace mbgl {

using namespace style;

namespace {

inline const HillshadeLayer::Impl& impl_cast(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == HillshadeLayer::Impl::staticTypeInfo());
    return static_cast<const HillshadeLayer::Impl&>(*impl);
}

} // namespace

RenderHillshadeLayer::RenderHillshadeLayer(Immutable<style::HillshadeLayer::Impl> _impl)
    : RenderLayer(makeMutable<HillshadeLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()) {}

RenderHillshadeLayer::~RenderHillshadeLayer() = default;

std::array<float, 2> RenderHillshadeLayer::getLatRange(const UnwrappedTileID& id) {
    const LatLng latlng0 = LatLng(id);
    const LatLng latlng1 = LatLng(UnwrappedTileID(id.canonical.z, id.canonical.x, id.canonical.y + 1));
    return {{static_cast<float>(latlng0.latitude()), static_cast<float>(latlng1.latitude())}};
}

std::array<float, 2> RenderHillshadeLayer::getLight(const PaintParameters& parameters) {
    const auto& evaluated = static_cast<const HillshadeLayerProperties&>(*evaluatedProperties).evaluated;
    float azimuthal = util::deg2radf(evaluated.get<HillshadeIlluminationDirection>());
    if (evaluated.get<HillshadeIlluminationAnchor>() == HillshadeIlluminationAnchorType::Viewport)
        azimuthal = azimuthal - static_cast<float>(parameters.state.getBearing());
    return {{evaluated.get<HillshadeExaggeration>(), azimuthal}};
}

void RenderHillshadeLayer::transition(const TransitionParameters& parameters) {
    unevaluated = impl_cast(baseImpl).paint.transitioned(parameters, std::move(unevaluated));
}

void RenderHillshadeLayer::evaluate(const PropertyEvaluationParameters& parameters) {
    auto properties = makeMutable<HillshadeLayerProperties>(staticImmutableCast<HillshadeLayer::Impl>(baseImpl),
                                                            unevaluated.evaluate(parameters));
    passes = (properties->evaluated.get<style::HillshadeExaggeration>() > 0)
                 ? (RenderPass::Translucent | RenderPass::Pass3D)
                 : RenderPass::None;
    properties->renderPasses = mbgl::underlying_type(passes);
    evaluatedProperties = std::move(properties);
}

bool RenderHillshadeLayer::hasTransition() const {
    return unevaluated.hasTransition();
}

bool RenderHillshadeLayer::hasCrossfade() const {
    return false;
}

void RenderHillshadeLayer::prepare(const LayerPrepareParameters& params) {
    renderTiles = params.source->getRenderTiles();
    maxzoom = params.source->getMaxZoom();
}

void RenderHillshadeLayer::render(PaintParameters& parameters) {
    assert(renderTiles);
    if (parameters.pass != RenderPass::Translucent && parameters.pass != RenderPass::Pass3D) return;

    if (!parameters.shaders.populate(hillshadeProgram)) return;
    if (!parameters.shaders.populate(hillshadePrepareProgram)) return;

    const auto& evaluated = static_cast<const HillshadeLayerProperties&>(*evaluatedProperties).evaluated;
    auto draw = [&](const mat4& matrix,
                    const auto& vertexBuffer,
                    const auto& indexBuffer,
                    const auto& segments,
                    const UnwrappedTileID& id,
                    const auto& textureBindings) {
        const HillshadeProgram::Binders paintAttributeData{evaluated, 0};

        const auto allUniformValues = HillshadeProgram::computeAllUniformValues(
            HillshadeProgram::LayoutUniformValues{
                uniforms::matrix::Value(matrix),
                uniforms::highlight::Value(evaluated.get<HillshadeHighlightColor>()),
                uniforms::shadow::Value(evaluated.get<HillshadeShadowColor>()),
                uniforms::accent::Value(evaluated.get<HillshadeAccentColor>()),
                uniforms::light::Value(getLight(parameters)),
                uniforms::latrange::Value(getLatRange(id)),
            },
            paintAttributeData,
            evaluated,
            static_cast<float>(parameters.state.getZoom()));
        const auto allAttributeBindings = HillshadeProgram::computeAllAttributeBindings(
            vertexBuffer, paintAttributeData, evaluated);

        checkRenderability(parameters, HillshadeProgram::activeBindingCount(allAttributeBindings));

        hillshadeProgram->draw(parameters.context,
                               *parameters.renderPass,
                               gfx::Triangles(),
                               parameters.depthModeForSublayer(0, gfx::DepthMaskType::ReadOnly),
                               gfx::StencilMode::disabled(),
                               parameters.colorModeForRenderPass(),
                               gfx::CullFaceMode::disabled(),
                               indexBuffer,
                               segments,
                               allUniformValues,
                               allAttributeBindings,
                               textureBindings,
                               getID());
    };

    mat4 mat;
    matrix::ortho(mat, 0, util::EXTENT, -util::EXTENT, 0, 0, 1);
    matrix::translate(mat, mat, 0, -util::EXTENT, 0);

    for (const RenderTile& tile : *renderTiles) {
        auto* bucket_ = tile.getBucket(*baseImpl);
        if (!bucket_) {
            continue;
        }
        auto& bucket = static_cast<HillshadeBucket&>(*bucket_);

        if (!bucket.hasData()) {
            continue;
        }

        if (!bucket.isPrepared() && parameters.pass == RenderPass::Pass3D) {
            assert(bucket.dem);
            const uint16_t stride = bucket.getDEMData().stride;
            const uint16_t tilesize = bucket.getDEMData().dim;
            auto view = parameters.context.createOffscreenTexture({tilesize, tilesize},
                                                                  gfx::TextureChannelDataType::UnsignedByte);

            auto renderPass = parameters.encoder->createRenderPass("hillshade prepare",
                                                                   {*view, Color{0.0f, 0.0f, 0.0f, 0.0f}, {}, {}});

            const Properties<>::PossiblyEvaluated properties;
            const HillshadePrepareProgram::Binders paintAttributeData{properties, 0};

            const auto allUniformValues = HillshadePrepareProgram::computeAllUniformValues(
                HillshadePrepareProgram::LayoutUniformValues{
                    uniforms::matrix::Value(mat),
                    uniforms::dimension::Value({{stride, stride}}),
                    uniforms::zoom::Value(static_cast<float>(tile.id.canonical.z)),
                    uniforms::maxzoom::Value(static_cast<float>(maxzoom)),
                    uniforms::unpack::Value(bucket.getDEMData().getUnpackVector()),
                },
                paintAttributeData,
                properties,
                static_cast<float>(parameters.state.getZoom()));
            const auto allAttributeBindings = HillshadePrepareProgram::computeAllAttributeBindings(
                *parameters.staticData.rasterVertexBuffer, paintAttributeData, properties);

            checkRenderability(parameters, HillshadePrepareProgram::activeBindingCount(allAttributeBindings));

            // Copy over the segments so that we can create our own DrawScopes
            // that get destroyed after this draw call.
            auto segments = RenderStaticData::rasterSegments();
            hillshadePrepareProgram->draw(parameters.context,
                                          *renderPass,
                                          gfx::Triangles(),
                                          parameters.depthModeForSublayer(0, gfx::DepthMaskType::ReadOnly),
                                          gfx::StencilMode::disabled(),
                                          parameters.colorModeForRenderPass(),
                                          gfx::CullFaceMode::disabled(),
                                          *parameters.staticData.quadTriangleIndexBuffer,
                                          segments,
                                          allUniformValues,
                                          allAttributeBindings,
                                          HillshadePrepareProgram::TextureBindings{
                                              textures::image::Value{bucket.dem->getResource()},
                                          },
                                          "prepare");
            bucket.texture = std::move(view->getTexture());
            bucket.setPrepared(true);
        } else if (parameters.pass == RenderPass::Translucent) {
            assert(bucket.texture);

            if (bucket.vertexBuffer && bucket.indexBuffer) {
                // Draw only the parts of the tile that aren't drawn by another tile in the layer.
                draw(parameters.matrixForTile(tile.id, true),
                     *bucket.vertexBuffer,
                     *bucket.indexBuffer,
                     bucket.segments,
                     tile.id,
                     HillshadeProgram::TextureBindings{
                         textures::image::Value{bucket.texture->getResource(), gfx::TextureFilterType::Linear},
                     });
            } else {
                // Draw the full tile.
                if (bucket.segments.empty()) {
                    // Copy over the segments so that we can create our own DrawScopes.
                    bucket.segments = RenderStaticData::rasterSegments();
                }
                draw(parameters.matrixForTile(tile.id, true),
                     *parameters.staticData.rasterVertexBuffer,
                     *parameters.staticData.quadTriangleIndexBuffer,
                     bucket.segments,
                     tile.id,
                     HillshadeProgram::TextureBindings{
                         textures::image::Value{bucket.texture->getResource(), gfx::TextureFilterType::Linear},
                     });
            }
        }
    }
}

} // namespace mbgl
