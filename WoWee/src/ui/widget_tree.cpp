#include "ui/widget_tree.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wowee {
namespace ui {

namespace {

bool contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}

int strataRank(FrameStrata s) { return static_cast<int>(s); }
int layerRank(DrawLayer l) { return static_cast<int>(l); }

} // namespace

AnchorPoint resolveAnchorPoint(const std::string& rawName) {
    const std::string name = upper(rawName);
    AnchorPoint p;
    // Vertical: TOP is 1, BOTTOM is 0, neither is centred. Tested before the
    // horizontal half because the names combine (TOPLEFT is both).
    if (contains(name, "TOP"))         p.fy = 1.0f;
    else if (contains(name, "BOTTOM")) p.fy = 0.0f;
    else                               p.fy = 0.5f;

    if (contains(name, "LEFT"))        p.fx = 0.0f;
    else if (contains(name, "RIGHT"))  p.fx = 1.0f;
    else                               p.fx = 0.5f;
    return p;
}

DrawLayer parseDrawLayer(const std::string& rawName) {
    const std::string n = upper(rawName);
    if (n == "BACKGROUND") return DrawLayer::Background;
    if (n == "BORDER")     return DrawLayer::Border;
    if (n == "OVERLAY")    return DrawLayer::Overlay;
    if (n == "HIGHLIGHT")  return DrawLayer::Highlight;
    return DrawLayer::Artwork;   // Blizzard's default
}

FrameStrata parseStrata(const std::string& rawName) {
    const std::string n = upper(rawName);
    if (n == "WORLD")             return FrameStrata::World;
    if (n == "BACKGROUND")        return FrameStrata::Background;
    if (n == "LOW")               return FrameStrata::Low;
    if (n == "HIGH")              return FrameStrata::High;
    if (n == "DIALOG")            return FrameStrata::Dialog;
    if (n == "FULLSCREEN")        return FrameStrata::Fullscreen;
    if (n == "FULLSCREEN_DIALOG") return FrameStrata::FullscreenDialog;
    if (n == "TOOLTIP")           return FrameStrata::Tooltip;
    return FrameStrata::Medium;
}

WidgetTree::WidgetTree() {
    widgets_.emplace_back();          // id 0 is "none"
    rootId_ = create(WidgetKind::Frame, 0, "UIParent");
}

uint32_t WidgetTree::create(WidgetKind kind, uint32_t parent, const std::string& name) {
    const uint32_t id = static_cast<uint32_t>(widgets_.size());
    widgets_.emplace_back();
    Widget& w = widgets_.back();
    w.id = id;
    w.kind = kind;
    w.name = name;
    w.creationOrder = nextOrder_++;
    // Regions belong to the frame that made them; a frame with no parent hangs
    // off the root, which is what UIParent is for.
    if (parent == 0 && id != rootId_ && rootId_ != 0) parent = rootId_;
    w.parent = parent;
    if (parent != 0 && parent < widgets_.size()) {
        widgets_[parent].children.push_back(id);
    }
    return id;
}

Widget* WidgetTree::get(uint32_t id) {
    if (id == 0 || id >= widgets_.size()) return nullptr;
    return &widgets_[id];
}

const Widget* WidgetTree::get(uint32_t id) const {
    if (id == 0 || id >= widgets_.size()) return nullptr;
    return &widgets_[id];
}

void WidgetTree::clearPoints(uint32_t id) {
    if (Widget* w = get(id)) w->anchors.clear();
}

void WidgetTree::addPoint(uint32_t id, const Anchor& anchor) {
    Widget* w = get(id);
    if (!w) return;
    // Setting a point that is already set moves it, as in WoW; a frame that
    // slides (the bonus action bar) re-sets the same point every frame.
    auto samePoint = [&](const Anchor& a) {
        if (a.point.size() != anchor.point.size()) return false;
        for (size_t i = 0; i < a.point.size(); ++i)
            if (std::toupper(static_cast<unsigned char>(a.point[i])) !=
                std::toupper(static_cast<unsigned char>(anchor.point[i]))) return false;
        return true;
    };
    for (Anchor& a : w->anchors) {
        if (samePoint(a)) { a = anchor; return; }
    }
    w->anchors.push_back(anchor);
}

void WidgetTree::setAllPoints(uint32_t id, uint32_t relativeTo) {
    Widget* w = get(id);
    if (!w) return;
    w->anchors.clear();
    // Two opposing corners, which is exactly what makes the size fall out of
    // the solver below rather than needing an explicit one.
    Anchor tl; tl.point = "TOPLEFT";     tl.relativePoint = "TOPLEFT";     tl.relativeTo = relativeTo;
    Anchor br; br.point = "BOTTOMRIGHT"; br.relativePoint = "BOTTOMRIGHT"; br.relativeTo = relativeTo;
    w->anchors.push_back(tl);
    w->anchors.push_back(br);
}

void WidgetTree::layout(float pixelW, float pixelH) {
    // How many pixels one interface unit is worth. Everything below works in
    // units; only the renderer and hit testing convert.
    uiScale_ = (pixelH > 0.0f) ? (pixelH / kInterfaceHeight) : 1.0f;
    const float screenW = (uiScale_ > 0.0f) ? (pixelW / uiScale_) : pixelW;
    const float screenH = kInterfaceHeight;

    Widget& rootW = widgets_[rootId_];
    rootW.left = 0.0f;
    rootW.bottom = 0.0f;
    rootW.rectW = screenW;
    rootW.rectH = screenH;
    rootW.visible = rootW.shown;
    rootW.effStrata = rootW.strata;
    rootW.effLevel = 0;

    for (uint32_t child : rootW.children) layoutWidget(child, screenW, screenH);
    collectDrawOrder();
}

void WidgetTree::layoutWidget(uint32_t id, float screenW, float screenH) {
    Widget* w = get(id);
    if (!w) return;
    const Widget* parent = get(w->parent);

    w->visible = w->shown && (!parent || parent->visible);
    // Strata and level are inherited unless the widget set its own. A child
    // frame sits one level above its parent so it draws over it, which is what
    // makes a button's own regions land on top of the frame holding it.
    w->effStrata = w->strataExplicit ? w->strata : (parent ? parent->effStrata : FrameStrata::Medium);
    w->effLevel  = w->levelExplicit  ? w->level  : (parent ? parent->effLevel + 1 : 0);

    // Solve each axis from the anchors. An anchor says "this fraction of my rect
    // sits at that point", which is one linear constraint; two constraints with
    // different fractions give the size as well as the position, and that is how
    // a frame pinned at two opposing corners gets sized without anyone calling
    // SetSize.
    struct Constraint { float f; float target; };
    std::vector<Constraint> cx, cy;
    cx.reserve(w->anchors.size());
    cy.reserve(w->anchors.size());

    for (const Anchor& a : w->anchors) {
        const Widget* rel = (a.relativeTo != 0) ? get(a.relativeTo) : parent;
        float relLeft, relBottom, relW, relH;
        if (rel) {
            relLeft = rel->left; relBottom = rel->bottom; relW = rel->rectW; relH = rel->rectH;
        } else {
            relLeft = 0.0f; relBottom = 0.0f; relW = screenW; relH = screenH;
        }
        const AnchorPoint rp = resolveAnchorPoint(a.relativePoint);
        const AnchorPoint mp = resolveAnchorPoint(a.point);
        cx.push_back({mp.fx, relLeft   + rp.fx * relW + a.x});
        cy.push_back({mp.fy, relBottom + rp.fy * relH + a.y});
    }

    auto solveAxis = [](const std::vector<Constraint>& cs, float explicitSize,
                        float parentOrigin, float parentSize,
                        float& outOrigin, float& outSize) {
        if (cs.empty()) {
            // Unanchored: WoW leaves this undefined, and centring on the parent
            // is the least surprising thing to draw.
            outSize = explicitSize;
            outOrigin = parentOrigin + (parentSize - explicitSize) * 0.5f;
            return;
        }
        // Pick the two constraints furthest apart in fraction; anything closer
        // is either redundant or a near-degenerate pair that would divide by
        // almost zero and throw the rect across the screen.
        size_t lo = 0, hi = 0;
        for (size_t i = 1; i < cs.size(); ++i) {
            if (cs[i].f < cs[lo].f) lo = i;
            if (cs[i].f > cs[hi].f) hi = i;
        }
        const float spread = cs[hi].f - cs[lo].f;
        if (spread > 0.01f) {
            outSize = (cs[hi].target - cs[lo].target) / spread;
            outOrigin = cs[lo].target - cs[lo].f * outSize;
        } else {
            outSize = explicitSize;
            outOrigin = cs[0].target - cs[0].f * outSize;
        }
    };

    const float pLeft   = parent ? parent->left   : 0.0f;
    const float pBottom = parent ? parent->bottom : 0.0f;
    const float pW      = parent ? parent->rectW  : screenW;
    const float pH      = parent ? parent->rectH  : screenH;

    solveAxis(cx, w->width,  pLeft,   pW, w->left,   w->rectW);
    solveAxis(cy, w->height, pBottom, pH, w->bottom, w->rectH);

    for (uint32_t child : w->children) layoutWidget(child, screenW, screenH);
}

uint32_t WidgetTree::hitTest(float x, float y) const {
    const Widget* best = nullptr;
    for (const Widget& w : widgets_) {
        if (w.id == 0 || w.kind != WidgetKind::Frame) continue;
        if (!w.visible || !w.mouseEnabled) continue;
        if (w.rectW <= 0.0f || w.rectH <= 0.0f) continue;
        if (x < w.left || x > w.left + w.rectW) continue;
        if (y < w.bottom || y > w.bottom + w.rectH) continue;
        if (!best) { best = &w; continue; }
        // Same comparison the draw order uses, read the other way round: the
        // last thing painted is the first thing clicked.
        const int sa = strataRank(w.effStrata), sb = strataRank(best->effStrata);
        if (sa != sb) { if (sa > sb) best = &w; continue; }
        if (w.effLevel != best->effLevel) { if (w.effLevel > best->effLevel) best = &w; continue; }
        if (w.creationOrder > best->creationOrder) best = &w;
    }
    return best ? best->id : 0;
}

void WidgetTree::collectDrawOrder() {
    drawOrder_.clear();
    for (const Widget& w : widgets_) {
        if (w.id == 0) continue;
        if (!w.visible) continue;
        if (w.alpha <= 0.001f) continue;
        // Frames are containers, except when they carry a backdrop or are a
        // status bar — then the frame itself has something to paint, and it
        // paints underneath its own regions because they sit a level above it.
        if (w.kind == WidgetKind::Frame && !w.hasBackdrop && !w.isStatusBar) continue;
        if (w.rectW <= 0.0f || w.rectH <= 0.0f) continue;
        if (w.kind == WidgetKind::Texture && w.texturePath.empty() && !w.solidColor) continue;
        if (w.kind == WidgetKind::Frame && w.isStatusBar && w.barTexture.empty() &&
            !w.hasBackdrop) continue;
        if (w.kind == WidgetKind::FontString && w.text.empty()) continue;
        drawOrder_.push_back(&w);
    }

    std::sort(drawOrder_.begin(), drawOrder_.end(),
              [](const Widget* a, const Widget* b) {
                  const int sa = strataRank(a->effStrata), sb = strataRank(b->effStrata);
                  if (sa != sb) return sa < sb;
                  if (a->effLevel != b->effLevel) return a->effLevel < b->effLevel;
                  const int la = layerRank(a->layer), lb = layerRank(b->layer);
                  if (la != lb) return la < lb;
                  if (a->subLevel != b->subLevel) return a->subLevel < b->subLevel;
                  // Ties resolve by creation order, so a region added later sits
                  // on top of one added earlier — the same rule the real client
                  // uses within a layer.
                  return a->creationOrder < b->creationOrder;
              });
}

} // namespace ui
} // namespace wowee
