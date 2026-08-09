#include "ncbind.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <spdlog/spdlog.h>
#include <vector>

#define NCB_MODULE_NAME TJS_W("TextRender.dll")

// #define dbg_print TVPAddLog
#define dbg_print

// use Kirikiri-Z rasterizer for layouting
#include "FontRasterizer.h"
#include "LayerBitmapIntf.h"

#define LOGGER spdlog::get("plugin")

extern FontRasterizer *GetCurrentRasterizer();

using RgbColor = uint32_t;

// -----------------------------------------------------------------------------
// TJS dictionary helpers.
//
// The original DLL uses lower-case keys such as "fontsize", "linespacing",
// "shadowcolor", etc.  Do not use C++ member-name stringification here: the
// existing reimplementation accidentally exposed camelCase dictionary keys.
// -----------------------------------------------------------------------------

static bool getVariantProp(iTJSDispatch2 *dict, const tjs_char *name,
                           tTJSVariant &value) {
    if(!dict)
        return false;

    if(TJS_FAILED(dict->PropGet(0, name, nullptr, &value, dict)))
        return false;

    return value.Type() != tvtVoid;
}

static bool getBoolProp(iTJSDispatch2 *dict, const tjs_char *name, bool &out) {
    tTJSVariant value;
    if(!getVariantProp(dict, name, value))
        return false;
    out = static_cast<tjs_int>(value) != 0;
    return true;
}

static bool getIntProp(iTJSDispatch2 *dict, const tjs_char *name, int &out) {
    tTJSVariant value;
    if(!getVariantProp(dict, name, value))
        return false;
    out = static_cast<int>(static_cast<tjs_int>(value));
    return true;
}

static bool getRealProp(iTJSDispatch2 *dict, const tjs_char *name, float &out) {
    tTJSVariant value;
    if(!getVariantProp(dict, name, value))
        return false;
    out = static_cast<float>(static_cast<tjs_real>(value));
    return true;
}

static bool getColorProp(iTJSDispatch2 *dict, const tjs_char *name,
                         RgbColor &out) {
    tTJSVariant value;
    if(!getVariantProp(dict, name, value))
        return false;
    out = static_cast<RgbColor>(static_cast<tjs_int>(value));
    return true;
}

static bool getStringProp(iTJSDispatch2 *dict, const tjs_char *name,
                          ttstr &out) {
    tTJSVariant value;
    if(!getVariantProp(dict, name, value))
        return false;

    auto string = value.AsStringNoAddRef();
    if(!string)
        return false;

    out = *string;
    return true;
}

static void setBoolProp(iTJSDispatch2 *dict, const tjs_char *name, bool value) {
    tTJSVariant variant(static_cast<tjs_int>(value));
    dict->PropSet(TJS_MEMBERENSURE, name, nullptr, &variant, dict);
}

static void setIntProp(iTJSDispatch2 *dict, const tjs_char *name, int value) {
    tTJSVariant variant(static_cast<tjs_int>(value));
    dict->PropSet(TJS_MEMBERENSURE, name, nullptr, &variant, dict);
}

static void setRealProp(iTJSDispatch2 *dict, const tjs_char *name, float value) {
    tTJSVariant variant(static_cast<tjs_real>(value));
    dict->PropSet(TJS_MEMBERENSURE, name, nullptr, &variant, dict);
}

static void setColorProp(iTJSDispatch2 *dict, const tjs_char *name,
                         RgbColor value) {
    tTJSVariant variant(static_cast<tjs_int>(value));
    dict->PropSet(TJS_MEMBERENSURE, name, nullptr, &variant, dict);
}

static void setStringProp(iTJSDispatch2 *dict, const tjs_char *name,
                          const ttstr &value) {
    tTJSVariant variant(value);
    dict->PropSet(TJS_MEMBERENSURE, name, nullptr, &variant, dict);
}

// -----------------------------------------------------------------------------

struct TextRenderState {
    // Constructor seed values recovered from textrender.dll @ 0x1000d420.
    bool bold = false;
    bool italic = false;
    ttstr face = TJS_W("normal");

    float fontSize = 24.0f;
    float bigFontSize = 48.0f;
    float smallFontSize = 12.0f;

    RgbColor chColor = 0xffffffffu;

    // Note: the constructor seed is 10, while setDefault({fontsize: ...})
    // derives rubySize = fontsize / 3 when the rubysize key is omitted.
    float rubySize = 10.0f;
    float rubyOffset = -2.0f;

    bool shadow = true;
    RgbColor shadowColor = 0xff000000u;
    int shadowDiff = 1;

    bool edge = false;
    RgbColor edgeColor = 0xff0080ffu;

    // Paragraph/style defaults recovered from the DLL.
    float lineSpacing = 6.0f;
    float pitch = 0.0f;
    float lineSize = 24.0f;

    // Runtime fields kept here for compatibility with the existing wrapper.
    bool renderOver = false;
    float renderDelay = 0.0f;
    ttstr renderText = TJS_W("");

    tTJSVariant serialize() const {
        auto dict = TJSCreateDictionaryObject();

        setStringProp(dict, TJS_W("face"), face);
        setBoolProp(dict, TJS_W("bold"), bold);
        setBoolProp(dict, TJS_W("italic"), italic);
        setRealProp(dict, TJS_W("fontsize"), fontSize);
        setRealProp(dict, TJS_W("bigfontsize"), bigFontSize);
        setRealProp(dict, TJS_W("smallfontsize"), smallFontSize);
        setRealProp(dict, TJS_W("rubysize"), rubySize);
        setRealProp(dict, TJS_W("rubyoffset"), rubyOffset);
        setColorProp(dict, TJS_W("color"), chColor);
        setBoolProp(dict, TJS_W("shadow"), shadow);
        setColorProp(dict, TJS_W("shadowcolor"), shadowColor);
        setIntProp(dict, TJS_W("shadowdiff"), shadowDiff);
        setBoolProp(dict, TJS_W("edge"), edge);
        setColorProp(dict, TJS_W("edgecolor"), edgeColor);
        setRealProp(dict, TJS_W("linespacing"), lineSpacing);
        setRealProp(dict, TJS_W("pitch"), pitch);
        setRealProp(dict, TJS_W("linesize"), lineSize);

        auto result = tTJSVariant(dict, dict);
        dict->Release();
        return result;
    }

    // setDefault() semantics recovered from 0x10002220.
    // Missing keys keep the existing seed/default, except that supplying
    // fontsize derives big/small/ruby/line sizes if those keys are omitted.
    void deserializeDefault(tTJSVariant settings) {
        auto dict = settings.AsObjectNoAddRef();
        if(!dict)
            return;

        getStringProp(dict, TJS_W("face"), face);
        getBoolProp(dict, TJS_W("bold"), bold);
        getBoolProp(dict, TJS_W("italic"), italic);

        float newFontSize = fontSize;
        const bool hasFontSize =
            getRealProp(dict, TJS_W("fontsize"), newFontSize);
        if(hasFontSize)
            fontSize = newFontSize;

        if(!getRealProp(dict, TJS_W("bigfontsize"), bigFontSize) &&
           hasFontSize)
            bigFontSize = fontSize * 2.0f;

        if(!getRealProp(dict, TJS_W("smallfontsize"), smallFontSize) &&
           hasFontSize)
            smallFontSize = fontSize * 0.5f;

        if(!getRealProp(dict, TJS_W("rubysize"), rubySize) && hasFontSize)
            rubySize = fontSize / 3.0f;

        getRealProp(dict, TJS_W("rubyoffset"), rubyOffset);
        getColorProp(dict, TJS_W("color"), chColor);
        getBoolProp(dict, TJS_W("shadow"), shadow);
        getColorProp(dict, TJS_W("shadowcolor"), shadowColor);
        getIntProp(dict, TJS_W("shadowdiff"), shadowDiff);
        getBoolProp(dict, TJS_W("edge"), edge);
        getColorProp(dict, TJS_W("edgecolor"), edgeColor);
        getRealProp(dict, TJS_W("linespacing"), lineSpacing);
        getRealProp(dict, TJS_W("pitch"), pitch);

        if(!getRealProp(dict, TJS_W("linesize"), lineSize) && hasFontSize)
            lineSize = fontSize;
    }
};

struct TextRenderOptions {
    // Exact UTF-16 strings recovered from the DLL's .rdata section.
    ttstr following = TJS_W(
        "%),:;]}。，、．：；゛゜ヽヾゝゞ々’”）〕］｝〉》」』】°′″℃￠％‰　!.?・？！ー"
        "ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ");
    ttstr leading = TJS_W("\\$([{‘“（〔［｛〈《「『【￥＄￡");
    ttstr begin = TJS_W("「『（‘“〔［｛〈《");
    ttstr end = TJS_W("」』）’”〕］｝〉》");

    bool vertical = false;
    int kinsokuMax = 1;
    bool wordBreak = true;

    bool ignoreColor = false;
    bool ignoreSize = false;
    bool ignoreDelay = false;
    bool ignoreOverX = false;
    bool ignoreOverY = false;
    bool widthTimeScale = false;
    bool ignoreRuby = false;
    bool ignoreType = false;
    bool ignoreFace = false;
    bool ignoreStyle = false;

    tTJSVariant serialize() const {
        auto dict = TJSCreateDictionaryObject();

        setStringProp(dict, TJS_W("following"), following);
        setStringProp(dict, TJS_W("leading"), leading);
        setStringProp(dict, TJS_W("begin"), begin);
        setStringProp(dict, TJS_W("end"), end);
        setBoolProp(dict, TJS_W("vertical"), vertical);
        setIntProp(dict, TJS_W("kinsoku_max"), kinsokuMax);
        setBoolProp(dict, TJS_W("word_break"), wordBreak);
        setBoolProp(dict, TJS_W("ignore_color"), ignoreColor);
        setBoolProp(dict, TJS_W("ignore_size"), ignoreSize);
        setBoolProp(dict, TJS_W("ignore_delay"), ignoreDelay);
        setBoolProp(dict, TJS_W("ignore_overx"), ignoreOverX);
        setBoolProp(dict, TJS_W("ignore_overy"), ignoreOverY);
        setBoolProp(dict, TJS_W("width_time_scale"), widthTimeScale);
        setBoolProp(dict, TJS_W("ignore_ruby"), ignoreRuby);
        setBoolProp(dict, TJS_W("ignore_type"), ignoreType);
        setBoolProp(dict, TJS_W("ignore_face"), ignoreFace);
        setBoolProp(dict, TJS_W("ignore_style"), ignoreStyle);

        auto result = tTJSVariant(dict, dict);
        dict->Release();
        return result;
    }

    void deserialize(tTJSVariant settings) {
        auto dict = settings.AsObjectNoAddRef();
        if(!dict)
            return;

        getStringProp(dict, TJS_W("following"), following);
        getStringProp(dict, TJS_W("leading"), leading);
        getStringProp(dict, TJS_W("begin"), begin);
        getStringProp(dict, TJS_W("end"), end);
        getBoolProp(dict, TJS_W("vertical"), vertical);
        getIntProp(dict, TJS_W("kinsoku_max"), kinsokuMax);
        getBoolProp(dict, TJS_W("word_break"), wordBreak);
        getBoolProp(dict, TJS_W("ignore_color"), ignoreColor);
        getBoolProp(dict, TJS_W("ignore_size"), ignoreSize);
        getBoolProp(dict, TJS_W("ignore_delay"), ignoreDelay);

        // The DLL maps ignore_over to the same flag as ignore_overy.
        bool ignoreOver = ignoreOverY;
        if(getBoolProp(dict, TJS_W("ignore_over"), ignoreOver))
            ignoreOverY = ignoreOver;
        getBoolProp(dict, TJS_W("ignore_overy"), ignoreOverY);
        getBoolProp(dict, TJS_W("ignore_overx"), ignoreOverX);

        getBoolProp(dict, TJS_W("width_time_scale"), widthTimeScale);
        getBoolProp(dict, TJS_W("ignore_ruby"), ignoreRuby);
        getBoolProp(dict, TJS_W("ignore_type"), ignoreType);
        getBoolProp(dict, TJS_W("ignore_face"), ignoreFace);
        getBoolProp(dict, TJS_W("ignore_style"), ignoreStyle);
    }

    static TextRenderOptions from(tTJSVariant settings) {
        TextRenderOptions options{};
        options.deserialize(settings);
        return options;
    }
};

struct CharacterInfo {
    // Public record shape recovered from the serialization routine around
    // 0x100035d3.  Geometry and delay are REAL values in the original DLL.
    bool graph = false;
    ttstr text = TJS_W("");

    float x = 0.0f;
    float y = 0.0f;
    float cw = 0.0f;
    float size = 0.0f;

    ttstr face = TJS_W("normal");
    RgbColor color = 0xffffffffu;
    bool bold = false;
    bool italic = false;
    bool shadow = false;
    bool edge = false;
    RgbColor shadowColor = 0xff000000u;
    int shadowDiff = 1;
    RgbColor edgeColor = 0xff0080ffu;

    ttstr ruby = TJS_W("");
    bool vertical = false;
    float delay = 0.0f;
    int link = -1;
    ttstr linkName = TJS_W("");

    // Internal-only layout metadata.  These are deliberately not serialized.
    int lineIndex = 0;
    // Logical cursor advance including the pitch active for this character.
    // The DLL aligns a line from its flow cursor, so trailing pitch matters.
    float flowAdvance = 0.0f;

    tTJSVariant serialize() const {
        auto dict = TJSCreateDictionaryObject();

        setBoolProp(dict, TJS_W("graph"), graph);
        setStringProp(dict, TJS_W("text"), text);
        setRealProp(dict, TJS_W("x"), x);
        setRealProp(dict, TJS_W("y"), y);
        setRealProp(dict, TJS_W("cw"), cw);
        setRealProp(dict, TJS_W("size"), size);
        setStringProp(dict, TJS_W("face"), face);
        setColorProp(dict, TJS_W("color"), color);
        setBoolProp(dict, TJS_W("bold"), bold);
        setBoolProp(dict, TJS_W("italic"), italic);
        setBoolProp(dict, TJS_W("shadow"), shadow);
        setBoolProp(dict, TJS_W("edge"), edge);
        setColorProp(dict, TJS_W("shadowColor"), shadowColor);
        setIntProp(dict, TJS_W("shadowDiff"), shadowDiff);
        setColorProp(dict, TJS_W("edgeColor"), edgeColor);

        if(ruby.length() > 0)
            setStringProp(dict, TJS_W("ruby"), ruby);

        setBoolProp(dict, TJS_W("vertical"), vertical);
        setRealProp(dict, TJS_W("delay"), delay);
        setIntProp(dict, TJS_W("link"), link);
        if(link >= 0 && linkName.length() > 0)
            setStringProp(dict, TJS_W("linkName"), linkName);

        auto result = tTJSVariant(dict, dict);
        dict->Release();
        return result;
    }

    void deserialize(tTJSVariant settings) {
        auto dict = settings.AsObjectNoAddRef();
        if(!dict)
            return;

        getBoolProp(dict, TJS_W("graph"), graph);
        getStringProp(dict, TJS_W("text"), text);
        getRealProp(dict, TJS_W("x"), x);
        getRealProp(dict, TJS_W("y"), y);
        getRealProp(dict, TJS_W("cw"), cw);
        getRealProp(dict, TJS_W("size"), size);
        getStringProp(dict, TJS_W("face"), face);
        getColorProp(dict, TJS_W("color"), color);
        getBoolProp(dict, TJS_W("bold"), bold);
        getBoolProp(dict, TJS_W("italic"), italic);
        getBoolProp(dict, TJS_W("shadow"), shadow);
        getBoolProp(dict, TJS_W("edge"), edge);
        getColorProp(dict, TJS_W("shadowColor"), shadowColor);
        getIntProp(dict, TJS_W("shadowDiff"), shadowDiff);
        getColorProp(dict, TJS_W("edgeColor"), edgeColor);
        getStringProp(dict, TJS_W("ruby"), ruby);
        getBoolProp(dict, TJS_W("vertical"), vertical);
        getRealProp(dict, TJS_W("delay"), delay);
        getIntProp(dict, TJS_W("link"), link);
        getStringProp(dict, TJS_W("linkName"), linkName);
    }

    static CharacterInfo from(tTJSVariant settings) {
        CharacterInfo info{};
        info.deserialize(settings);
        return info;
    }
};

struct LinkRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

enum TextRenderAlignment {
    kTextRenderAlignmentLeft = -1,
    kTextRenderAlignmentCenter = 0,
    kTextRenderAlignmentRight = 1,
};

enum TextRenderValign {
    kTextRenderValignTop = -1,
    kTextRenderValignMiddle = 0,
    kTextRenderValignBottom = 1,
};

#define property_accessor(name, type, storage)                                 \
    type get_##name() const { return storage; }                                \
    void set_##name(type value) { storage = value; }

#define property_accessor_cast(name, type, cast, storage)                      \
    cast get_##name() const { return static_cast<cast>(storage); }              \
    void set_##name(cast value) { storage = static_cast<type>(value); }

#define property_accessor_string(name, storage)                                \
    tTJSVariant get_##name() const { return tTJSVariant(storage); }             \
    void set_##name(tTJSVariant value) {                                       \
        auto string = value.AsStringNoAddRef();                                \
        if(string)                                                             \
            storage = *string;                                                 \
    }

#define property_delegate(name) NCB_PROPERTY(name, get_##name, set_##name);

class TextRenderBase {
public:
    TextRenderBase();
    virtual ~TextRenderBase();

    bool render(tTJSString text, int autoIndent, int diff, int all, bool same);
    void setRenderSize(int width, int height);
    void setDefault(tTJSVariant defaultSettings);
    void setOption(tTJSVariant options);
    void setFont(tTJSVariant fontSettings);
    void setStyle(tTJSVariant styleSettings);
    tTJSVariant getCharacters(int start, int count);
    tTJSVariant getLinkNames();
    tTJSVariant getLinkRects(int link);
    tTJSVariant getLinkCharacters(int link);
    bool isLinkContains(int link, double x, double y) const;
    int getLinkOfPosition(double x, double y) const;
    void clear();
    void done();
    void newline();

    void resetFont();
    void resetStyle();
    tTJSVariant getKeyWait();
    void setKeyWait(tTJSVariant) { throw "no support setKeyWait"; }
    int calcShowCount(int time);
    bool contains(double x, double y) const;

    // Class-level scale values in the DLL.  fontScale is not part of the
    // default/current font state and there is no original defaultFontScale.
    property_accessor(timeScale, float, m_timeScale);
    property_accessor(fontScale, float, m_fontScale);

    property_accessor(vertical, bool, m_vertical);

    bool get_bold() const { return m_state.bold; }
    void set_bold(bool value) {
        if(m_state.bold != value) {
            m_state.bold = value;
            updateFont();
        }
    }

    bool get_italic() const { return m_state.italic; }
    void set_italic(bool value) {
        if(m_state.italic != value) {
            m_state.italic = value;
            updateFont();
        }
    }

    tTJSVariant get_face() const { return tTJSVariant(m_state.face); }
    void set_face(tTJSVariant value) {
        auto string = value.AsStringNoAddRef();
        if(string) {
            m_state.face = *string;
            updateFont();
        }
    }

    double get_fontSize() const { return static_cast<double>(m_state.fontSize); }
    void set_fontSize(double value) {
        m_state.fontSize = static_cast<float>(value);
        updateFont();
    }
    // DLL %B/%S read the default big/small basis directly (+0xbc/+0xc0).
    // There is no separately reset current big/small field in the original.
    // Keep these camelCase accessors only as a compatibility extension and
    // point them at the same storage as defaultBigFontSize/defaultSmallFontSize.
    double get_bigFontSize() const {
        return static_cast<double>(m_default.bigFontSize);
    }
    void set_bigFontSize(double value) {
        m_default.bigFontSize = static_cast<float>(value);
    }
    double get_smallFontSize() const {
        return static_cast<double>(m_default.smallFontSize);
    }
    void set_smallFontSize(double value) {
        m_default.smallFontSize = static_cast<float>(value);
    }
    property_accessor_cast(chColor, RgbColor, tjs_int, m_state.chColor);
    property_accessor_cast(rubySize, float, double, m_state.rubySize);
    property_accessor_cast(rubyOffset, float, double, m_state.rubyOffset);
    property_accessor(shadow, bool, m_state.shadow);
    property_accessor_cast(shadowColor, RgbColor, tjs_int, m_state.shadowColor);
    property_accessor(shadowDiff, int, m_state.shadowDiff);
    property_accessor(edge, bool, m_state.edge);
    property_accessor_cast(edgeColor, RgbColor, tjs_int, m_state.edgeColor);
    property_accessor_cast(lineSpacing, float, double, m_state.lineSpacing);
    property_accessor_cast(pitch, float, double, m_state.pitch);
    property_accessor_cast(lineSize, float, double, m_state.lineSize);
    property_accessor_cast(align, TextRenderAlignment, int, m_alignment);
    property_accessor_cast(valign, TextRenderValign, int, m_valign);

    bool get_renderOver() const { return m_state.renderOver; }
    void set_renderOver(bool) { throw "renderOver is read-only"; }

    double get_renderDelay() const {
        // DLL @ 0x10010d60 exposes the accumulated render timeline and only
        // applies timeScale at query time.  The active %d/%a delay is a
        // separate internal value.
        return static_cast<double>(m_elapsedDelay * m_timeScale);
    }
    void set_renderDelay(double) { throw "renderDelay is read-only"; }

    // DLL getters at 0x100017e0..0x10001810 read four independent content
    // bounds (+a8/+ac/+b0/+b4). They are not the cursor and not box edges.
    double get_renderLeft() const { return static_cast<double>(m_renderLeft); }
    void set_renderLeft(double) { throw "renderLeft is read-only"; }
    double get_renderTop() const { return static_cast<double>(m_renderTop); }
    void set_renderTop(double) { throw "renderTop is read-only"; }
    double get_renderRight() const { return static_cast<double>(m_renderRight); }
    void set_renderRight(double) { throw "renderRight is read-only"; }
    double get_renderBottom() const { return static_cast<double>(m_renderBottom); }
    void set_renderBottom(double) { throw "renderBottom is read-only"; }

    double get_maxScrollOffset() const {
        // Exact getter @ 0x10010d80. No clamping is performed by the DLL.
        return m_vertical ? static_cast<double>(m_boxWidth - m_renderLeft)
                          : static_cast<double>(m_boxHeight - m_renderBottom);
    }
    void set_maxScrollOffset(double) {
        throw "maxScrollOffset is read-only";
    }
    property_accessor_string(renderText, m_state.renderText);
    int get_renderCount() const {
        // DLL keeps an independent count (+0x20c), rather than deriving it
        // from renderText. Include the pending kinsoku buffer as render() has
        // already accepted those characters even if they have not flushed.
        return static_cast<int>(m_characters.size() + m_buffer.size());
    }
    void set_renderCount(int) { throw "avoid to set renderCount"; }

    property_accessor(defaultBold, bool, m_default.bold);
    property_accessor(defaultItalic, bool, m_default.italic);
    property_accessor_string(defaultFace, m_default.face);
    property_accessor_cast(defaultFontSize, float, double, m_default.fontSize);
    property_accessor_cast(defaultBigFontSize, float, double,
                           m_default.bigFontSize);
    property_accessor_cast(defaultSmallFontSize, float, double,
                           m_default.smallFontSize);
    property_accessor_cast(defaultChColor, RgbColor, tjs_int,
                           m_default.chColor);
    property_accessor_cast(defaultRubySize, float, double, m_default.rubySize);
    property_accessor_cast(defaultRubyOffset, float, double,
                           m_default.rubyOffset);
    property_accessor(defaultShadow, bool, m_default.shadow);
    property_accessor_cast(defaultShadowColor, RgbColor, tjs_int,
                           m_default.shadowColor);
    property_accessor(defaultShadowDiff, int, m_default.shadowDiff);
    property_accessor(defaultEdge, bool, m_default.edge);
    property_accessor_cast(defaultEdgeColor, RgbColor, tjs_int,
                           m_default.edgeColor);
    property_accessor_cast(defaultLineSpacing, float, double,
                           m_default.lineSpacing);
    property_accessor_cast(defaultPitch, float, double, m_default.pitch);
    property_accessor_cast(defaultLineSize, float, double, m_default.lineSize);
    property_accessor_cast(defaultAlign, TextRenderAlignment, int,
                           m_defaultAlignment);
    property_accessor_cast(defaultValign, TextRenderValign, int,
                           m_defaultValign);

private:
    float m_boxWidth = 0.0f;
    float m_boxHeight = 0.0f;

    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_indent = 0.0f;

    // Content bounds exposed as renderLeft/renderTop/renderRight/renderBottom.
    float m_renderLeft = 0.0f;
    float m_renderTop = 0.0f;
    float m_renderRight = 0.0f;
    float m_renderBottom = 0.0f;
    int m_autoIndent = 0;

    bool m_overflow = false;
    bool m_isBeginningOfLine = true;

    bool m_vertical = false;
    TextRenderAlignment m_alignment = kTextRenderAlignmentLeft;
    TextRenderValign m_valign = kTextRenderValignTop;
    TextRenderAlignment m_defaultAlignment = kTextRenderAlignmentLeft;
    TextRenderValign m_defaultValign = kTextRenderValignTop;

    float m_timeScale = 1.0f;
    float m_fontScale = 1.0f;

    TextRenderOptions m_options{};
    TextRenderState m_default{};
    TextRenderState m_state{};

    std::vector<CharacterInfo> m_characters{};
    std::vector<CharacterInfo> m_buffer{};

    // Kinsoku/layout state.
    uint32_t m_mode = 0;
    int m_lineIndex = 0;
    float m_lineCrossExtent = 0.0f;
    bool m_lineFinalized = false;

    // Timing state recovered from the DLL.  m_elapsedDelay corresponds to the
    // unscaled cumulative character timeline (+0x210 during a segment);
    // m_state.renderDelay is the active %d/%a character delay (+0x180).
    // timeScale is intentionally applied only by public/query operations.
    float m_elapsedDelay = 0.0f;

    // The DLL stores ruby text and an integer parameter separately.  Exact
    // multi-character ruby distribution is still partially unresolved; the
    // parser/storage and CharacterInfo field are kept so callers do not lose
    // the markup.
    ttstr m_pendingRuby = TJS_W("");
    int m_pendingRubyCount = 0;

    int m_currentLink = -1;
    ttstr m_currentLinkName = TJS_W("");
    std::vector<ttstr> m_linkNames{};

    void pushCharacter(tjs_char ch);
    void pushGraphicalCharacter(const ttstr &graph);
    void performLinebreak();
    void flush(bool force = false);
    void finalizeCurrentLine();
    void applyAlignment();
    void updateFont();
    void advanceWideSpace();
    void recomputeRenderBounds();
    int findOrAddLink(const ttstr &name);
    std::vector<LinkRect> collectLinkRects(int link) const;

    float getCharacterDelay(const CharacterInfo &info) const;
};

enum TextRenderMode {
    kTextRenderModeLeading = 0,
    kTextRenderModeNormal,
    kTextRenderModeFollowing,
};

// -----------------------------------------------------------------------------

TextRenderBase::TextRenderBase() {
    // The constructor seeds current state from the same values as default.
    m_state = m_default;
    m_state.renderDelay = 0.0f;
}

TextRenderBase::~TextRenderBase() {}

static bool readchar(const tTJSString &str, size_t &i, tjs_char &c) {
    const auto len = str.GetLen();
    if(++i >= len)
        return false;
    c = str[i];
    return true;
}

static ttstr readUntil(const tTJSString &str, size_t &i, tjs_char terminator) {
    ttstr result{};
    tjs_char ch = 0;

    while(readchar(str, i, ch)) {
        if(ch == terminator)
            return result;
        result += ch;
    }

    TVPThrowExceptionMessage(
        TJS_W("TextRenderBase::render() failed to parse: expected '%1', found EOF"),
        terminator);
    return result;
}

static bool isDecimalDigit(tjs_char ch) { return ch >= '0' && ch <= '9'; }

static int parseSignedDecimal(const ttstr &text, int fallback,
                              bool *hadDigits = nullptr) {
    bool negative = false;
    bool digits = false;
    int value = 0;

    for(int i = 0; i < text.length(); ++i) {
        const tjs_char ch = text[i];
        if(ch == '-') {
            negative = !negative;
            continue;
        }
        if(!isDecimalDigit(ch))
            continue;
        digits = true;
        value = value * 10 + static_cast<int>(ch - '0');
    }

    if(hadDigits)
        *hadDigits = digits;
    if(!digits)
        return fallback;
    return negative ? -value : value;
}

static RgbColor parseHexColor(const ttstr &text, RgbColor fallback) {
    if(text.length() == 0)
        return fallback;

    RgbColor color = 0;
    for(int i = 0; i < text.length(); ++i) {
        const tjs_char ch = text[i];
        RgbColor nibble = 0;
        if(ch >= '0' && ch <= '9')
            nibble = static_cast<RgbColor>(ch - '0');
        else if(ch >= 'A' && ch <= 'F')
            nibble = 10u + static_cast<RgbColor>(ch - 'A');
        else if(ch >= 'a' && ch <= 'f')
            nibble = 10u + static_cast<RgbColor>(ch - 'a');
        else
            TVPThrowExceptionMessage(
                TJS_W("TextRenderBase::render() failed to parse hexadecimal color"));
        color = (color << 4u) | nibble;
    }

    // The DLL forces opaque alpha on explicit text/shadow/edge colors.
    return 0xff000000u | (color & 0x00ffffffu);
}

static bool findchInChars(const ttstr &chars, tjs_char ch) {
    for(int i = 0; i < chars.length(); ++i) {
        if(chars[i] == ch)
            return true;
    }
    return false;
}

float TextRenderBase::getCharacterDelay(const CharacterInfo &info) const {
    float delay = m_state.renderDelay;
    if(m_options.widthTimeScale) {
        const float em = m_fontScale * m_state.fontSize;
        if(em != 0.0f) {
            const float advance = m_vertical ? info.size : info.cw;
            delay *= advance / em;
        }
    }
    return delay;
}

bool TextRenderBase::render(tTJSString text, int autoIndent, int diff, int all,
                            bool same) {
    (void)same;

    m_autoIndent = autoIndent;

    // Recovered from render prologue: all>0 && diff==0 uses 0.001f.
    const float baseDelay = (all > 0 && diff == 0) ? 0.001f
                                                   : static_cast<float>(diff);
    // render() stores the call's base delay into the active delay field on
    // every invocation (DLL render prologue around 0x1000e241).
    m_state.renderDelay = baseDelay;

    const auto len = text.GetLen();

    for(size_t i = 0; i < len; ++i) {
        auto ch = text[i];

        switch(ch) {
            case '\n':
                flush();
                performLinebreak();
                break;

            case '%': {
                if(!readchar(text, i, ch))
                    break;

                switch(ch) {
                    case 'b': {
                        tjs_char value = 0;
                        if(!readchar(text, i, value))
                            value = 0;
                        if(!m_options.ignoreType) {
                            m_state.bold = value == '1'
                                               ? true
                                               : value == '0' ? false
                                                              : m_default.bold;
                            updateFont();
                        }
                        break;
                    }

                    case 'i': {
                        tjs_char value = 0;
                        if(!readchar(text, i, value))
                            value = 0;
                        if(!m_options.ignoreType) {
                            m_state.italic = value == '1'
                                                 ? true
                                                 : value == '0'
                                                       ? false
                                                       : m_default.italic;
                            updateFont();
                        }
                        break;
                    }

                    case 's': {
                        tjs_char value = 0;
                        if(!readchar(text, i, value))
                            break;

                        if(value == '#') {
                            auto digits = readUntil(text, i, ';');
                            if(!m_options.ignoreColor)
                                m_state.shadowColor =
                                    parseHexColor(digits, m_default.shadowColor);
                        } else if(!m_options.ignoreType) {
                            m_state.shadow = value == '1'
                                                 ? true
                                                 : value == '0'
                                                       ? false
                                                       : m_default.shadow;
                        }
                        break;
                    }

                    case 'e': {
                        tjs_char value = 0;
                        if(!readchar(text, i, value))
                            break;

                        if(value == '#') {
                            auto digits = readUntil(text, i, ';');
                            if(!m_options.ignoreColor)
                                m_state.edgeColor =
                                    parseHexColor(digits, m_default.edgeColor);
                        } else if(!m_options.ignoreType) {
                            m_state.edge = value == '1'
                                               ? true
                                               : value == '0' ? false
                                                              : m_default.edge;
                        }
                        break;
                    }

                    case 'B':
                        if(!m_options.ignoreSize) {
                            m_state.fontSize = m_default.bigFontSize;
                            updateFont();
                        }
                        break;

                    case 'S':
                        if(!m_options.ignoreSize) {
                            m_state.fontSize = m_default.smallFontSize;
                            updateFont();
                        }
                        break;

                    case 'r':
                        resetFont();
                        break;

                    case 'C':
                        if(!m_options.ignoreStyle)
                            m_alignment = kTextRenderAlignmentCenter;
                        break;

                    case 'R':
                        if(!m_options.ignoreStyle)
                            m_alignment = kTextRenderAlignmentRight;
                        break;

                    case 'L':
                        if(!m_options.ignoreStyle)
                            m_alignment = kTextRenderAlignmentLeft;
                        break;

                    case 'f': {
                        auto face = readUntil(text, i, ';');
                        if(!m_options.ignoreFace) {
                            m_state.face =
                                face.length() == 0 ? m_default.face : face;
                            updateFont();
                        }
                        break;
                    }

                    case 'p': {
                        auto valueText = readUntil(text, i, ';');
                        if(!m_options.ignoreStyle) {
                            bool hadDigits = false;
                            const int value = parseSignedDecimal(
                                valueText, static_cast<int>(m_default.pitch),
                                &hadDigits);
                            m_state.pitch = hadDigits
                                                ? static_cast<float>(value)
                                                : m_default.pitch;
                        }
                        break;
                    }

                    case 'd': {
                        auto valueText = readUntil(text, i, ';');
                        if(!m_options.ignoreDelay) {
                            bool hadDigits = false;
                            const int percent =
                                parseSignedDecimal(valueText, 0, &hadDigits);
                            m_state.renderDelay =
                                hadDigits ? (static_cast<float>(percent) / 100.0f) *
                                                baseDelay
                                          : baseDelay;
                        }
                        break;
                    }

                    case 'a': {
                        auto valueText = readUntil(text, i, ';');
                        if(!m_options.ignoreDelay) {
                            bool hadDigits = false;
                            const int value =
                                parseSignedDecimal(valueText, 0, &hadDigits);
                            m_state.renderDelay = hadDigits
                                                      ? static_cast<float>(value)
                                                      : baseDelay;
                        }
                        break;
                    }

                    case 't': {
                        // The DLL accepts decimal or $label forms and updates
                        // its timing/label machinery.  Parsing is exact here;
                        // callback dispatch is intentionally not fabricated.
                        (void)readUntil(text, i, ';');
                        break;
                    }

                    case 'w': {
                        // Percent-w is key-wait/timing machinery (not a space).
                        // Keep the stream in sync until the callback contract is
                        // wired to the surrounding TJS host.
                        (void)readUntil(text, i, ';');
                        break;
                    }

                    case 'D': {
                        // Label/delay command.  Original supports both numeric
                        // and $label payloads.  Preserve parsing without
                        // inventing the host callback side effect.
                        (void)readUntil(text, i, ';');
                        break;
                    }

                    case 'l': {
                        auto linkName = readUntil(text, i, ';');
                        if(linkName.length() == 0) {
                            m_currentLink = -1;
                            m_currentLinkName = TJS_W("");
                        } else {
                            // The DLL keeps a stable link table. Preserve that
                            // behavior instead of collapsing every link to 0.
                            m_currentLink = findOrAddLink(linkName);
                            m_currentLinkName = linkName;
                        }
                        break;
                    }

                    case ';':
                        if(!m_options.ignoreSize) {
                            m_state.fontSize = m_default.fontSize;
                            updateFont();
                        }
                        break;

                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9': {
                        ttstr digits{};
                        digits += ch;
                        auto rest = readUntil(text, i, ';');
                        for(int p = 0; p < rest.length(); ++p)
                            digits += rest[p];

                        if(!m_options.ignoreSize) {
                            bool hadDigits = false;
                            const int percent =
                                parseSignedDecimal(digits, 100, &hadDigits);
                            if(hadDigits) {
                                m_state.fontSize = m_default.fontSize *
                                                   static_cast<float>(percent) /
                                                   100.0f;
                                updateFont();
                            }
                        }
                        break;
                    }

                    default:
                        // The stripped DLL does not throw for an unknown %xx
                        // command; it skips payload to ';'.
                        while(i < len && text[i] != ';')
                            ++i;
                        break;
                }
                break;
            }

            case '\\': {
                if(!readchar(text, i, ch))
                    break;

                switch(ch) {
                    case 'n':
                        flush();
                        performLinebreak();
                        break;
                    case 't':
                        pushCharacter('\t');
                        break;
                    case 'i':
                        // DLL helper 0x100134c0: indent = current flow cursor.
                        m_indent = m_vertical ? m_y : m_x;
                        break;
                    case 'r':
                        m_indent = 0.0f;
                        break;
                    case 'w':
                        advanceWideSpace();
                        break;
                    case 'k':
                        // Key-wait marker. Exact host-facing array shape is
                        // still being reverse engineered.
                        break;
                    case 'x':
                        // Original clears two parser flags. No visible glyph.
                        break;
                    default:
                        pushCharacter(ch);
                        break;
                }
                break;
            }

            case '[': {
                auto ruby = readUntil(text, i, ']');
                if(m_options.ignoreRuby)
                    break;

                int comma = -1;
                for(int p = 0; p < ruby.length(); ++p) {
                    if(ruby[p] == ',') {
                        comma = p;
                        break;
                    }
                }

                m_pendingRuby = TJS_W("");
                m_pendingRubyCount = 0;
                if(comma < 0) {
                    m_pendingRuby = ruby;
                } else {
                    for(int p = 0; p < comma; ++p)
                        m_pendingRuby += ruby[p];

                    ttstr countText{};
                    for(int p = comma + 1; p < ruby.length(); ++p)
                        countText += ruby[p];
                    m_pendingRubyCount = parseSignedDecimal(countText, 0);
                }
                break;
            }

            case '#': {
                auto digits = readUntil(text, i, ';');
                if(!m_options.ignoreColor)
                    m_state.chColor =
                        parseHexColor(digits, m_default.chColor);
                break;
            }

            case '&': {
                auto graph = readUntil(text, i, ';');
                pushGraphicalCharacter(graph);
                break;
            }

            case '$': {
                // Original calls onEval for a non-empty payload and recursively
                // renders the returned text.  Callback plumbing is outside the
                // current reimplementation, but do not leak the markup into the
                // character stream.
                (void)readUntil(text, i, ';');
                break;
            }

            default:
                pushCharacter(ch);
                break;
        }
    }

    // Overflow is determined on the active flow axis. The DLL has independent
    // ignore_overx / ignore_overy flags.
    if(m_vertical) {
        m_overflow = !m_options.ignoreOverY && m_y > m_boxHeight;
    } else {
        m_overflow = !m_options.ignoreOverX && m_x > m_boxWidth;
    }
    m_state.renderOver = m_overflow;
    return !m_overflow;
}

void TextRenderBase::advanceWideSpace() {
    auto rasterizer = GetCurrentRasterizer();
    float advance = 0.0f;

    if(m_vertical) {
        // Exact DLL behavior: vertical \w uses fontScale * fontsize.
        advance = m_fontScale * m_state.fontSize;
    } else {
        // Exact DLL behavior asks text width of U+3000 and falls back to
        // fontsize if the callback reports zero.
        int width = 0;
        int height = 0;
        rasterizer->GetTextExtent(static_cast<tjs_char>(0x3000), width, height);
        advance = static_cast<float>(width);
        if(advance == 0.0f)
            advance = m_state.fontSize;
    }

    if(m_vertical)
        m_y += advance;
    else
        m_x += advance;
}

void TextRenderBase::performLinebreak() {
    flush(true);

    const float cross = std::max(m_lineCrossExtent, m_state.lineSize);
    if(m_vertical) {
        // Recovered vertical flow: columns advance from right to left.
        m_x -= cross + m_state.lineSpacing;
        m_y = m_indent;
    } else {
        // The DLL first places horizontal glyphs at
        //     y = currentY - glyphSize
        // and only when the line is finalized adds the resolved line height
        // to every glyph in that line.  Omitting this second stage makes
        // vertically-centered text sit about half an em too high.
        finalizeCurrentLine();

        m_x = m_indent;
        m_y += cross + m_state.lineSpacing;
    }

    ++m_lineIndex;
    m_lineCrossExtent = 0.0f;
    m_lineFinalized = false;
    m_isBeginningOfLine = true;
}

void TextRenderBase::pushGraphicalCharacter(const ttstr &graph) {
    if(graph.length() == 0)
        return;

    // The original calls onGetGraphSize and emits a graph CharacterInfo with
    // callback-provided REAL width/height. There is no graph-size callback in
    // this C++ wrapper yet, so emitting a fake zero/guessed size would corrupt
    // wrapping more than preserving the TODO. Log explicitly instead.
    LOGGER->warn("TextRenderBase: graph markup encountered; onGetGraphSize callback is not wired");
}

void TextRenderBase::pushCharacter(tjs_char ch) {
    if((0xD800 <= ch && ch <= 0xDBFF) ||
       (0xDC00 <= ch && ch <= 0xDFFF)) {
        TVPThrowExceptionMessage(TJS_W("unexpected character: surrogate pair"));
    }

    const bool isLeadingChar = findchInChars(m_options.leading, ch);
    const bool isFollowingChar = findchInChars(m_options.following, ch);
    const bool isIndent = findchInChars(m_options.begin, ch);
    const bool isIndentDecr = findchInChars(m_options.end, ch);

    uint32_t current = kTextRenderModeNormal;
    if(isLeadingChar)
        current = kTextRenderModeLeading;
    else if(isFollowingChar)
        current = kTextRenderModeFollowing;

    // Keep the original reimplementation's grouping idea, but all cursor
    // advances below now match the DLL (cw/size + pitch only).
    if(m_mode == kTextRenderModeFollowing || m_mode != kTextRenderModeLeading)
        flush();

    auto rasterizer = GetCurrentRasterizer();
    int advanceWidth = 0;
    int ignoredHeight = 0;
    rasterizer->GetTextExtent(ch, advanceWidth, ignoredHeight);

    // Hard-confirmed from the CharacterInfo construction path in the
    // original DLL (0x10012263..0x1001227d): `size` is not the rasterizer's
    // measured glyph height.  It is the current font size multiplied by the
    // TextRender fontScale.  This value is also what the horizontal placement
    // path subtracts from the baseline-like Y cursor.
    const float size = m_state.fontSize * m_fontScale;

    CharacterInfo info{};
    info.graph = false;
    info.text = ttstr() + ch;
    info.cw = static_cast<float>(advanceWidth);
    info.size = size;
    info.face = m_state.face;
    info.color = m_state.chColor;
    info.bold = m_state.bold;
    info.italic = m_state.italic;
    info.shadow = m_state.shadow;
    info.edge = m_state.edge;
    info.shadowColor = m_state.shadowColor;
    info.shadowDiff = m_state.shadowDiff;
    info.edgeColor = m_state.edgeColor;
    info.vertical = m_vertical;
    info.delay = m_elapsedDelay;
    info.link = m_currentLink;
    info.linkName = m_currentLinkName;
    info.lineIndex = m_lineIndex;
    info.flowAdvance = (m_vertical ? info.size : info.cw) + m_state.pitch;

    if(m_pendingRuby.length() > 0) {
        // High-confidence part: ruby is a CharacterInfo string and the parser
        // stores a separate integer counter.  Multi-character distribution is
        // not sufficiently proven to fake; attach once so the information is
        // preserved for the consumer.
        info.ruby = m_pendingRuby;
        m_pendingRuby = TJS_W("");
        m_pendingRubyCount = 0;
    }

    const float charDelay = getCharacterDelay(info);
    m_elapsedDelay += charDelay;

    m_buffer.push_back(std::move(info));

    if(m_autoIndent) {
        const float flowAdvance =
            m_vertical ? m_buffer.back().size : m_buffer.back().cw;

        if(m_isBeginningOfLine && m_autoIndent < 0) {
            if(m_vertical)
                m_y -= flowAdvance;
            else
                m_x -= flowAdvance;
        }

        if(isIndent)
            m_indent = (m_vertical ? m_y : m_x) + flowAdvance;

        if(isIndentDecr && m_indent > 0.0f) {
            flush();
            m_indent = 0.0f;
        }
    }

    m_mode = current;
    m_isBeginningOfLine = false;
}

void TextRenderBase::flush(bool force) {
    if(m_buffer.empty())
        return;

    for(size_t index = 0; index < m_buffer.size(); ++index) {
        auto &ch = m_buffer[index];

        if(m_vertical) {
            float newY = m_y + ch.flowAdvance;
            const bool over = m_boxHeight > 0.0f && newY > m_boxHeight &&
                              !m_options.ignoreOverY;

            if(over && !force) {
                // Move the entire grouped kinsoku buffer to the next column.
                const float cross = std::max(m_lineCrossExtent, m_state.lineSize);
                m_x -= cross + m_state.lineSpacing;
                m_y = m_indent;
                ++m_lineIndex;
                m_lineCrossExtent = 0.0f;

                for(auto &pending : m_buffer)
                    pending.lineIndex = m_lineIndex;
                flush(true);
                return;
            }

            // Vertical columns start at render width and move right-to-left.
            // The DLL's exact glyph-origin correction depends on its host
            // text-width callback; using x-cw keeps the record inside the box.
            ch.x = m_x - ch.cw;
            ch.y = m_y;
            m_y = newY;
            m_lineCrossExtent = std::max(m_lineCrossExtent, ch.cw);
        } else {
            float newX = m_x + ch.flowAdvance;
            const bool over = m_boxWidth > 0.0f && newX > m_boxWidth &&
                              !m_options.ignoreOverX;

            if(over && !force) {
                const float cross = std::max(m_lineCrossExtent, m_state.lineSize);

                // The buffered kinsoku group belongs on the next line, so
                // finalize the already-placed line before advancing currentY.
                finalizeCurrentLine();

                m_x = m_indent;
                m_y += cross + m_state.lineSpacing;
                ++m_lineIndex;
                m_lineCrossExtent = 0.0f;
                m_lineFinalized = false;

                for(auto &pending : m_buffer)
                    pending.lineIndex = m_lineIndex;
                flush(true);
                return;
            }

            ch.x = m_x;
            // Original TextRender.dll uses a baseline-like horizontal cursor:
            // CharacterInfo.y = currentY - CharacterInfo.size.
            // Keeping y=currentY shifts the whole visual glyph box downward
            // and then makes the final vertical block alignment offset wrong.
            ch.y = m_y - ch.size;
            m_x = newX;
            m_lineCrossExtent = std::max(m_lineCrossExtent, ch.size);
        }
    }

    for(auto &ch : m_buffer) {
        m_state.renderText += ch.text;
        m_characters.push_back(ch);
    }
    m_buffer.clear();
    m_lineFinalized = false;
    recomputeRenderBounds();
}

void TextRenderBase::finalizeCurrentLine() {
    if(m_vertical || m_lineFinalized)
        return;

    bool hasCharacters = false;
    for(const auto &ch : m_characters) {
        if(ch.lineIndex == m_lineIndex) {
            hasCharacters = true;
            break;
        }
    }

    if(!hasCharacters) {
        m_lineFinalized = true;
        return;
    }

    // Horizontal branch of the original 0x100111d0 line-finalizer:
    //
    //   lineHeight = max(max(CharacterInfo.size), lineSize)
    //   CharacterInfo.y += lineHeight
    //
    // CharacterInfo.y was initially written as currentY - size, so this
    // produces bottom-aligned glyphs inside the resolved line box:
    //
    //   finalY = currentY + lineHeight - size
    //
    // This is deliberately not an empirical visual offset.
    const float lineHeight =
        std::max(m_lineCrossExtent, m_state.lineSize);

    for(auto &ch : m_characters) {
        if(ch.lineIndex == m_lineIndex)
            ch.y += lineHeight / 2.0;
    }

    m_lineFinalized = true;
    recomputeRenderBounds();
}

void TextRenderBase::applyAlignment() {
    if(m_characters.empty()) {
        recomputeRenderBounds();
        return;
    }

    // Recovered line-alignment behavior (0x100111d0 / 0x100116fb):
    // align=-1 leaves the flow origin alone, align=0 offsets by half of the
    // remaining flow space, align=1 by all of it. Conversion is truncation
    // toward zero. In horizontal mode the flow axis is X; in vertical mode it
    // is Y. This is done independently for every line/column.
    if(m_alignment != kTextRenderAlignmentLeft) {
        size_t begin = 0;
        while(begin < m_characters.size()) {
            const int line = m_characters[begin].lineIndex;
            size_t end = begin + 1;
            while(end < m_characters.size() &&
                  m_characters[end].lineIndex == line)
                ++end;

            float logicalEnd = 0.0f;
            for(size_t i = begin; i < end; ++i) {
                const auto &ch = m_characters[i];
                const float endPos = m_vertical ? (ch.y + ch.flowAdvance)
                                                : (ch.x + ch.flowAdvance);
                logicalEnd = std::max(logicalEnd, endPos);
            }

            const float boxExtent = m_vertical ? m_boxHeight : m_boxWidth;
            const float remaining = boxExtent - logicalEnd;
            const float raw = m_alignment == kTextRenderAlignmentCenter
                                  ? remaining * 0.5f
                                  : remaining;
            const float offset = static_cast<float>(static_cast<int>(raw));

            if(offset != 0.0f) {
                for(size_t i = begin; i < end; ++i) {
                    if(m_vertical)
                        m_characters[i].y += offset;
                    else
                        m_characters[i].x += offset;
                }
            }

            begin = end;
        }
    }

    recomputeRenderBounds();

    if(m_vertical) {
        // This is a surprising but hard-confirmed quirk in the original DLL:
        // final cross-axis placement of a vertical block reads *align* again
        // (field +e0), not valign (+e8).  valign is not read by the vertical
        // done/alignment path.  Center uses -(width-renderLeft)/2 and Right
        // uses -(width-renderLeft), both truncated toward zero.
        float offsetX = 0.0f;
        if(m_alignment == kTextRenderAlignmentCenter) {
            const float raw = (m_boxWidth - m_renderLeft) * -0.5f;
            offsetX = static_cast<float>(static_cast<int>(raw));
        } else if(m_alignment == kTextRenderAlignmentRight) {
            const float raw = m_boxWidth - m_renderLeft;
            offsetX = -static_cast<float>(static_cast<int>(raw));
        }

        if(offsetX != 0.0f) {
            for(auto &ch : m_characters)
                ch.x += offsetX;
        }
    } else {
        // Horizontal mode uses valign for whole-block Y placement:
        //   -1 top, 0 middle, 1 bottom.
        // No clamp and no empirical -7 adjustment exist in the DLL.
        float offsetY = 0.0f;
        if(m_valign == kTextRenderValignMiddle) {
            const float raw = (m_boxHeight - m_renderBottom) * 0.5f;
            offsetY = static_cast<float>(static_cast<int>(raw));
        } else if(m_valign == kTextRenderValignBottom) {
            const float raw = m_boxHeight - m_renderBottom;
            offsetY = static_cast<float>(static_cast<int>(raw));
        }

        if(offsetY != 0.0f) {
            for(auto &ch : m_characters)
                ch.y += offsetY;
        }
    }

    recomputeRenderBounds();
}

void TextRenderBase::setRenderSize(int width, int height) {
    m_boxWidth = static_cast<float>(width);
    m_boxHeight = static_cast<float>(height);

    dbg_print(
        TVPFormatMessage(TJS_W("set render size: (%1, %2)"), width, height));
    clear();
}

void TextRenderBase::setDefault(tTJSVariant defaultSettings) {
    dbg_print(TJS_W("set default format"));

    auto dict = defaultSettings.AsObjectNoAddRef();
    if(!dict)
        return;

    m_default.deserializeDefault(defaultSettings);

    int align = static_cast<int>(m_defaultAlignment);
    if(getIntProp(dict, TJS_W("align"), align))
        m_defaultAlignment = static_cast<TextRenderAlignment>(align);

    int valign = static_cast<int>(m_defaultValign);
    if(getIntProp(dict, TJS_W("valign"), valign))
        m_defaultValign = static_cast<TextRenderValign>(valign);
}

void TextRenderBase::setOption(tTJSVariant options) {
    dbg_print(TJS_W("set option"));
    m_options.deserialize(options);
    m_vertical = m_options.vertical;
}

void TextRenderBase::setFont(tTJSVariant fontSettings) {
    auto dict = fontSettings.AsObjectNoAddRef();
    if(!dict)
        return;

    bool update = false;

    ttstr face = m_state.face;
    if(getStringProp(dict, TJS_W("face"), face)) {
        m_state.face = face;
        update = true;
    }

    bool bold = m_state.bold;
    if(getBoolProp(dict, TJS_W("bold"), bold)) {
        m_state.bold = bold;
        update = true;
    }

    bool italic = m_state.italic;
    if(getBoolProp(dict, TJS_W("italic"), italic)) {
        m_state.italic = italic;
        update = true;
    }

    float fontSize = m_state.fontSize;
    if(getRealProp(dict, TJS_W("fontsize"), fontSize)) {
        m_state.fontSize = fontSize;
        update = true;
    }

    getRealProp(dict, TJS_W("rubysize"), m_state.rubySize);
    getRealProp(dict, TJS_W("rubyoffset"), m_state.rubyOffset);
    getColorProp(dict, TJS_W("color"), m_state.chColor);
    getBoolProp(dict, TJS_W("shadow"), m_state.shadow);
    getColorProp(dict, TJS_W("shadowcolor"), m_state.shadowColor);
    getIntProp(dict, TJS_W("shadowdiff"), m_state.shadowDiff);
    getBoolProp(dict, TJS_W("edge"), m_state.edge);
    getColorProp(dict, TJS_W("edgecolor"), m_state.edgeColor);

    if(update)
        updateFont();
}

void TextRenderBase::setStyle(tTJSVariant styleSettings) {
    auto dict = styleSettings.AsObjectNoAddRef();
    if(!dict)
        return;

    getRealProp(dict, TJS_W("linespacing"), m_state.lineSpacing);
    getRealProp(dict, TJS_W("pitch"), m_state.pitch);

    float lineSize = m_state.lineSize;
    const bool hasLineSize =
        getRealProp(dict, TJS_W("linesize"), lineSize);
    if(hasLineSize) {
        m_state.lineSize = lineSize;
    } else {
        // Recovered setStyle behavior: fontsize acts as the linesize fallback.
        float fontSize = 0.0f;
        if(getRealProp(dict, TJS_W("fontsize"), fontSize))
            m_state.lineSize = fontSize;
    }

    int align = static_cast<int>(m_alignment);
    if(getIntProp(dict, TJS_W("align"), align))
        m_alignment = static_cast<TextRenderAlignment>(align);

    int valign = static_cast<int>(m_valign);
    if(getIntProp(dict, TJS_W("valign"), valign))
        m_valign = static_cast<TextRenderValign>(valign);
}

tTJSVariant TextRenderBase::getCharacters(int start, int count) {
    auto array = TJSCreateArrayObject();

    if(start < 0)
        start = 0;

    const int total = static_cast<int>(m_characters.size());
    if(start > total)
        start = total;

    // DLL range helper: the second argument is a count, not an end index;
    // count==0 means "the rest".
    if(count <= 0 || start + count > total)
        count = total - start;

    for(int i = 0; i < count; ++i) {
        auto character = m_characters[static_cast<size_t>(start + i)].serialize();
        array->PropSetByNum(TJS_MEMBERENSURE, i, &character, array);
    }

    auto result = tTJSVariant(array, array);
    array->Release();
    return result;
}

std::vector<LinkRect> TextRenderBase::collectLinkRects(int link) const {
    std::vector<LinkRect> rects;
    if(link < 0 || static_cast<size_t>(link) >= m_linkNames.size())
        return rects;

    // The DLL stores a per-link vector of [left, top, right, bottom] REAL
    // rectangles (record size 0x10; see 0x1000d380/0x100040a0).  Rebuild the
    // same semantic data from our final CharacterInfo positions. Adjacent
    // characters in one link run and one line/column share a single rect;
    // reopening the same named link later creates another rect.
    bool open = false;
    size_t previousIndex = 0;
    int previousLine = -1;
    LinkRect current{};

    for(size_t i = 0; i < m_characters.size(); ++i) {
        const auto &ch = m_characters[i];
        if(ch.link != link)
            continue;

        const LinkRect glyph{ch.x, ch.y, ch.x + ch.cw, ch.y + ch.size};
        const bool contiguous =
            open && i == previousIndex + 1 && ch.lineIndex == previousLine;

        if(!contiguous) {
            if(open)
                rects.push_back(current);
            current = glyph;
            open = true;
        } else {
            current.left = std::min(current.left, glyph.left);
            current.top = std::min(current.top, glyph.top);
            current.right = std::max(current.right, glyph.right);
            current.bottom = std::max(current.bottom, glyph.bottom);
        }

        previousIndex = i;
        previousLine = ch.lineIndex;
    }

    if(open)
        rects.push_back(current);
    return rects;
}

tTJSVariant TextRenderBase::getLinkNames() {
    // Actual member bound as getLinkNames is 0x10003d30. It iterates the
    // stable link-name table in insertion order and returns a TJS array.
    auto array = TJSCreateArrayObject();
    for(size_t i = 0; i < m_linkNames.size(); ++i) {
        tTJSVariant value(m_linkNames[i]);
        array->PropSetByNum(TJS_MEMBERENSURE, static_cast<tjs_int>(i), &value,
                            array);
    }
    auto result = tTJSVariant(array, array);
    array->Release();
    return result;
}

tTJSVariant TextRenderBase::getLinkRects(int link) {
    // DLL 0x100040a0 serializes each internal REAL rect as
    // { left, top, width = right-left, height = bottom-top }.
    auto array = TJSCreateArrayObject();
    const auto rects = collectLinkRects(link);
    for(size_t i = 0; i < rects.size(); ++i) {
        const auto &rect = rects[i];
        auto dict = TJSCreateDictionaryObject();
        setRealProp(dict, TJS_W("left"), rect.left);
        setRealProp(dict, TJS_W("top"), rect.top);
        setRealProp(dict, TJS_W("width"), rect.right - rect.left);
        setRealProp(dict, TJS_W("height"), rect.bottom - rect.top);
        tTJSVariant value(dict, dict);
        dict->Release();
        array->PropSetByNum(TJS_MEMBERENSURE, static_cast<tjs_int>(i), &value,
                            array);
    }
    auto result = tTJSVariant(array, array);
    array->Release();
    return result;
}

tTJSVariant TextRenderBase::getLinkCharacters(int link) {
    // DLL 0x10004390 serializes the character indices stored for one link.
    // Filtering by CharacterInfo.link is equivalent for this port.
    auto array = TJSCreateArrayObject();
    tjs_int out = 0;
    if(link >= 0 && static_cast<size_t>(link) < m_linkNames.size()) {
        for(const auto &ch : m_characters) {
            if(ch.link != link)
                continue;
            auto value = ch.serialize();
            array->PropSetByNum(TJS_MEMBERENSURE, out++, &value, array);
        }
    }
    auto result = tTJSVariant(array, array);
    array->Release();
    return result;
}

bool TextRenderBase::isLinkContains(int link, double x, double y) const {
    // Exact signature/shape from 0x10013a90 -> 0x1000d380:
    // (link, x, y), testing all stored link rectangles.
    const float px = static_cast<float>(x);
    const float py = static_cast<float>(y);
    for(const auto &rect : collectLinkRects(link)) {
        if(rect.left <= px && px <= rect.right && rect.top <= py &&
           py <= rect.bottom)
            return true;
    }
    return false;
}

int TextRenderBase::getLinkOfPosition(double x, double y) const {
    // DLL 0x10004420/0x10013af0 returns the first matching link index, else -1.
    for(size_t i = 0; i < m_linkNames.size(); ++i) {
        if(isLinkContains(static_cast<int>(i), x, y))
            return static_cast<int>(i);
    }
    return -1;
}

void TextRenderBase::clear() {
    dbg_print(TJS_W("clear character buffer and format"));

    m_characters.clear();
    m_buffer.clear();

    resetFont();
    resetStyle();

    m_state.renderText = TJS_W("");
    m_state.renderOver = false;
    m_overflow = false;

    m_x = m_vertical ? m_boxWidth : 0.0f;
    m_y = 0.0f;
    m_indent = 0.0f;

    // Exact clear() seed recovered at 0x1000da70: horizontal bounds start
    // at 0; vertical bounds start at boxWidth.
    const float boundSeed = m_vertical ? m_boxWidth : 0.0f;
    m_renderLeft = boundSeed;
    m_renderTop = boundSeed;
    m_renderRight = boundSeed;
    m_renderBottom = boundSeed;
    m_autoIndent = 0;
    m_mode = kTextRenderModeLeading;
    m_lineIndex = 0;
    m_lineCrossExtent = 0.0f;
    m_lineFinalized = false;
    m_isBeginningOfLine = true;
    m_elapsedDelay = 0.0f;
    m_pendingRuby = TJS_W("");
    m_pendingRubyCount = 0;
    m_currentLink = -1;
    m_currentLinkName = TJS_W("");
    m_linkNames.clear();

    updateFont();
}

void TextRenderBase::recomputeRenderBounds() {
    if(m_characters.empty()) {
        const float seed = m_vertical ? m_boxWidth : 0.0f;
        m_renderLeft = seed;
        m_renderTop = seed;
        m_renderRight = seed;
        m_renderBottom = seed;
        return;
    }

    float left = m_characters.front().x;
    float top = m_characters.front().y;
    float right = m_characters.front().x + m_characters.front().cw;
    float bottom = m_characters.front().y + m_characters.front().size;

    for(const auto &ch : m_characters) {
        left = std::min(left, ch.x);
        top = std::min(top, ch.y);
        right = std::max(right, ch.x + ch.cw);
        bottom = std::max(bottom, ch.y + ch.size);
    }

    m_renderLeft = left;
    m_renderTop = top;
    m_renderRight = right;
    m_renderBottom = bottom;
}

static bool textEquals(const ttstr &a, const ttstr &b) {
    if(a.length() != b.length())
        return false;
    for(int i = 0; i < a.length(); ++i) {
        if(a[i] != b[i])
            return false;
    }
    return true;
}

int TextRenderBase::findOrAddLink(const ttstr &name) {
    for(size_t i = 0; i < m_linkNames.size(); ++i) {
        if(textEquals(m_linkNames[i], name))
            return static_cast<int>(i);
    }

    m_linkNames.push_back(name);
    return static_cast<int>(m_linkNames.size() - 1);
}

void TextRenderBase::updateFont() {
    auto rasterizer = GetCurrentRasterizer();

    // FontRasterizer/tTVPFont itself is integer-height based even though the
    // original TextRender DLL stores fontsize as float.  Keep the layout state
    // fractional and only convert at this adapter boundary.
    const int height = std::max(1, static_cast<int>(m_state.fontSize));
    auto font = tTVPFont{
        height,
        static_cast<tjs_uint32>((m_state.bold ? TVP_TF_BOLD : 0) |
                                (m_state.italic ? TVP_TF_ITALIC : 0)),
        0,
        m_state.face,
    };

    rasterizer->ApplyFont(font);
}

void TextRenderBase::done() {
    dbg_print(TJS_W("flush character buffer"));
    flush();

    // done() in the DLL finalizes the current line before the whole-block
    // align/valign pass.  This is essential for correct horizontal Y.
    finalizeCurrentLine();

    applyAlignment();
}

void TextRenderBase::newline() {
    flush();
    performLinebreak();
}

void TextRenderBase::resetFont() {
    // Recovered resetFont(): font/type/color/effect/ruby only. Paragraph style
    // is NOT reset here.
    m_state.face = m_default.face;
    m_state.bold = m_default.bold;
    m_state.italic = m_default.italic;
    m_state.fontSize = m_default.fontSize;
    m_state.rubySize = m_default.rubySize;
    m_state.rubyOffset = m_default.rubyOffset;
    m_state.chColor = m_default.chColor;
    m_state.shadow = m_default.shadow;
    m_state.shadowColor = m_default.shadowColor;
    m_state.shadowDiff = m_default.shadowDiff;
    m_state.edge = m_default.edge;
    m_state.edgeColor = m_default.edgeColor;
    updateFont();
}

void TextRenderBase::resetStyle() {
    // Recovered resetStyle(): paragraph style only.
    m_state.lineSize = m_default.lineSize;
    m_state.lineSpacing = m_default.lineSpacing;
    m_state.pitch = m_default.pitch;
    m_alignment = m_defaultAlignment;
    m_valign = m_defaultValign;
}

tTJSVariant TextRenderBase::getKeyWait() {
    // The original has a non-trivial wait table populated by %w/%D/timing
    // commands. Its element schema is not yet proven, so returning an empty
    // array is safer than exposing fabricated records.
    auto array = TJSCreateArrayObject();
    auto result = tTJSVariant(array, array);
    array->Release();
    return result;
}

int TextRenderBase::calcShowCount(int time) {
    // Exact high-level behavior recovered from 0x10010f50: scan backwards and
    // return the first index+1 whose character timestamp, after applying the
    // class timeScale, is <= the requested time.
    const float queryTime = static_cast<float>(time);
    for(int i = static_cast<int>(m_characters.size()) - 1; i >= 0; --i) {
        const float shownAt = m_characters[static_cast<size_t>(i)].delay *
                              m_timeScale;
        if(shownAt <= queryTime)
            return i + 1;
    }
    return 0;
}

bool TextRenderBase::contains(double x, double y) const {
    // DLL @ 0x10001820 performs an inclusive content-bounds test.
    const float px = static_cast<float>(x);
    const float py = static_cast<float>(y);
    return m_renderLeft <= px && px <= m_renderRight &&
           m_renderTop <= py && py <= m_renderBottom;
}

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

NCB_REGISTER_CLASS(TextRenderBase) {
    Constructor();

    NCB_METHOD(render);
    NCB_METHOD(setRenderSize);
    NCB_METHOD(setDefault);
    NCB_METHOD(setOption);
    NCB_METHOD(setFont);
    NCB_METHOD(setStyle);
    NCB_METHOD(getCharacters);
    NCB_METHOD(getLinkNames);
    NCB_METHOD(getLinkRects);
    NCB_METHOD(getLinkCharacters);
    NCB_METHOD(isLinkContains);
    NCB_METHOD(getLinkOfPosition);
    NCB_METHOD(clear);
    NCB_METHOD(done);
    NCB_METHOD(newline);
    NCB_METHOD(resetFont);
    NCB_METHOD(resetStyle);
    NCB_METHOD(getKeyWait);
    NCB_METHOD(calcShowCount);
    NCB_METHOD(contains);

    Property(TJS_W("keyWait"), &Class::getKeyWait, &Class::setKeyWait);

    property_delegate(renderCount);
    property_delegate(renderOver);
    property_delegate(renderDelay);
    property_delegate(renderText);
    property_delegate(renderLeft);
    property_delegate(renderRight);
    property_delegate(renderTop);
    property_delegate(renderBottom);
    property_delegate(maxScrollOffset);

    property_delegate(vertical);
    property_delegate(timeScale);
    property_delegate(fontScale);

    property_delegate(bold);
    property_delegate(italic);
    property_delegate(face);

    // The DLL does NOT register lowercase active font/style properties.
    // Lowercase names (fontsize, bigfontsize, color, linespacing, ...) are
    // dictionary keys consumed by setDefault/setFont/setStyle.  The existing
    // port did expose active properties, so retain its camelCase API below as
    // a compatibility extension rather than pretending it is original ABI.
    property_delegate(shadow);
    property_delegate(edge);
    property_delegate(pitch);
    property_delegate(align);
    property_delegate(valign);

    // Compatibility-only active properties from the current port.
    property_delegate(fontSize);
    property_delegate(bigFontSize);
    property_delegate(smallFontSize);
    property_delegate(chColor);
    property_delegate(rubySize);
    property_delegate(rubyOffset);
    property_delegate(shadowColor);
    property_delegate(shadowDiff);
    property_delegate(edgeColor);
    property_delegate(lineSpacing);
    property_delegate(lineSize);

    property_delegate(defaultBold);
    property_delegate(defaultItalic);
    property_delegate(defaultFace);
    property_delegate(defaultFontSize);
    property_delegate(defaultBigFontSize);
    property_delegate(defaultSmallFontSize);
    property_delegate(defaultChColor);
    property_delegate(defaultRubySize);
    property_delegate(defaultRubyOffset);
    property_delegate(defaultShadow);
    property_delegate(defaultShadowColor);
    property_delegate(defaultShadowDiff);
    property_delegate(defaultEdge);
    property_delegate(defaultEdgeColor);
    property_delegate(defaultLineSpacing);
    property_delegate(defaultPitch);
    property_delegate(defaultLineSize);
    property_delegate(defaultAlign);
    property_delegate(defaultValign);
}
