/*
 * Copyright 2023 Rive
 */

#pragma once

#include "rive/math/raw_path.hpp"
#include "rive/math/wangs_formula.hpp"
#include "rive/pls/pls.hpp"
#include "rive/pls/fixed_queue.hpp"
#include "rive/shapes/paint/stroke_cap.hpp"
#include "rive/shapes/paint/stroke_join.hpp"
#include "rive/refcnt.hpp"

namespace rive::pls
{
class PLSDraw;
class PLSPath;
class PLSPaint;
class PLSRenderContext;
class PLSGradient;

// High level abstraction of a single object to be drawn (path, imageRect, or imageMesh). These get
// built up for an entire frame in order to count GPU resource allocation sizes, and then sorted,
// batched, and drawn.
class PLSDraw
{
public:
    // Use a "fullscreen" bounding box that is reasonably larger than any screen, but not so big
    // that it runs the risk of overflowing.
    constexpr static IAABB kFullscreenPixelBounds = {0, 0, 1 << 24, 1 << 24};

    enum class Type : uint8_t
    {
        midpointFanPath,
        interiorTriangulationPath,
        imageRect,
        imageMesh
    };

    PLSDraw(IAABB pixelBounds, const Mat2D&, BlendMode, rcp<const PLSTexture> imageTexture, Type);

    const IAABB& pixelBounds() const { return m_pixelBounds; }
    const PLSTexture* imageTexture() const { return m_imageTextureRef; }
    Type type() const { return m_type; }
    bool hasClipRect() const { return m_clipRectInverseMatrix != nullptr; }

    // Clipping setup.
    void setClipID(uint32_t clipID) { m_clipID = clipID; }
    void setClipRect(const pls::ClipRectInverseMatrix* m) { m_clipRectInverseMatrix = m; }

    // Running counts of objects that need to be allocated in the render context's various GPU
    // buffers.
    struct ResourceCounters
    {
        using VecType = simd::gvec<size_t, 8>;

        VecType toVec() const
        {
            static_assert(sizeof(VecType) == sizeof(*this));
            VecType vec;
            RIVE_INLINE_MEMCPY(&vec, this, sizeof(VecType));
            return vec;
        }

        ResourceCounters(const VecType& vec)
        {
            static_assert(sizeof(*this) == sizeof(VecType));
            RIVE_INLINE_MEMCPY(this, &vec, sizeof(*this));
        }

        ResourceCounters() = default;

        size_t midpointFanTessVertexCount = 0;
        size_t outerCubicTessVertexCount = 0;
        size_t pathCount = 0;
        size_t contourCount = 0;
        size_t tessellatedSegmentCount = 0; // lines, curves, standalone joins, emulated caps, etc.
        size_t maxTriangleVertexCount = 0;
        size_t imageDrawCount = 0; // imageRect or imageMesh.
        size_t complexGradientSpanCount = 0;
    };

    // Used to allocate GPU resources for a collection of draws.
    const ResourceCounters& resourceCounts() const { return m_resourceCounts; }

    // Adds the gradient (if any) for this draw to the render context's gradient texture.
    // Returns false if this draw needed a gradient but there wasn't room for it in the texture, at
    // which point the gradient texture will need to be re-rendered mid flight.
    bool allocateGradientIfNeeded(PLSRenderContext*, ResourceCounters*);

    // Pushes the data for this draw to the render context. Called once the GPU buffers have been
    // counted and allocated, and the draws have been sorted.
    virtual void pushToRenderContext(PLSRenderContext*) = 0;

    // We can't have a destructor because we're block-allocated. Instead, the client calls this
    // method before clearing the drawList to release all our held references.
    virtual void releaseRefs();

protected:
    const PLSTexture* const m_imageTextureRef;
    const IAABB m_pixelBounds;
    const Mat2D m_matrix;
    const BlendMode m_blendMode;
    const Type m_type;

    uint32_t m_clipID = 0;
    const pls::ClipRectInverseMatrix* m_clipRectInverseMatrix = nullptr;

    // Filled in by the subclass constructor.
    ResourceCounters m_resourceCounts;

    // Gradient data used by some draws. Stored in the base class so allocateGradientIfNeeded()
    // doesn't have to be virtual.
    const PLSGradient* m_gradientRef = nullptr;
    pls::SimplePaintValue m_simplePaintValue;
};

// Even though PLSDraw is block-allocated, we sill need to call releaseRefs() on each individual
// indstance before releasing the block. This smart pointer guarantees we always call releaseRefs().
struct PLSDrawReleaseRefs
{
    void operator()(PLSDraw* draw) { draw->releaseRefs(); }
};
using PLSDrawUniquePtr = std::unique_ptr<PLSDraw, PLSDrawReleaseRefs>;

// High level abstraction of a single path to be drawn (midpoint fan or interior triangulation).
class PLSPathDraw : public PLSDraw
{
public:
    // Creates either a normal path draw or an interior triangulation if the path is large enough.
    static PLSDrawUniquePtr Make(PLSRenderContext*,
                                 const Mat2D&,
                                 rcp<const PLSPath>,
                                 FillRule,
                                 const PLSPaint*,
                                 RawPath* scratchPath);

    void pushToRenderContext(PLSRenderContext*) final;

    void releaseRefs() override;

public:
    PLSPathDraw(IAABB pathBounds,
                const Mat2D&,
                rcp<const PLSPath>,
                FillRule,
                const PLSPaint*,
                Type);

    virtual void onPushToRenderContext(PLSRenderContext*) = 0;

    const PLSPath* const m_pathRef;
    const bool m_isStroked;
    const FillRule m_fillRule; // Bc PLSPath fillRule can mutate during the artboard draw process.
    const pls::PaintType m_paintType;
    const float m_strokeRadius;

    // Used to guarantee m_pathRef doesn't change for the entire time we hold it.
    RIVE_DEBUG_CODE(size_t m_rawPathMutationID;)
};

// Draws a path by fanning tessellation patches around the midpoint of each contour.
class MidpointFanPathDraw : public PLSPathDraw
{
public:
    MidpointFanPathDraw(PLSRenderContext*,
                        IAABB pixelBounds,
                        const Mat2D&,
                        rcp<const PLSPath>,
                        FillRule,
                        const PLSPaint*);

protected:
    void onPushToRenderContext(PLSRenderContext*) override;

    // Emulates a stroke cap before the given cubic by pushing a copy of the cubic, reversed, with 0
    // tessellation segments leading up to the join section, and a 180-degree join that looks like
    // the desired stroke cap.
    void pushEmulatedStrokeCapAsJoinBeforeCubic(PLSRenderContext*,
                                                const Vec2D cubic[],
                                                uint32_t emulatedCapAsJoinFlags,
                                                uint32_t strokeCapSegmentCount);

    float m_strokeMatrixMaxScale;
    StrokeJoin m_strokeJoin;
    StrokeCap m_strokeCap;

    struct ContourInfo
    {
        RawPath::Iter endOfContour;
        size_t endLineIdx;
        size_t firstCurveIdx;
        size_t endCurveIdx;
        size_t firstRotationIdx; // We measure rotations on both curves and round joins.
        size_t endRotationIdx;
        Vec2D midpoint;
        bool closed;
        size_t strokeJoinCount;
        uint32_t strokeCapSegmentCount;
        uint32_t paddingVertexCount;
        RIVE_DEBUG_CODE(uint32_t tessVertexCount;)
    };

    ContourInfo* m_contours;
    FixedQueue<uint8_t> m_numChops;
    FixedQueue<Vec2D> m_chopVertices;
    std::array<Vec2D, 2>* m_tangentPairs = nullptr;
    uint32_t* m_polarSegmentCounts = nullptr;
    uint32_t* m_parametricSegmentCounts = nullptr;

    // Consistency checks for onPushToRenderContext().
    RIVE_DEBUG_CODE(size_t m_pendingLineCount;)
    RIVE_DEBUG_CODE(size_t m_pendingCurveCount;)
    RIVE_DEBUG_CODE(size_t m_pendingRotationCount;)
    RIVE_DEBUG_CODE(size_t m_pendingStrokeJoinCount;)
    RIVE_DEBUG_CODE(size_t m_pendingStrokeCapCount;)
    // Counts how many additional curves were pushed by pushEmulatedStrokeCapAsJoinBeforeCubic().
    RIVE_DEBUG_CODE(size_t m_pendingEmptyStrokeCountForCaps;)
};

// Draws a path by triangulating the interior into non-overlapping triangles and tessellating the
// outer curves.
class InteriorTriangulationDraw : public PLSPathDraw
{
public:
    enum class TriangulatorAxis
    {
        horizontal,
        vertical,
        dontCare,
    };

    InteriorTriangulationDraw(PLSRenderContext*,
                              IAABB pixelBounds,
                              const Mat2D&,
                              rcp<const PLSPath>,
                              FillRule,
                              const PLSPaint*,
                              RawPath* scratchPath,
                              TriangulatorAxis);

protected:
    void onPushToRenderContext(PLSRenderContext*) override;

    // The final segment in an outerCurve patch is a bowtie join.
    constexpr static size_t kJoinSegmentCount = 1;
    constexpr static size_t kPatchSegmentCountExcludingJoin =
        kOuterCurvePatchSegmentSpan - kJoinSegmentCount;

    // Maximum # of outerCurve patches a curve on the path can be subdivided into.
    constexpr static size_t kMaxCurveSubdivisions =
        (kMaxParametricSegments + kPatchSegmentCountExcludingJoin - 1) /
        kPatchSegmentCountExcludingJoin;

    static size_t FindSubdivisionCount(const Vec2D pts[],
                                       const wangs_formula::VectorXform& vectorXform)
    {
        size_t numSubdivisions =
            ceilf(wangs_formula::cubic(pts, kParametricPrecision, vectorXform) *
                  (1.f / kPatchSegmentCountExcludingJoin));
        return std::clamp<size_t>(numSubdivisions, 1, kMaxCurveSubdivisions);
    }

    enum class PathOp : bool
    {
        countDataAndTriangulate,
        submitOuterCubics,
    };

    // For now, we just iterate and subdivide the path twice (once for each enum in PathOp).
    // Since we only do this for large paths, and since we're triangulating the path interior
    // anyway, adding complexity to only run Wang's formula and chop once would save about ~5%
    // of the total CPU time. (And large paths are GPU-bound anyway.)
    void processPath(PLSRenderContext* context,
                     PathOp op,
                     RawPath* scratchPath = nullptr,
                     TriangulatorAxis = TriangulatorAxis::dontCare);

    GrInnerFanTriangulator* m_triangulator = nullptr;
};

// Pushes an imageRect to the render context.
// This should only be used when we don't have bindless textures in atomic mode. Otherwise, images
// should be drawn as rectangular paths with an image paint.
class ImageRectDraw : public PLSDraw
{
public:
    ImageRectDraw(PLSRenderContext*,
                  IAABB pixelBounds,
                  const Mat2D&,
                  BlendMode,
                  rcp<const PLSTexture>,
                  float opacity);

    void pushToRenderContext(PLSRenderContext*) override;

protected:
    const float m_opacity;
};

// Pushes an imageMesh to the render context.
class ImageMeshDraw : public PLSDraw
{
public:
    ImageMeshDraw(IAABB pixelBounds,
                  const Mat2D&,
                  BlendMode,
                  rcp<const PLSTexture>,
                  rcp<const RenderBuffer> vertexBuffer,
                  rcp<const RenderBuffer> uvBuffer,
                  rcp<const RenderBuffer> indexBuffer,
                  uint32_t indexCount,
                  float opacity);

    void pushToRenderContext(PLSRenderContext*) override;

    void releaseRefs() override;

protected:
    const RenderBuffer* const m_vertexBufferRef;
    const RenderBuffer* const m_uvBufferRef;
    const RenderBuffer* const m_indexBufferRef;
    const uint32_t m_indexCount;
    const float m_opacity;
};
} // namespace rive::pls
