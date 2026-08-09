// Compatibility implementation of miahmie's shrinkCopy plugin.
// Original source: https://github.com/wtnbgo/shrinkCopy
// The upstream plugin follows the Kirikiri engine license.

#define NCB_MODULE_NAME TJS_W("shrinkCopy.dll")

#include "ncbind.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

struct LayerBuffer {
    using Pixel = std::uint32_t;
    using Byte = std::uint8_t;

    static bool IsValidLayer(iTJSDispatch2 *layer) {
        if(!layer ||
           TJS_FAILED(layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"),
                                          layer)))
            return false;

        tTJSVariant value;
        return TJS_SUCCEEDED(layer->PropGet(0, TJS_W("hasImage"), nullptr,
                                            &value, layer)) &&
               value.AsInteger() != 0;
    }

    static bool GetSize(iTJSDispatch2 *layer, tjs_int &width,
                        tjs_int &height, tjs_int &pitch) {
        if(!IsValidLayer(layer))
            return false;

        tTJSVariant value;
        if(TJS_FAILED(layer->PropGet(0, TJS_W("imageWidth"), nullptr, &value,
                                     layer)))
            return false;
        width = static_cast<tjs_int>(value.AsInteger());

        value.Clear();
        if(TJS_FAILED(layer->PropGet(0, TJS_W("imageHeight"), nullptr, &value,
                                     layer)))
            return false;
        height = static_cast<tjs_int>(value.AsInteger());

        value.Clear();
        if(TJS_FAILED(layer->PropGet(0, TJS_W("mainImageBufferPitch"),
                                     nullptr, &value, layer)))
            return false;
        pitch = static_cast<tjs_int>(value.AsInteger());
        return width > 0 && height > 0 && pitch != 0;
    }

    static bool GetForRead(iTJSDispatch2 *layer, tjs_int &width,
                           tjs_int &height, const Byte *&pixels,
                           tjs_int &pitch) {
        if(!GetSize(layer, width, height, pitch))
            return false;

        tTJSVariant value;
        if(TJS_FAILED(layer->PropGet(0, TJS_W("mainImageBuffer"), nullptr,
                                     &value, layer)))
            return false;
        pixels = reinterpret_cast<const Byte *>(
            static_cast<tjs_intptr_t>(value.AsInteger()));
        return pixels != nullptr;
    }

    static bool GetForWrite(iTJSDispatch2 *layer, tjs_int &width,
                            tjs_int &height, Byte *&pixels, tjs_int &pitch) {
        if(!GetSize(layer, width, height, pitch))
            return false;

        tTJSVariant value;
        if(TJS_FAILED(layer->PropGet(0, TJS_W("mainImageBufferForWrite"),
                                     nullptr, &value, layer)))
            return false;
        pixels = reinterpret_cast<Byte *>(
            static_cast<tjs_intptr_t>(value.AsInteger()));
        return pixels != nullptr;
    }
};

class AreaAverageShrink final : private LayerBuffer {
public:
    static tjs_error Invoke(tTJSVariant *, tjs_int numparams,
                            tTJSVariant **param,
                            iTJSDispatch2 *destination) {
        if(numparams < 9)
            return TJS_E_BADPARAMCOUNT;

        AreaAverageShrink operation(
            destination, param[0]->AsReal(), param[1]->AsReal(),
            param[2]->AsReal(), param[3]->AsReal(),
            param[4]->AsObjectNoAddRef(),
            static_cast<tjs_int>(param[5]->AsInteger()),
            static_cast<tjs_int>(param[6]->AsInteger()),
            static_cast<tjs_int>(param[7]->AsInteger()),
            static_cast<tjs_int>(param[8]->AsInteger()));

        if(!operation.Check())
            return TJS_E_INVALIDPARAM;
        if(operation.Clip())
            operation.Copy();
        return TJS_S_OK;
    }

private:
    using Real = tjs_real;
    using Weight = std::uint64_t;

    struct AverageInfo {
        tjs_int offset{};
        tjs_int step{};
        Weight topAlpha{};
        Weight topColor{};
        Weight bottomAlpha{};
        Weight bottomColor{};
        Weight total{};
    };

    struct Sum {
        Weight red{};
        Weight green{};
        Weight blue{};
        Weight alpha{};
    };

    struct AverageWork {
        Weight unit{};
        Real maximum{};
        Real ratio{};
        Real difference{};
        tjs_int sourceOrigin{};
        tjs_int offsetMultiplier{};
    };

    AreaAverageShrink(iTJSDispatch2 *destination, Real dx, Real dy, Real dw,
                      Real dh, iTJSDispatch2 *source, tjs_int sx, tjs_int sy,
                      tjs_int sw, tjs_int sh) :
        destination_(destination), dx_(dx), dy_(dy), dw_(dw), dh_(dh),
        source_(source), sx_(sx), sy_(sy), sw_(sw), sh_(sh) {}

    static tjs_int RealToInt(Real value) {
        return value < 0 ? -static_cast<tjs_int>(-value)
                         : static_cast<tjs_int>(value);
    }

    bool Check() {
        if(sw_ <= 0 || sh_ <= 0 || dw_ <= 0 || dh_ <= 0 || sw_ < dw_ ||
           sh_ < dh_)
            return false;

        return GetForRead(source_, sourceWidth_, sourceHeight_, sourcePixels_,
                          sourcePitch_) &&
               GetForWrite(destination_, destinationWidth_,
                           destinationHeight_, destinationPixels_,
                           destinationPitch_);
    }

    bool Clip() {
        const Real zoomX = dw_ / static_cast<Real>(sw_);
        const Real zoomY = dh_ / static_cast<Real>(sh_);

        if(sx_ + sw_ <= 0 || sy_ + sh_ <= 0 || sx_ >= sourceWidth_ ||
           sy_ >= sourceHeight_)
            return false;

        if(sx_ < 0) {
            sw_ += sx_;
            const Real cut = zoomX * static_cast<Real>(-sx_);
            dw_ -= cut;
            sx_ = 0;
            dx_ += cut;
        }
        if(sy_ < 0) {
            sh_ += sy_;
            const Real cut = zoomY * static_cast<Real>(-sy_);
            dh_ -= cut;
            sy_ = 0;
            dy_ += cut;
        }

        tjs_int cut = sx_ + sw_ - sourceWidth_;
        if(cut > 0) {
            sw_ -= cut;
            dw_ -= zoomX * static_cast<Real>(cut);
        }
        cut = sy_ + sh_ - sourceHeight_;
        if(cut > 0) {
            sh_ -= cut;
            dh_ -= zoomY * static_cast<Real>(cut);
        }

        destinationX_ = RealToInt(dx_);
        destinationY_ = RealToInt(dy_);
        destinationSpanX_ = RealToInt(dx_ + dw_) - destinationX_;
        destinationSpanY_ = RealToInt(dy_ + dh_) - destinationY_;
        if(dx_ + dw_ > static_cast<Real>(destinationX_ + destinationSpanX_))
            ++destinationSpanX_;
        if(dy_ + dh_ > static_cast<Real>(destinationY_ + destinationSpanY_))
            ++destinationSpanY_;

        if(destinationX_ + destinationSpanX_ <= 0 ||
           destinationY_ + destinationSpanY_ <= 0 ||
           destinationX_ >= destinationWidth_ ||
           destinationY_ >= destinationHeight_)
            return false;

        clipStartX_ = destinationX_ < 0 ? -destinationX_ : 0;
        clipStartY_ = destinationY_ < 0 ? -destinationY_ : 0;
        clipEndX_ = destinationX_ + destinationSpanX_ > destinationWidth_
                        ? destinationWidth_ - destinationX_
                        : destinationSpanX_;
        clipEndY_ = destinationY_ + destinationSpanY_ > destinationHeight_
                        ? destinationHeight_ - destinationY_
                        : destinationSpanY_;
        return clipStartX_ < clipEndX_ && clipStartY_ < clipEndY_;
    }

    Weight MakeAverageTable(std::vector<AverageInfo> &table,
                            bool horizontal) const {
        tjs_int position = horizontal ? clipStartX_ : clipStartY_;
        const tjs_int end = (horizontal ? clipEndX_ : clipEndY_) - 1;

        AverageWork work;
        work.maximum = static_cast<Real>(horizontal ? sw_ : sh_);
        work.ratio = work.maximum / (horizontal ? dw_ : dh_);
        work.difference = (horizontal ? dx_ : dy_) -
                          static_cast<Real>(horizontal ? destinationX_
                                                       : destinationY_);
        work.sourceOrigin = horizontal ? sx_ : sy_;
        work.offsetMultiplier = horizontal ? 4 : sourcePitch_;
        work.unit = 256;
        if(work.ratio <= 1.0 / 16.0) {
            work.unit /= static_cast<tjs_int>((2.0 / 16.0) / work.ratio);
            work.unit = std::max<Weight>(work.unit, 1);
        }

        auto output = table.begin();
        if(position == end) {
            SetAverageEdge(work, *output, position);
        } else {
            SetAverageEdge(work, *output++, position++);
            while(position < end)
                SetAverage(work, *output++, position++);
            SetAverageEdge(work, *output, position);
        }
        return work.unit;
    }

    static void SetAverage(const AverageWork &work, AverageInfo &info,
                           tjs_int position) {
        Real first = (static_cast<Real>(position) - work.difference) *
                     work.ratio;
        const Real second = first + work.ratio;
        const tjs_int firstPixel = RealToInt(first);
        const tjs_int secondPixel = RealToInt(second);
        info.offset = (work.sourceOrigin + firstPixel + 1) *
                      work.offsetMultiplier;
        info.topAlpha = info.topColor =
            work.unit - RealToInt((first - firstPixel) * work.unit);
        info.bottomAlpha = info.bottomColor =
            RealToInt((second - secondPixel) * work.unit);
        info.step = secondPixel - firstPixel - 1;
        info.total = info.topAlpha + info.bottomAlpha +
                     static_cast<Weight>(info.step) * work.unit;
    }

    static void SetAverageEdge(const AverageWork &work, AverageInfo &info,
                               tjs_int position) {
        const Real first = (static_cast<Real>(position) - work.difference) *
                           work.ratio;
        const Real second = first + work.ratio;
        const Real clippedFirst = std::max<Real>(first, 0);
        const Real clippedSecond = std::min<Real>(second, work.maximum);
        const tjs_int firstPixel = RealToInt(first);
        const tjs_int secondPixel = RealToInt(second);
        const tjs_int clippedFirstPixel = RealToInt(clippedFirst);
        const tjs_int clippedSecondPixel = RealToInt(clippedSecond);

        info.offset = (work.sourceOrigin + clippedFirstPixel + 1) *
                      work.offsetMultiplier;
        info.topColor =
            work.unit - RealToInt((first - firstPixel) * work.unit);
        info.bottomColor =
            RealToInt((second - secondPixel) * work.unit);
        info.total = info.topColor + info.bottomColor +
                     static_cast<Weight>(secondPixel - firstPixel - 1) *
                         work.unit;
        info.topAlpha = work.unit -
                        RealToInt((clippedFirst - clippedFirstPixel) *
                                  work.unit);
        info.bottomAlpha = RealToInt(
            (clippedSecond - clippedSecondPixel) * work.unit);
        info.step = clippedSecondPixel - clippedFirstPixel - 1;
    }

    static void AddPoint(Sum &sum, const Byte *pixel, Weight alphaWeight,
                         Weight colorWeight) {
        if(alphaWeight == 0)
            return;
        const Pixel color = *reinterpret_cast<const Pixel *>(pixel);
        sum.red += (color & 0xffu) * colorWeight;
        sum.green += ((color >> 8u) & 0xffu) * colorWeight;
        sum.blue += ((color >> 16u) & 0xffu) * colorWeight;
        sum.alpha += (color >> 24u) * alphaWeight;
    }

    void AddHorizontal(Sum &sum, const Byte *pixel, tjs_int count,
                       Weight alphaWeight, Weight colorWeight) const {
        for(; alphaWeight && count > 0; --count, pixel += 4)
            AddPoint(sum, pixel, alphaWeight, colorWeight);
    }

    void AddVertical(Sum &sum, const Byte *pixel, tjs_int count,
                     Weight alphaWeight, Weight colorWeight) const {
        for(; alphaWeight && count > 0; --count, pixel += sourcePitch_)
            AddPoint(sum, pixel, alphaWeight, colorWeight);
    }

    void AddRectangle(Sum &sum, const Byte *pixel, tjs_int width,
                      tjs_int height, Weight weight) const {
        for(tjs_int y = 0; weight && y < height;
            ++y, pixel += sourcePitch_)
            AddHorizontal(sum, pixel, width, weight, weight);
    }

    void Copy() {
        std::vector<AverageInfo> horizontal(clipEndX_ - clipStartX_);
        std::vector<AverageInfo> vertical(clipEndY_ - clipStartY_);
        const Weight horizontalUnit = MakeAverageTable(horizontal, true);
        const Weight verticalUnit = MakeAverageTable(vertical, false);
        const Weight centerWeight = horizontalUnit * verticalUnit;

        Byte *destinationLine =
            destinationPixels_ + ((destinationX_ + clipStartX_) * 4) +
            ((destinationY_ + clipStartY_) * destinationPitch_);
        for(const AverageInfo &verticalInfo : vertical) {
            Byte *destination = destinationLine;
            const Byte *sourceLine = sourcePixels_ + verticalInfo.offset;
            for(const AverageInfo &horizontalInfo : horizontal) {
                Sum sum;
                const Byte *source = sourceLine + horizontalInfo.offset;
                const tjs_int sourceSpanX = horizontalInfo.step * 4;
                const tjs_int sourceSpanY =
                    verticalInfo.step * sourcePitch_;

                AddPoint(sum, source - 4 - sourcePitch_,
                         horizontalInfo.topAlpha * verticalInfo.topAlpha,
                         horizontalInfo.topColor * verticalInfo.topColor);
                AddHorizontal(sum, source - sourcePitch_, horizontalInfo.step,
                              horizontalUnit * verticalInfo.topAlpha,
                              horizontalUnit * verticalInfo.topColor);
                AddPoint(sum, source + sourceSpanX - sourcePitch_,
                         horizontalInfo.bottomAlpha * verticalInfo.topAlpha,
                         horizontalInfo.bottomColor * verticalInfo.topColor);
                AddVertical(sum, source - 4, verticalInfo.step,
                            horizontalInfo.topAlpha * verticalUnit,
                            horizontalInfo.topColor * verticalUnit);
                AddRectangle(sum, source, horizontalInfo.step,
                             verticalInfo.step, centerWeight);
                AddVertical(sum, source + sourceSpanX, verticalInfo.step,
                            horizontalInfo.bottomAlpha * verticalUnit,
                            horizontalInfo.bottomColor * verticalUnit);
                AddPoint(sum, source - 4 + sourceSpanY,
                         horizontalInfo.topAlpha * verticalInfo.bottomAlpha,
                         horizontalInfo.topColor * verticalInfo.bottomColor);
                AddHorizontal(sum, source + sourceSpanY, horizontalInfo.step,
                              horizontalUnit * verticalInfo.bottomAlpha,
                              horizontalUnit * verticalInfo.bottomColor);
                AddPoint(sum, source + sourceSpanX + sourceSpanY,
                         horizontalInfo.bottomAlpha *
                             verticalInfo.bottomAlpha,
                         horizontalInfo.bottomColor *
                             verticalInfo.bottomColor);

                const Weight divisor =
                    horizontalInfo.total * verticalInfo.total;
                destination[0] = static_cast<Byte>(sum.red / divisor);
                destination[1] = static_cast<Byte>(sum.green / divisor);
                destination[2] = static_cast<Byte>(sum.blue / divisor);
                destination[3] = static_cast<Byte>(sum.alpha / divisor);
                destination += 4;
            }
            destinationLine += destinationPitch_;
        }
    }

    iTJSDispatch2 *destination_{};
    Real dx_{};
    Real dy_{};
    Real dw_{};
    Real dh_{};
    tjs_int destinationX_{};
    tjs_int destinationY_{};
    tjs_int destinationSpanX_{};
    tjs_int destinationSpanY_{};
    tjs_int clipStartX_{};
    tjs_int clipStartY_{};
    tjs_int clipEndX_{};
    tjs_int clipEndY_{};

    iTJSDispatch2 *source_{};
    tjs_int sx_{};
    tjs_int sy_{};
    tjs_int sw_{};
    tjs_int sh_{};
    const Byte *sourcePixels_{};
    tjs_int sourceWidth_{};
    tjs_int sourceHeight_{};
    tjs_int sourcePitch_{};
    Byte *destinationPixels_{};
    tjs_int destinationWidth_{};
    tjs_int destinationHeight_{};
    tjs_int destinationPitch_{};
};

class IntegerFactorShrink final : private LayerBuffer {
public:
    static tjs_error Invoke(tTJSVariant *, tjs_int numparams,
                            tTJSVariant **param,
                            iTJSDispatch2 *destination) {
        if(numparams < 2)
            return TJS_E_BADPARAMCOUNT;

        IntegerFactorShrink operation(
            destination, param[0]->AsObjectNoAddRef(),
            static_cast<tjs_int>(param[1]->AsInteger()),
            numparams >= 3 ? static_cast<tjs_int>(param[2]->AsInteger()) : 0);
        if(!operation.Check())
            return TJS_E_INVALIDPARAM;
        if(!operation.Resize())
            return TJS_E_FAIL;
        operation.Copy();
        return TJS_S_OK;
    }

private:
    IntegerFactorShrink(iTJSDispatch2 *destination, iTJSDispatch2 *source,
                        tjs_int stepX, tjs_int stepY) :
        destination_(destination), source_(source), stepX_(stepX),
        stepY_(stepY ? stepY : stepX) {}

    bool Check() {
        return stepX_ > 0 && stepY_ > 0 &&
               GetForRead(source_, sourceWidth_, sourceHeight_, sourcePixels_,
                          sourcePitch_) &&
               IsValidLayer(destination_);
    }

    bool Resize() {
        tTJSVariant width((sourceWidth_ + stepX_ - 1) / stepX_);
        tTJSVariant height((sourceHeight_ + stepY_ - 1) / stepY_);
        tTJSVariant *parameters[] = { &width, &height };
        return TJS_SUCCEEDED(destination_->FuncCall(
                   0, TJS_W("setImageSize"), nullptr, nullptr, 2, parameters,
                   destination_)) &&
               GetForWrite(destination_, destinationWidth_,
                           destinationHeight_, destinationPixels_,
                           destinationPitch_);
    }

    static void Average(Byte *&destination, const Byte *source,
                        tjs_int outputCount, tjs_int sampleCount,
                        tjs_int sourceStep, tjs_int sampleStep) {
        for(tjs_int output = 0; output < outputCount;
            ++output, source += sourceStep) {
            std::uint64_t premulRed = 0;
            std::uint64_t premulGreen = 0;
            std::uint64_t premulBlue = 0;
            std::uint64_t alpha = 0;

            const Byte *sample = source;
            for(tjs_int index = 0; index < sampleCount;
                ++index, sample += sampleStep) {
                const std::uint64_t a = sample[3];

                premulRed += sample[0] * a;
                premulGreen += sample[1] * a;
                premulBlue += sample[2] * a;
                alpha += a;
            }

            if(alpha != 0) {
                *destination++ = static_cast<Byte>(premulRed / alpha);
                *destination++ = static_cast<Byte>(premulGreen / alpha);
                *destination++ = static_cast<Byte>(premulBlue / alpha);
            } else {
                *destination++ = 0;
                *destination++ = 0;
                *destination++ = 0;
            }

            *destination++ =
                static_cast<Byte>(alpha / static_cast<std::uint64_t>(sampleCount));
        }
    }

    void ShrinkLineX(Byte *destination, const Byte *source) const {
        const tjs_int fullBlocks = sourceWidth_ / stepX_;
        Average(destination, source, fullBlocks, stepX_, stepX_ * 4, 4);
        const tjs_int remainder = sourceWidth_ - fullBlocks * stepX_;
        if(remainder > 0)
            Average(destination, source + fullBlocks * stepX_ * 4, 1,
                    remainder, 0, 4);
    }

    void Copy() {
        if(stepY_ <= 1) {
            const Byte *source = sourcePixels_;
            Byte *destination = destinationPixels_;
            for(tjs_int y = 0; y < sourceHeight_; ++y) {
                ShrinkLineX(destination, source);
                source += sourcePitch_;
                destination += destinationPitch_;
            }
            return;
        }

        const tjs_int rowBytes = destinationWidth_ * 4;
        std::vector<Byte> intermediate(
            static_cast<std::size_t>(rowBytes) * stepY_);
        const Byte *source = sourcePixels_;
        Byte *destination = destinationPixels_;
        for(tjs_int outputY = 0; outputY < destinationHeight_; ++outputY) {
            const tjs_int remaining = sourceHeight_ - outputY * stepY_;
            const tjs_int sampleRows = std::min(stepY_, remaining);
            for(tjs_int row = 0; row < sampleRows; ++row) {
                ShrinkLineX(intermediate.data() + row * rowBytes, source);
                source += sourcePitch_;
            }
            Byte *write = destination;
            Average(write, intermediate.data(), destinationWidth_, sampleRows,
                    4, rowBytes);
            destination += destinationPitch_;
        }
    }

    iTJSDispatch2 *destination_{};
    iTJSDispatch2 *source_{};
    tjs_int stepX_{};
    tjs_int stepY_{};
    const Byte *sourcePixels_{};
    tjs_int sourceWidth_{};
    tjs_int sourceHeight_{};
    tjs_int sourcePitch_{};
    Byte *destinationPixels_{};
    tjs_int destinationWidth_{};
    tjs_int destinationHeight_{};
    tjs_int destinationPitch_{};
};

} // namespace

NCB_ATTACH_FUNCTION(shrinkCopy, Layer, AreaAverageShrink::Invoke);
NCB_ATTACH_FUNCTION(shrinkCopyFast, Layer, IntegerFactorShrink::Invoke);
