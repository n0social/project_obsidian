#include "ui/framexml_emitter.hpp"

#include "ui/xml_parser.hpp"

#include <algorithm>
#include <sstream>

namespace wowee {
namespace ui {

namespace {

/// Element names that produce a frame rather than a region. Everything here is
/// created through CreateFrame with its own type, so a Button gets a Button's
/// behaviour even where the widget system does not yet distinguish them.
bool isFrameElement(const std::string& n) {
    static const char* kFrames[] = {
        "Frame", "Button", "CheckButton", "StatusBar", "Slider", "EditBox",
        "ScrollFrame", "ScrollingMessageFrame", "MessageFrame", "SimpleHTML",
        "ColorSelect", "Model", "PlayerModel", "DressUpModel", "TabardModel",
        "Cooldown", "GameTooltip", "MovieFrame", "ArchaeologyDigSiteFrame"
    };
    for (const char* f : kFrames) if (n == f) return true;
    return false;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    out += "\"";
    return out;
}

/// A <Size> or <Offset> can be written as a child <AbsDimension x= y=> or, in
/// later files, as attributes directly on the element.
bool readDimension(const XmlNode& node, float& x, float& y) {
    if (const XmlNode* abs = node.child("AbsDimension")) {
        x = abs->attrFloat("x", 0.0f);
        y = abs->attrFloat("y", 0.0f);
        return true;
    }
    if (node.attr("x") || node.attr("y")) {
        x = node.attrFloat("x", 0.0f);
        y = node.attrFloat("y", 0.0f);
        return true;
    }
    return false;
}


/// The argument names a handler's body expects to find in scope. Blizzard's
/// inline scripts use them without declaring them, so they have to be the
/// function's parameters.
std::string scriptParameters(const std::string& script) {
    if (script == "OnUpdate")        return "self, elapsed";
    if (script == "OnEvent")         return "self, event, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9";
    if (script == "OnClick")         return "self, button, down";
    if (script == "OnDoubleClick")   return "self, button";
    if (script == "OnMouseDown" ||
        script == "OnMouseUp")       return "self, button";
    if (script == "OnDragStart" ||
        script == "OnDragStop" ||
        script == "OnReceiveDrag")   return "self, button";
    if (script == "OnEnter" ||
        script == "OnLeave")         return "self, motion";
    if (script == "OnChar")          return "self, text";
    if (script == "OnKeyDown" ||
        script == "OnKeyUp")         return "self, key";
    if (script == "OnValueChanged")  return "self, value";
    if (script == "OnTextChanged")   return "self, isUserInput";
    if (script == "OnMouseWheel")    return "self, delta";
    if (script == "OnSizeChanged")   return "self, width, height";
    if (script == "OnAttributeChanged") return "self, name, value";
    if (script == "OnHyperlinkClick" ||
        script == "OnHyperlinkEnter" ||
        script == "OnHyperlinkLeave") return "self, link, text, button";
    if (script == "OnTooltipSetItem" ||
        script == "OnTooltipSetUnit") return "self";
    // Scrolling. UIPanelScrollFrameTemplate's own OnVerticalScroll body opens
    // with scrollbar:SetValue(offset), and without the name that was nil on
    // every scroll frame in the interface.
    if (script == "OnVerticalScroll" ||
        script == "OnHorizontalScroll") return "self, offset";
    if (script == "OnScrollRangeChanged") return "self, xrange, yrange";
    if (script == "OnCursorChanged")   return "self, x, y, width, height";
    if (script == "OnColorSelect")     return "self, r, g, b";
    if (script == "OnMinMaxChanged")   return "self, min, max";
    if (script == "OnTooltipAddMoney") return "self, cost, maxcost";
    if (script == "OnTooltipSetDefaultAnchor") return "self, parent";
    if (script == "OnMovieShowSubtitle")       return "self, text";
    if (script == "OnInputLanguageChanged")    return "self, language";
    if (script == "OnFinished")        return "self, requested";
    // OnLoad, OnShow, OnHide and the rest take only self.
    return "self";
}

/// Named parameters, and then varargs regardless. A body is free to use `...`
/// whatever handler it belongs to, and a parameter list without it does not
/// merely leave the values behind — it fails to compile, taking the whole
/// template with it.
std::string scriptSignature(const std::string& script) {
    return scriptParameters(script) + ", ...";
}

/// Statements that publish the 1.12 handler globals. `this` is never a
/// parameter, so a plain assignment reaches the global. OnEvent's arg1 is a
/// parameter, so it has to be copied onto _G or a function the body calls
/// still sees the global as nil.
std::string scriptGlobalPreamble(const std::string& script) {
    std::string p = "this = self; ";
    if (script == "OnUpdate") {
        p += "arg1 = elapsed; ";
    } else if (script == "OnClick" || script == "OnDoubleClick" ||
               script == "OnMouseDown" || script == "OnMouseUp" ||
               script == "OnDragStart" || script == "OnDragStop" ||
               script == "OnReceiveDrag") {
        p += "arg1 = button; ";
    } else if (script == "OnValueChanged") {
        p += "arg1 = value; ";
    } else if (script == "OnChar") {
        p += "arg1 = text; ";
    } else if (script == "OnMouseWheel") {
        p += "arg1 = delta; ";
    } else if (script == "OnVerticalScroll" || script == "OnHorizontalScroll") {
        p += "arg1 = offset; ";
    } else if (script == "OnEvent") {
        p += "_G.event = event; _G.arg1 = arg1; _G.arg2 = arg2; _G.arg3 = arg3; "
             "_G.arg4 = arg4; _G.arg5 = arg5; _G.arg6 = arg6; _G.arg7 = arg7; "
             "_G.arg8 = arg8; _G.arg9 = arg9; ";
    }
    return p;
}

struct Emitter {
    EmitResult result;
    int temp = 0;
    /// True while emitting a template body. Inside one the owning frame is not
    /// known until the template is replayed, so $parent has to be resolved then
    /// rather than now.
    bool runtimeParentName = false;

    /// Temporaries live in a table rather than in locals. Lua allows 200 locals
    /// per function and a large file declares far more widgets than that —
    /// FriendsFrame and InterfaceOptionsPanels both went over, and the failure
    /// is the whole chunk refusing to compile rather than anything degrading.
    std::string nextVar() { return "__w[" + std::to_string(++temp) + "]"; }

    /// The Lua expression for a region or frame's name. A literal where the
    /// owning frame is known, and a concatenation against the real frame's name
    /// where it is not — which is what makes $parentBackdrop inside a template
    /// become FooFrameBackdrop on the frame that inherits it, rather than
    /// naming itself after the template.
    std::string nameArg(const std::string& rawName, const std::string& parentName,
                        const std::string& selfVar) {
        if (rawName.empty()) return "nil";
        const std::string token = "$parent";
        const bool isParented = rawName.compare(0, token.size(), token) == 0;
        if (!isParented) return quote(rawName);
        const std::string suffix = rawName.substr(token.size());
        if (runtimeParentName) {
            return "((" + selfVar + ":GetName() or \"\") .. " + quote(suffix) + ")";
        }
        return quote(parentName + suffix);
    }

    void line(const std::string& s) { result.lua += s; result.lua += "\n"; }

    void emitScripts(const XmlNode& scripts, const std::string& var) {
        for (const XmlNode& s : scripts.children) {
            // <OnClick function="Foo"/> names an existing global; an inline body
            // is a function literal. Both end up as the same SetScript call.
            // Present but empty is not a name. Emitted as one it produces
            // SetScript("X", ) — a syntax error that loses the whole file, not
            // just the handler.
            if (const std::string* fn = s.attr("function"); fn && !fn->empty()) {
                line(var + ":SetScript(" + quote(s.name) + ", " + *fn + ")");
                continue;
            }
            std::string body = s.text;
            if (body.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            // Each handler's arguments have names, and the body uses them
            // directly without declaring them: an OnUpdate says `elapsed`, an
            // OnClick says `button`. Passing them positionally as arg1..argN
            // left those names nil, so every one of these bodies failed the
            // moment it touched its own argument — arithmetic on a nil elapsed
            // being the loudest of them.
            // 1.12 scripts read the frame as the global `this`, and the first
            // handler argument as the global `arg1`. The names in the parameter
            // list only cover a body that uses those names directly. A body
            // that calls WorldStateAlwaysUpFrame_OnLoad() or
            // ExhaustionTick_OnUpdate(arg1) is talking to the globals.
            line(var + ":SetScript(" + quote(s.name) +
                 ", function(" + scriptSignature(s.name) + ") " +
                 scriptGlobalPreamble(s.name) + body + " end)");
        }
    }

    /// Returns the variable holding the region, so a caller that has to hand it
    /// to a setter afterwards — button art does — can name it. isTexture is
    /// explicit because button art does not carry it in the element name:
    /// <NormalTexture> is a texture and <ButtonText> a font string.
    std::string emitRegion(const XmlNode& node, const std::string& parentVar,
                           const std::string& parentName, const std::string& layerName,
                           bool isTexture, const std::string& nameVar = std::string()) {
        const std::string var = nextVar();
        const std::string rawName = node.attrOr("name", "");
        const std::string name = substituteParent(rawName, parentName);

        line(var + " = " + parentVar +
             (isTexture ? ":CreateTexture(" : ":CreateFontString(") +
             nameArg(rawName, parentName, nameVar.empty() ? parentVar : nameVar) +
             ", " + quote(layerName) + ")");

        emitParentKey(node, var, parentVar);

        if (const std::string* file = node.attr("file")) {
            line(var + ":SetTexture(" + quote(*file) + ")");
        }
        if (const std::string* text = node.attr("text")) {
            line(var + ":SetText(" + quote(*text) + ")");
        }
        if (const std::string* j = node.attr("justifyH")) {
            line(var + ":SetJustifyH(" + quote(*j) + ")");
        }
        // A FontString inherits a shared font object rather than a template,
        // and that is where its size and colour come from. FrameXML does this
        // on nearly every label it declares.
        if (const std::string* inh = node.attr("inherits")) {
            if (!isTexture) line(var + ":SetFontObject(" + quote(*inh) + ")");
        }
        // A texture given neither a size nor anchors fills its parent in the
        // real client. PlayerFrameTexture, the portrait ring and plate, is
        // declared exactly that way and otherwise laid out at 0x0.
        const bool fillsParent = node.attrBool("setAllPoints") ||
            (isTexture && !node.child("Size") && !node.child("Anchors"));
        if (fillsParent) {
            line(var + ":SetAllPoints(" + parentVar + ")");
        }
        if (node.attrBool("hidden")) line(var + ":Hide()");

        if (const XmlNode* size = node.child("Size")) {
            float w = 0, h = 0;
            if (readDimension(*size, w, h))
                line(var + ":SetSize(" + std::to_string(w) + ", " + std::to_string(h) + ")");
        }
        if (const XmlNode* tc = node.child("TexCoords")) {
            line(var + ":SetTexCoord(" + std::to_string(tc->attrFloat("left", 0.0f)) + ", " +
                 std::to_string(tc->attrFloat("right", 1.0f)) + ", " +
                 std::to_string(tc->attrFloat("top", 0.0f)) + ", " +
                 std::to_string(tc->attrFloat("bottom", 1.0f)) + ")");
        }
        if (const XmlNode* col = node.child("Color")) {
            const std::string args =
                std::to_string(col->attrFloat("r", 1.0f)) + ", " +
                std::to_string(col->attrFloat("g", 1.0f)) + ", " +
                std::to_string(col->attrFloat("b", 1.0f)) + ", " +
                std::to_string(col->attrFloat("a", 1.0f));
            line(var + (isTexture ? ":SetVertexColor(" : ":SetTextColor(") + args + ")");
        }
        if (const XmlNode* anchors = node.child("Anchors"))
            emitAnchors(*anchors, var, parentVar, parentName);
        return var;
    }

    /// Button art declared as its own element rather than inside a Layer.
    ///
    /// These are ordinary regions with an implied draw layer and a setter to
    /// call afterwards, and the emitter used to ignore all of them. That lost
    /// the names they declare: a button whose <ButtonText name="$parentNormalText">
    /// was dropped leaves _G["DropDownList1Button1NormalText"] undefined, and
    /// with the API fallback on the lookup answers with a function instead of
    /// failing outright. HighlightTexture alone appears in 62 FrameXML files.
    void emitButtonRegions(const XmlNode& node, const std::string& var,
                           const std::string& name) {
        struct Slot { const char* element; const char* setter;
                      const char* layer; bool isTexture; };
        static constexpr Slot kSlots[] = {
            {"NormalTexture",          "SetNormalTexture",          "ARTWORK",   true},
            {"PushedTexture",          "SetPushedTexture",          "ARTWORK",   true},
            {"DisabledTexture",        "SetDisabledTexture",        "ARTWORK",   true},
            {"CheckedTexture",         "SetCheckedTexture",         "ARTWORK",   true},
            {"DisabledCheckedTexture", "SetDisabledCheckedTexture", "ARTWORK",   true},
            // Drawn above the button's own art rather than beside it, which is
            // the whole point of the highlight layer.
            {"HighlightTexture",       "SetHighlightTexture",       "HIGHLIGHT", true},
            // Over the art, so a label is never hidden by the face beneath it.
            {"ButtonText",             "SetFontString",             "OVERLAY",   false},
            // A slider's grip, which draws over the channel it runs in.
            {"ThumbTexture",           "SetThumbTexture",           "OVERLAY",   true},
        };
        for (const Slot& slot : kSlots) {
            const XmlNode* child = node.child(slot.element);
            if (!child) continue;
            const std::string regionVar =
                emitRegion(*child, var, name, slot.layer, slot.isTexture);
            line(var + ":" + slot.setter + "(" + regionVar + ")");
        }
    }

    void emitAnchors(const XmlNode& anchors, const std::string& var,
                     const std::string& parentVar,
                     const std::string& parentNameForAnchors = std::string()) {
        for (const XmlNode& a : anchors.children) {
            if (a.name != "Anchor") continue;
            const std::string point = a.attrOr("point", "CENTER");
            // relativeTo names a frame. Emitted as a string rather than a bare
            // identifier, because SetPoint resolves a name for us and because
            // the name is often $parentSomething — which is not an identifier
            // at all, and pasting it into Lua is a syntax error that loses the
            // whole file. Without one, the anchor is to the parent, which is
            // what leaving it out means.
            std::string relative = parentVar;
            if (const std::string* rt = a.attr("relativeTo")) {
                // Resolved against whatever owns this anchor, which is parentVar
                // — the containing frame for a region, and the parent frame for
                // a frame's own anchors. It used to say "self" regardless, which
                // inside a template asked the wrong frame for its name.
                relative = nameArg(*rt, parentNameForAnchors, parentVar);
                if (relative == "nil") relative = parentVar;
            }
            const std::string relPoint = a.attrOr("relativePoint", point);

            float ox = 0, oy = 0;
            if (const XmlNode* off = a.child("Offset")) readDimension(*off, ox, oy);
            if (a.attr("x") || a.attr("y")) {
                ox = a.attrFloat("x", ox);
                oy = a.attrFloat("y", oy);
            }
            line(var + ":SetPoint(" + quote(point) + ", " + relative + ", " +
                 quote(relPoint) + ", " + std::to_string(ox) + ", " + std::to_string(oy) + ")");
        }
    }

    /// Binds a region or frame to a named field on the frame containing it.
    ///
    /// parentKey="icon" means the owner can say self.icon rather than looking
    /// the name up, and FrameXML's own handlers do exactly that:
    /// QuestHonorFrameTemplate's OnLoad opens with self.icon:SetTexture(...).
    /// Ignoring the attribute left every one of those fields nil — 242 of them
    /// across 31 files.
    ///
    /// Written in brackets because the key is arbitrary text, and a key that
    /// happens to be a Lua keyword would otherwise not parse.
    void emitParentKey(const XmlNode& node, const std::string& var,
                       const std::string& parentVar) {
        const std::string* key = node.attr("parentKey");
        if (!key || key->empty() || parentVar.empty()) return;
        line(parentVar + "[" + quote(*key) + "] = " + var);
    }

    /// A <Font> is not a widget — it is a named set of type settings that font
    /// strings inherit by name, and SetFontObject reads height and colour off
    /// it. Ignoring the element left all 42 of FrameXML's font objects
    /// undefined, so every label that inherits one fell back to a default size
    /// and colour it was never meant to have.
    void emitFont(const XmlNode& node) {
        const std::string name = node.attrOr("name", "");
        if (name.empty()) return;

        float height = 0.0f;
        if (const XmlNode* fh = node.child("FontHeight")) {
            if (const XmlNode* abs = fh->child("AbsValue"))
                height = abs->attrFloat("val", 0.0f);
            else
                height = fh->attrFloat("val", 0.0f);
        }

        // Inheriting copies the settings first, so anything stated here wins —
        // the same order a frame's template follows.
        if (const std::string* inh = node.attr("inherits"); inh && !inh->empty()) {
            line(name + " = {}");
            line("do local base = _G[" + quote(*inh) + "]");
            line("  if type(base) == 'table' then");
            line("    for k, v in pairs(base) do " + name + "[k] = v end");
            line("  end");
            line("end");
        } else {
            line(name + " = " + name + " or {}");
        }
        if (height > 0.0f) line(name + ".height = " + std::to_string(height));
        if (const std::string* f = node.attr("font"))
            line(name + ".font = " + quote(*f));
        if (const std::string* o = node.attr("outline"))
            line(name + ".outline = " + quote(*o));
        if (const XmlNode* col = node.child("Color")) {
            line(name + ".r = " + std::to_string(col->attrFloat("r", 1.0f)));
            line(name + ".g = " + std::to_string(col->attrFloat("g", 1.0f)));
            line(name + ".b = " + std::to_string(col->attrFloat("b", 1.0f)));
            line(name + ".a = " + std::to_string(col->attrFloat("a", 1.0f)));
        }
    }

    /// Applies whatever this node inherits onto `var`. Templates apply before
    /// the frame's own settings, so anything stated on the frame overrides what
    /// it inherited — the order FrameXML relies on.
    void emitInherits(const XmlNode& node, const std::string& var) {
        const std::string* inherits = node.attr("inherits");
        if (!inherits) return;
        std::stringstream ss(*inherits);
        std::string one;
        while (std::getline(ss, one, ',')) {
            one.erase(0, one.find_first_not_of(" \t"));
            one.erase(one.find_last_not_of(" \t") + 1);
            if (one.empty()) continue;
            line("if __WoweeTemplates[" + quote(one) + "] then __WoweeTemplates[" +
                 quote(one) + "](" + var + ") else __WoweeMissingTemplate(" +
                 quote(one) + ") end");
        }
    }

    /// Returns the variable holding the new frame, or empty for a virtual
    /// one, so a caller that must hand it on — a scroll frame to its child —
    /// can name it.
    std::string emitFrame(const XmlNode& node, const std::string& parentVar,
                          const std::string& parentName,
                          const std::string& nameVar = std::string()) {
        const std::string rawName = node.attrOr("name", "");
        const std::string name = substituteParent(rawName, parentName);
        const bool isVirtual = node.attrBool("virtual");

        if (isVirtual) {
            // A template is not built now. It is recorded so a later inherits=
            // can replay it onto a real frame, which is the only thing "virtual"
            // means in FrameXML.
            if (name.empty()) {
                result.warnings.push_back("virtual frame with no name was skipped");
                return {};
            }
            Emitter inner;
            inner.temp = 0;
            inner.runtimeParentName = true;
            // Inside a template the containing frame is whatever inherits it,
            // so an unqualified anchor means "my parent" and has to be asked
            // for at replay time.
            inner.emitFrameBody(node, "self", name, "self:GetParent()",
                                std::string(), /*fireOnLoad=*/false);
            line("__WoweeTemplates[" + quote(name) + "] = function(self)");
            line("this = self");
            line("local __w = {}");
            // A template can itself inherit one, and this branch used to return
            // before that was ever emitted — so InterfaceOptionsListButtonTemplate
            // silently dropped the OptionsListButtonTemplate it is built on,
            // arriving with no highlight texture and no size. First, so the
            // template's own body overrides what it inherited.
            emitInherits(node, "self");
            result.lua += inner.result.lua;
            for (auto& w : inner.result.warnings) result.warnings.push_back(w);
            line("end");
            return {};
        }

        const std::string var = nextVar();
        const std::string parentArg = node.attr("parent")
            ? *node.attr("parent")
            : (parentVar.empty() ? "UIParent" : parentVar);
        // Through nameArg, the same as regions: inside a template a child named
        // $parentScrollBar has to work out its name when the template is
        // replayed, because the frame it belongs to is not known until then.
        // Baking the literal instead named every scroll bar after the template,
        // so the _G[self:GetName().."ScrollBar"] its own handlers look up never
        // existed — which is what took down most of FrameXML.
        line(var + " = CreateFrame(" + quote(node.name) + ", " +
             nameArg(rawName, parentName, parentArg) + ", " + parentArg + ")");

        // Identity before anything is built on top of it. FrameXML makes names
        // out of the id — a party member's pet frame opens its OnLoad with
        // self:GetParent():GetID() — and a template's children load while the
        // template is being applied, which is before the frame's own body runs.
        // Set there, the parent was still answering zero.
        if (const std::string* id = node.attr("id"); id && !id->empty()) {
            line(var + ":SetID(" + *id + ")");
        }
        // Before the template applies, so a template body that reaches back
        // through its parent for a sibling finds it already bound.
        emitParentKey(node, var, parentArg);
        emitInherits(node, var);
        emitFrameBody(node, var, name.empty() ? parentName : name, parentArg,
                      parentName, /*fireOnLoad=*/true,
                      rawName.empty() ? nameVar : std::string());
        return var;
    }

    /// ownerName is the name of the frame containing this one. A frame's own
    /// anchors say $parent meaning the frame they hang off, not themselves, so
    /// they need a different name from the one its regions use.
    void emitFrameBody(const XmlNode& node, const std::string& var,
                       const std::string& name, const std::string& parentVar,
                       const std::string& ownerName = std::string(),
                       bool fireOnLoad = true,
                       const std::string& nameVar = std::string()) {
        // What $parent resolves to for anything declared inside this frame. An
        // unnamed frame is not the answer — WoW walks up to the nearest named
        // ancestor, and PartyMemberPetFrameTemplate buries its $parentName two
        // unnamed frames deep, expecting PartyMemberFrame1PetFrameName. Asking
        // the unnamed frame for its name gave nil, so the region was called
        // "Name" and the lookup that wanted it found nothing.
        const std::string anchor = nameVar.empty() ? var : nameVar;
        if (const XmlNode* size = node.child("Size")) {
            float w = 0, h = 0;
            if (readDimension(*size, w, h))
                line(var + ":SetSize(" + std::to_string(w) + ", " + std::to_string(h) + ")");
        }
        if (const std::string* strata = node.attr("frameStrata")) {
            line(var + ":SetFrameStrata(" + quote(*strata) + ")");
        }
        // A slider's range, step and orientation, declared as attributes. The
        // range has to be set before the value, or the value is clamped to a
        // default range it was never meant to sit in.
        if (node.attr("minValue") || node.attr("maxValue")) {
            line(var + ":SetMinMaxValues(" +
                 std::to_string(node.attrFloat("minValue", 0.0f)) + ", " +
                 std::to_string(node.attrFloat("maxValue", 1.0f)) + ")");
        }
        if (node.attr("valueStep")) {
            line(var + ":SetValueStep(" +
                 std::to_string(node.attrFloat("valueStep", 0.0f)) + ")");
        }
        if (const std::string* o = node.attr("orientation")) {
            line(var + ":SetOrientation(" + quote(*o) + ")");
        }
        if (node.attr("defaultValue")) {
            line(var + ":SetValue(" +
                 std::to_string(node.attrFloat("defaultValue", 0.0f)) + ")");
        }

        // A button only receives the clicks it asks for, and one file asks in
        // the XML rather than from a script.
        if (const std::string* clicks = node.attr("registerForClicks");
            clicks && !clicks->empty()) {
            std::stringstream ss(*clicks);
            std::string one, args;
            while (std::getline(ss, one, ',')) {
                one.erase(0, one.find_first_not_of(" \t"));
                one.erase(one.find_last_not_of(" \t") + 1);
                if (one.empty()) continue;
                if (!args.empty()) args += ", ";
                args += quote(one);
            }
            if (!args.empty()) line(var + ":RegisterForClicks(" + args + ")");
        }
        if (node.attr("enableMouse")) {
            line(var + ":EnableMouse(" + (node.attrBool("enableMouse") ? "true" : "false") + ")");
        }
        // Attributes declared in the XML, which is where FrameXML puts a
        // frame's initial state — UIParent's panel offsets among them. Set
        // before anything else runs, because SetAttribute fires
        // OnAttributeChanged and a handler reading a sibling attribute must
        // find it already there.
        if (const XmlNode* attrs = node.child("Attributes")) {
            for (const XmlNode& a : attrs->children) {
                if (a.name != "Attribute") continue;
                const std::string* an = a.attr("name");
                if (!an || an->empty()) continue;
                const std::string type = a.attrOr("type", "string");
                const std::string val = a.attrOr("value", "");
                std::string literal;
                if (type == "number")       literal = std::to_string(a.attrFloat("value", 0.0f));
                else if (type == "boolean") literal = a.attrBool("value") ? "true" : "false";
                else                        literal = quote(val);
                line(var + ":SetAttribute(" + quote(*an) + ", " + literal + ")");
            }
        }

        // A frame can fill its parent instead of stating anchors, and this was
        // honoured for regions and ignored for frames — all 139 of them across
        // 53 files. An unanchored frame falls to the centre-on-parent default
        // with no size, so its centre is the screen's: PlayerFrame's name sat
        // in the middle of the world because two frames above it said
        // setAllPoints and were heard by nobody.
        if (node.attrBool("setAllPoints")) {
            line(var + ":SetAllPoints(" + parentVar + ")");
        }
        if (const XmlNode* anchors = node.child("Anchors")) {
            // Anchored to the frame that contains it when no relativeTo is
            // given. This used to say UIParent for everything, so a nested
            // frame was positioned against the screen rather than its parent —
            // which for anything inside a panel puts it somewhere else
            // entirely, and FrameXML nests constantly.
            // ownerName, not name: a $parentApply here means the Apply button
            // beside this one on the frame that holds both, not a child of this
            // frame. Using its own name built VideoOptionsFrameCancelApply for
            // what should have been VideoOptionsFrameApply — a name nothing has,
            // so the anchor silently fell back to the parent.
            emitAnchors(*anchors, var, parentVar, ownerName);
        }
        if (const XmlNode* layers = node.child("Layers")) {
            for (const XmlNode& layer : layers->children) {
                if (layer.name != "Layer") continue;
                const std::string level = layer.attrOr("level", "ARTWORK");
                for (const XmlNode& region : layer.children) {
                    if (region.name == "Texture" || region.name == "FontString")
                        emitRegion(region, var, name, level,
                                   region.name == "Texture", anchor);
                }
            }
        }
        // Before Frames and Scripts, so a child anchoring to $parentNormalTexture
        // and an OnLoad reading its own label both find something there.
        emitButtonRegions(node, var, name);
        // A StatusBar's fill. Without it every health, mana and XP bar in
        // FrameXML has a value and nothing to draw it with.
        if (const XmlNode* bar = node.child("BarTexture")) {
            if (const std::string* file = bar->attr("file"))
                line(var + ":SetStatusBarTexture(" + quote(*file) + ")");
        }
        if (const XmlNode* col = node.child("BarColor")) {
            line(var + ":SetStatusBarColor(" +
                 std::to_string(col->attrFloat("r", 1.0f)) + ", " +
                 std::to_string(col->attrFloat("g", 1.0f)) + ", " +
                 std::to_string(col->attrFloat("b", 1.0f)) + ", " +
                 std::to_string(col->attrFloat("a", 1.0f)) + ")");
        }
        // A scroll frame's content, which is a frame like any other but reached
        // through SetScrollChild rather than sitting in Frames.
        // HybridScrollFrameScrollChild_OnLoad does self:GetParent().scrollChild
        // = self, so it has to be built and its OnLoad run — ignoring the
        // element left self.scrollChild nil on every one of the 18 files that
        // declare one.
        if (const XmlNode* scrollChild = node.child("ScrollChild")) {
            for (const XmlNode& child : scrollChild->children) {
                if (!isFrameElement(child.name)) continue;
                const std::string childVar = emitFrame(child, var, name, anchor);
                if (!childVar.empty())
                    line(var + ":SetScrollChild(" + childVar + ")");
            }
        }
        if (const XmlNode* frames = node.child("Frames")) {
            for (const XmlNode& child : frames->children) {
                if (isFrameElement(child.name)) emitFrame(child, var, name, anchor);
            }
        }
        // Scripts last: OnLoad runs against a frame that is already built, which
        // is what every handler in FrameXML assumes.
        //
        // Not from inside a template body, though. A frame is loaded once, when
        // it is finished — not once per template it is built from. Firing at
        // each template ran ChatFrameEditBoxTemplate's OnLoad before the edit
        // box's own OnLoad had set self.chatFrame, which is the very thing that
        // handler opens by indexing. The template only installs the script; the
        // frame it is applied to runs it, after its own body has had its say.
        if (const XmlNode* scripts = node.child("Scripts")) {
            emitScripts(*scripts, var);
        }
        // Whether or not this frame declared one: a template it inherits may
        // have installed the handler, and that frame still loads. The runtime
        // check costs nothing when there is none.
        // hidden="true" is part of the frame's initial state, so OnLoad already
        // sees it hidden. OnLoad runs under pcall because every frame in a file
        // is built in one chunk: one handler throwing used to abandon the rest
        // of the file, skipping later frames and their Hide(), which left the
        // world map and character panels open over the game.
        if (node.attrBool("hidden")) line(var + ":Hide()");
        if (fireOnLoad) {
            line("do local __f = " + var + ":GetScript(\"OnLoad\") if __f then "
                 "local __ok, __e = pcall(__f, " + var + ") if not __ok then "
                 "__WoweeLogWarning(\"OnLoad \" .. tostring(" + var +
                 ":GetName()) .. \": \" .. tostring(__e)) end end end");
        }
    }
};

} // namespace

std::string substituteParent(const std::string& name, const std::string& parentName) {
    const std::string token = "$parent";
    if (name.compare(0, token.size(), token) != 0) return name;
    return parentName + name.substr(token.size());
}

EmitResult emitFrameXml(const XmlNode& root) {
    Emitter e;
    e.line("local __w = {}");
    if (root.name != "Ui") {
        e.result.warnings.push_back("root element is <" + root.name + ">, expected <Ui>");
    }
    for (const XmlNode& node : root.children) {
        if (node.name == "Script") {
            if (const std::string* file = node.attr("file")) e.result.scriptFiles.push_back(*file);
            else if (!node.text.empty()) e.result.lua += node.text + "\n";
        } else if (node.name == "Include") {
            if (const std::string* file = node.attr("file")) e.result.includeFiles.push_back(*file);
        } else if (node.name == "Font") {
            e.emitFont(node);
        } else if (isFrameElement(node.name)) {
            e.emitFrame(node, "", "");
        }
    }
    return std::move(e.result);
}

} // namespace ui
} // namespace wowee
