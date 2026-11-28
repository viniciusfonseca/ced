/* The canvas, dotted grid, node cards and sampled Bezier wires are adapted
 * from CUI's src/cui_nodes.c (6e6b353) and the Agentic Solutions preview fork.
 * Copyright (c) 2026 The CUI authors. Licensed under CUI's MIT license. */
#include "graph_ui.h"
#include "cui_internal.h"

typedef struct { float x, y, scale; } GraphTransform;
typedef struct {
    const CedExtensionGraph *graph;
    const CedExtensionGraphView *view;
    cui_id wheel_id;
    int hover_node, hover_link, corner_node, hover_corner;
    CedGraphAnchor hover_anchor;
    cui_vec2 mouse;
    bool hover, focus_visible;
} GraphPaint;

static const float min_zoom = .25f, max_zoom = 4;
enum { CORNER_EAST = 1, CORNER_SOUTH = 2 };

static GraphTransform graph_transform(cui_rect bounds, const CedExtensionGraphView *v) {
    float z = v->zoom * fminf(bounds.w / CED_GRAPH_WIDTH, bounds.h / (CED_GRAPH_HEIGHT - CED_GRAPH_TOP));
    return (GraphTransform){
        bounds.x + (bounds.w - CED_GRAPH_WIDTH * z) * .5f - v->pan.x * z,
        bounds.y + (bounds.h - (CED_GRAPH_HEIGHT - CED_GRAPH_TOP) * z) * .5f - v->pan.y * z, z
    };
}

static cui_vec2 screen(GraphTransform t, cui_vec2 p) {
    return (cui_vec2){t.x + p.x * t.scale, t.y + (p.y - CED_GRAPH_TOP) * t.scale};
}

static cui_vec2 world(GraphTransform t, cui_vec2 p) {
    return (cui_vec2){(p.x - t.x) / t.scale, CED_GRAPH_TOP + (p.y - t.y) / t.scale};
}

static cui_vec2 corner_point(const CedGraphNode *n, int corner) {
    return (cui_vec2){n->x + ((corner & CORNER_EAST) ? n->w : 0),
                      n->y + ((corner & CORNER_SOUTH) ? n->h : 0)};
}

static int corner_at(const CedGraphNode *n, cui_vec2 p, float scale) {
    float radius = fminf(8 / scale, fminf(n->w, n->h) * .3f);
    for (int i = 0; i < 4; i++) {
        cui_vec2 c = corner_point(n, i);
        if (fabsf(c.x - p.x) <= radius && fabsf(c.y - p.y) <= radius) return i;
    }
    return -1;
}

static cui_vec2 node_anchor(CedGraphNode n, CedGraphAnchor a) {
    float x = n.x, y = n.y;
    switch (a.side) {
    case GRAPH_TOP:    return (cui_vec2){x + n.w * a.offset, y};
    case GRAPH_RIGHT:  return (cui_vec2){x + n.w, y + n.h * a.offset};
    case GRAPH_BOTTOM: return (cui_vec2){x + n.w * a.offset, y + n.h};
    default:           return (cui_vec2){x, y + n.h * a.offset};
    }
}

static CedGraphAnchor nearest_anchor(const CedExtensionGraph *g, cui_vec2 p) {
    CedGraphAnchor best = {.node = -1};
    float distance = 12;
    for (int i = g->node_count - 1; i >= 0; i--) {
        CedGraphNode n = g->nodes[i];
        double horizontal = cui__clampf((p.x - n.x) / n.w, 0, 1);
        double vertical = cui__clampf((p.y - n.y) / n.h, 0, 1);
        for (int side = 0; side < GRAPH_SIDE_COUNT; side++) {
            CedGraphAnchor a = {i, side, side % 2 ? vertical : horizontal};
            cui_vec2 end = node_anchor(n, a);
            float d = hypotf(end.x - p.x, end.y - p.y);
            if (d < distance) { best = a; distance = d; }
        }
    }
    return best;
}

static int node_at(const CedExtensionGraph *g, cui_vec2 p) {
    for (int i = g->node_count - 1; i >= 0; i--) {
        const CedGraphNode *n = &g->nodes[i];
        if (cui__rect_contains((cui_rect){n->x,n->y,n->w,n->h}, p.x, p.y)) return i;
    }
    return -1;
}

static CedGraphNode displayed_node(const GraphPaint *paint, int i) {
    CedGraphNode n = paint->graph->nodes[i];
    const CedExtensionGraphView *view = paint->view;
    if (((view->pressed && view->moved) || view->geometry_pending) && view->drag_node == i) {
        n.x = view->drag_x;
        n.y = view->drag_y;
        n.w = view->drag_w;
        n.h = view->drag_h;
    }
    return n;
}

static cui_vec2 graph_endpoint(const GraphPaint *paint, GraphTransform t, CedGraphAnchor a) {
    return screen(t, node_anchor(displayed_node(paint, a.node), a));
}

static cui_vec2 along(cui_vec2 a, int side, float distance, float perpendicular) {
    static const cui_vec2 normals[] = {{0,-1}, {1,0}, {0,1}, {-1,0}};
    cui_vec2 n = normals[side];
    return (cui_vec2){a.x + n.x * distance - n.y * perpendicular, a.y + n.y * distance + n.x * perpendicular};
}

enum { CURVE_POINTS = 96 };
static int sample_curve(cui_vec2 a, cui_vec2 b, cui_vec2 c, cui_vec2 d, float length, cui_vec2 *out) {
    int n = (int)CUI_CLAMP(length / 5, 12, CURVE_POINTS - 1);
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        out[i] = (cui_vec2){w0*a.x + w1*b.x + w2*c.x + w3*d.x,
                            w0*a.y + w1*b.y + w2*c.y + w3*d.y};
    }
    return n + 1;
}

static int wire_points(cui_vec2 a, int from, cui_vec2 d, int to, float z, cui_vec2 *pts) {
    float dist = hypotf(d.x - a.x, d.y - a.y);
    float k = CUI_CLAMP(dist * .46f, 20 * z, 160 * z);
    return sample_curve(a, along(a, from, k, 0), along(d, to, k, 0), d, dist + 2*k, pts);
}

/* Clip each segment before expanding its pattern. Very distant nodes can
 * otherwise produce millions of offscreen dashes. */
static bool clip_axis(float a, float b, float lo, float hi, double *start, double *end) {
    if (a == b) return a >= lo && a <= hi;
    double low = ((double)lo - a) / ((double)b - a);
    double high = ((double)hi - a) / ((double)b - a);
    *start = fmax(*start, fmin(low, high));
    *end = fmin(*end, fmax(low, high));
    return *start <= *end;
}

static cui_vec2 along_segment(cui_vec2 a, cui_vec2 b, double t) {
    return (cui_vec2){a.x + ((double)b.x - a.x) * t, a.y + ((double)b.y - a.y) * t};
}

static void draw_pattern(cui_ctx *ui, const cui_vec2 *pts, int count, int style, float th, cui_color color, cui_rect clip) {
    double traveled = 0;
    for (int i = 1; i < count; i++) {
        cui_vec2 a = pts[i - 1], b = pts[i];
        double len = hypot((double)b.x - a.x, (double)b.y - a.y);
        double start = 0, end = 1;
        if (len > 0 && clip_axis(a.x, b.x, clip.x - th, clip.x + clip.w + th, &start, &end) &&
            clip_axis(a.y, b.y, clip.y - th, clip.y + clip.h + th, &start, &end)) {
            double lo = traveled + start * len, hi = traveled + end * len;
            if (style == GRAPH_DOTTED) {
                for (double dot = ceil(lo / 7) * 7; dot < hi; dot += 7)
                    cui_draw_circle(ui, along_segment(a, b, (dot - traveled) / len), th * .72f, color);
            } else {
                for (double dash = floor(lo / 16) * 16; dash < hi; dash += 16) {
                    double x = fmax(lo, dash), y = fmin(hi, dash + 10);
                    if (x < y) cui_draw_line(ui, along_segment(a, b, (x - traveled) / len),
                                             along_segment(a, b, (y - traveled) / len), th, color);
                }
            }
        }
        traveled += len;
    }
}

static void draw_wire(cui_ctx *ui, cui_vec2 a, int from, cui_vec2 d, int to, int style, float z,
                      cui_color color, bool selected, cui_rect clip) {
    cui_vec2 pts[CURVE_POINTS];
    int count = wire_points(a, from, d, to, z, pts);
    float th = cui__clampf(1.75f * z, 1.1f, 3) + (selected ? 2 : 0);
    if (style == GRAPH_SOLID) cui_draw_polyline(ui, pts, count, th, color, false);
    else draw_pattern(ui, pts, count, style, th, color, clip);
}

static int link_at(const GraphPaint *paint, GraphTransform t) {
    int hit = -1;
    float best = 7 * 7;
    cui_vec2 mouse = screen(t, paint->mouse);
    for (int i = paint->graph->link_count - 1; i >= 0; i--) {
        const CedGraphLink *link = &paint->graph->links[i];
        cui_vec2 pts[CURVE_POINTS];
        int count = wire_points(graph_endpoint(paint, t, link->from), link->from.side,
                                graph_endpoint(paint, t, link->to), link->to.side, t.scale, pts);
        for (int j = 1; j < count; j++) {
            cui_vec2 a = pts[j - 1], b = pts[j];
            float dx = b.x - a.x, dy = b.y - a.y, length = dx*dx + dy*dy;
            if (!length) continue;
            float at = cui__clampf(((mouse.x - a.x)*dx + (mouse.y - a.y)*dy) / length, 0, 1);
            float x = mouse.x - a.x - at*dx, y = mouse.y - a.y - at*dy;
            if (x*x + y*y < best) { best = x*x + y*y; hit = i; }
        }
    }
    return hit;
}

static void draw_endpoint(cui_ctx *ui, cui_vec2 tip, int side, bool arrow, float z, cui_color color) {
    if (arrow) {
        cui_vec2 head[] = {along(tip, side, 11*z, -4.5f*z), tip, along(tip, side, 11*z, 4.5f*z)};
        cui_draw_polyline(ui, head, CUI_COUNTOF(head), cui__clampf(1.8f*z, 1.1f, 3), color, false);
    } else {
        cui_draw_circle(ui, tip, fmaxf(2, 2.7f*z), color);
    }
}

static void draw_node_contents(cui_ctx *ui, cui_rect card, float zoom, const char *label, int len) {
    const cui_font font = CUI_FONT_MEDIUM;
    float footer = fminf(24 * zoom, card.h * .4f);
    cui_rect body = {card.x, card.y, card.w, card.h - footer};
    float icon = fminf(28 * zoom, fminf(body.w, body.h) * .72f);
    if (icon >= 3) cui_draw_icon(ui, CUI_ICON_BOX, body, icon, CUI_C(ui, TEXT_SECONDARY));

    float hair = fminf(1, zoom);
    cui_draw_rect(ui, (cui_rect){card.x + hair, card.y + body.h, card.w - 2 * hair, hair},
                  CUI_CORNERS(0), CUI_C(ui, BORDER));

    float size = fminf(cui__fs(ui, ui->theme.text_body) * zoom, footer * .7f);
    if (!len || size < 3) return;
    float available = card.w - 16 * zoom;
    float width = cui__text_width(ui, font, size, label, len, false);
    char shortened[128 * 4 + 3];
    if (width > available) {
        static const char ellipsis[] = "\xe2\x80\xa6";
        float ellipsis_width = cui__text_width(ui, font, size, ellipsis, 3, false);
        if (ellipsis_width > available) return;
        int cut = cui__text_hit(ui, font, size, label, len, available - ellipsis_width);
        memcpy(shortened, label, (size_t)cut);
        for (;;) {
            memcpy(shortened + cut, ellipsis, 3);
            width = cui__text_width(ui, font, size, shortened, cut + 3, false);
            if (width <= available || !cut) break;
            cut = cui__utf8_prev(label, cut);
        }
        label = shortened;
        len = cut + 3;
    }
    float line = cui__font_line_height(ui, font, size);
    cui__draw_text_line(ui, font, size, card.x + (card.w - width) * .5f,
                        card.y + body.h + (footer - line) * .5f, CUI_C(ui, FOREGROUND), label, len, false);
}

static void draw_card(cui_ctx *ui, GraphTransform t, CedGraphNode node, bool hovered, bool raised, bool selected) {
    float z = t.scale;
    cui_vec2 pos = screen(t, (cui_vec2){node.x, node.y});
    cui_rect r = {pos.x, pos.y, node.w*z, node.h*z};
    cui_corners radius = CUI_CORNERS(ui->theme.radius_control * z);
    bool dark = ui->mode == CUI_THEME_DARK;
    cui_color bg = dark ? CUI_C(ui, SURFACE_RAISED) : CUI_C(ui, SURFACE);
    cui_color border = raised || selected ? CUI_C(ui, ACCENT) : cui_mix(CUI_C(ui, BORDER), CUI_C(ui, BORDER_STRONG), hovered ? .7f : 0);
    if (raised) cui__draw_elevation(ui, r, radius, CUI_ELEVATION_FLOATING, 1);
    cui_draw_rect_border(ui, r, radius, bg, border, selected ? 2 : 1);
    if (dark) cui__draw_top_light(ui, r, radius, .045f);
    draw_node_contents(ui, r, z, node.label, (int)strlen(node.label));
}

static void graph_wheel_target(cui_ctx *ui, cui_id id, cui_rect bounds) {
    cui_rect clip = ui->dl.clip_stack[ui->dl.clip_depth - 1];
    float scale = ui->scale;
    cui_rect visible = cui__rect_intersect(bounds, (cui_rect){clip.x / scale, clip.y / scale, clip.w / scale, clip.h / scale});
    cui_scroll_state *state = cui_state(ui, id, sizeof *state);
    state->view_w = bounds.w;
    state->view_h = bounds.h;
    state->content_w = bounds.w + 1;
    state->content_h = bounds.h + 1;
    cui__paint_add_hit(ui, id, visible, CUI_NF_SCROLL_X | CUI_NF_SCROLL_Y);
}

static void paint_graph(cui_ctx *ui, cui_node *node, void *data) {
    cui_rect bounds = node->rect;
    const GraphPaint *paint = data;
    const CedExtensionGraph *g = paint->graph;
    const CedExtensionGraphView *v = paint->view;
    GraphTransform t = graph_transform(bounds, v);
    if (t.scale <= 0) return;
    cui_push_clip(ui, bounds);
    graph_wheel_target(ui, paint->wheel_id, bounds);
    bool dark = ui->mode == CUI_THEME_DARK;
    cui_color bg = dark ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), .45f) : CUI_C(ui, SURFACE_MUTED);
    cui_draw_rect(ui, bounds, CUI_CORNERS(0), bg);
    float step = 24*t.scale;
    while (step < 14) step *= 2;
    float x0 = t.x + ceilf((bounds.x - t.x) / step) * step;
    float y0 = t.y + ceilf((bounds.y - t.y) / step) * step;
    cui_color dot = cui_alpha(CUI_C(ui, BORDER_STRONG), dark ? .32f : .42f);
    for (float y = y0; y < bounds.y+bounds.h; y += step)
        for (float x = x0; x < bounds.x+bounds.w; x += step)
            cui_draw_circle(ui, (cui_vec2){x,y}, .55f, dot);

    cui_color acc = CUI_C(ui, ACCENT);
    for (int i = 0; i < g->link_count; i++) {
        const CedGraphLink *l = &g->links[i];
        draw_wire(ui, graph_endpoint(paint, t, l->from), l->from.side, graph_endpoint(paint, t, l->to), l->to.side,
                  l->style, t.scale, v->selected_links[i] || paint->hover_link == i ? acc : cui_alpha(acc, .78f),
                  v->selected_links[i], bounds);
    }
    int raised = v->pressed && v->moved ? v->drag_node : -1;
    for (int i = 0; i < g->node_count; i++)
        if (i != raised) draw_card(ui, t, displayed_node(paint, i), paint->hover_node == i, false, v->selected_nodes[i]);
    if (raised >= 0) draw_card(ui, t, displayed_node(paint, raised), false, true, v->selected_nodes[raised]);

    for (int i = 0; i < g->link_count; i++) {
        const CedGraphLink *l = &g->links[i];
        draw_endpoint(ui, graph_endpoint(paint, t, l->from), l->from.side,
                     l->direction == GRAPH_BACKWARD || l->direction == GRAPH_BOTH, t.scale, acc);
        draw_endpoint(ui, graph_endpoint(paint, t, l->to), l->to.side,
                     l->direction == GRAPH_FORWARD || l->direction == GRAPH_BOTH, t.scale, acc);
    }
    bool wiring = v->linking || (v->pressed && v->moved && v->press_anchor.node >= 0);
    if (wiring) {
        CedGraphAnchor from = v->linking ? v->from : v->press_anchor;
        cui_vec2 a = graph_endpoint(paint, t, from);
        if (paint->hover) {
            CedGraphAnchor target = paint->hover_anchor;
            cui_vec2 d = target.node >= 0 ? graph_endpoint(paint, t, target) : screen(t, paint->mouse);
            int side = target.node >= 0 ? target.side : (from.side + 2) % GRAPH_SIDE_COUNT;
            draw_wire(ui, a, from.side, d, side, v->style, t.scale, cui_alpha(acc, .78f), false, bounds);
        }
        cui_draw_circle(ui, a, 5, acc);
    }
    if (paint->hover_anchor.node >= 0 && raised < 0) {
        cui_vec2 tip = graph_endpoint(paint, t, paint->hover_anchor);
        cui_draw_circle(ui, tip, 7, cui_alpha(acc, .24f));
        cui_draw_circle(ui, tip, 3, acc);
    }
    int corner = raised >= 0 ? v->resize_corner : paint->hover_corner;
    int corner_node = raised >= 0 ? raised : paint->corner_node;
    if (corner >= 0) {
        CedGraphNode n = displayed_node(paint, corner_node);
        cui_vec2 grip = screen(t, corner_point(&n, corner));
        cui_draw_rect_border(ui, (cui_rect){grip.x - 4, grip.y - 4, 8, 8}, CUI_CORNERS(1),
                             CUI_C(ui, SURFACE), acc, 1.5f);
    }
    if (paint->focus_visible) cui__draw_focus_ring(ui, cui__rect_inset(bounds, 2), CUI_CORNERS(0), 1);
    cui_pop_clip(ui);
}

static cJSON *position_edit(const char *type, int node, int x, int y) {
    cJSON *edit = cJSON_CreateObject();
    if (edit && cJSON_AddStringToObject(edit, "type", type) &&
        (node < 0 || cJSON_AddNumberToObject(edit, "node", node)) &&
        cJSON_AddNumberToObject(edit, "x", x) && cJSON_AddNumberToObject(edit, "y", y)) return edit;
    cJSON_Delete(edit);
    return NULL;
}

static cJSON *resize_edit(const CedExtensionGraphView *v) {
    cJSON *edit = position_edit("resizeNode", v->drag_node, v->drag_x, v->drag_y);
    if (edit && cJSON_AddNumberToObject(edit, "width", v->drag_w) && cJSON_AddNumberToObject(edit, "height", v->drag_h))
        return edit;
    cJSON_Delete(edit);
    return NULL;
}

static bool add_anchor(cJSON *link, const char *name, CedGraphAnchor a) {
    cJSON *end = cJSON_AddObjectToObject(link, name);
    return end && cJSON_AddNumberToObject(end, "node", a.node) && cJSON_AddStringToObject(end, "side", ced_graph_sides[a.side]) &&
        cJSON_AddNumberToObject(end, "offset", a.offset);
}

static cJSON *link_edit(CedExtensionGraphView *v, const CedExtensionGraph *g, CedGraphAnchor from, CedGraphAnchor to) {
    if (from.node == to.node && from.side == to.side && from.offset == to.offset) {
        v->status = "Choose a different border point.";
        return NULL;
    }
    if (g->link_count == CED_GRAPH_MAX_LINKS) {
        v->status = "Link limit reached.";
        return NULL;
    }
    cJSON *edit = cJSON_CreateObject(), *link = edit ? cJSON_AddObjectToObject(edit, "link") : NULL;
    if (link && cJSON_AddStringToObject(edit, "type", "addLink") &&
        add_anchor(link, "from", from) && add_anchor(link, "to", to) &&
        cJSON_AddStringToObject(link, "style", ced_graph_styles[v->style]) &&
        cJSON_AddStringToObject(link, "direction", ced_graph_directions[v->direction])) return edit;
    cJSON_Delete(edit);
    return NULL;
}

static void clear_selection(CedExtensionGraphView *v) {
    memset(v->selected_nodes, 0, sizeof v->selected_nodes);
    memset(v->selected_links, 0, sizeof v->selected_links);
}

static void select_at(CedExtensionGraphView *v, int node, int link, bool extend) {
    if (!extend) clear_selection(v);
    if (node >= 0) v->selected_nodes[node] = !extend || !v->selected_nodes[node];
    else if (link >= 0) v->selected_links[link] = !extend || !v->selected_links[link];
}

static cJSON *finish_gesture(CedExtensionGraphView *v, const GraphPaint *paint, bool editable, bool extend) {
    const CedExtensionGraph *g = paint->graph;
    CedGraphAnchor to = paint->hover_anchor;
    v->status = NULL;
    if (editable && v->moved && v->drag_node >= 0) {
        const CedGraphNode *node = &g->nodes[v->drag_node];
        if (v->resize_corner >= 0) {
            if (v->drag_x != node->x || v->drag_y != node->y || v->drag_w != node->w || v->drag_h != node->h)
                return resize_edit(v);
        } else if (v->drag_x != node->x || v->drag_y != node->y)
            return position_edit("moveNode", v->drag_node, v->drag_x, v->drag_y);
        return NULL;
    }
    if (editable && to.node >= 0 && (v->linking || v->press_anchor.node >= 0)) {
        if (v->linking || v->moved) {
            cJSON *edit = link_edit(v, g, v->linking ? v->from : v->press_anchor, to);
            if (edit) v->linking = false;
            return edit;
        }
        v->from = to;
        v->linking = true;
    } else if (paint->hover && !v->moved) {
        v->linking = false;
        select_at(v, paint->hover_corner >= 0 ? paint->corner_node : paint->hover_node, paint->hover_link, extend);
    }
    return NULL;
}

static int graph_position(float position) {
    return (int)roundf(cui__clampf(position, -CED_GRAPH_MAX_COORDINATE, CED_GRAPH_MAX_COORDINATE));
}

static void drag_geometry(CedExtensionGraphView *v, const CedGraphNode *n, float dx, float dy) {
    v->drag_w = n->w;
    v->drag_h = n->h;
    if (v->resize_corner < 0) {
        v->drag_x = graph_position(n->x + dx);
        v->drag_y = graph_position(n->y + dy);
        return;
    }
    bool east = (v->resize_corner & CORNER_EAST) != 0, south = (v->resize_corner & CORNER_SOUTH) != 0;
    int right = n->x + n->w, bottom = n->y + n->h;
    v->drag_w = (int)roundf(CUI_CLAMP(east ? n->w + dx : right - graph_position(n->x + dx), 40, CED_GRAPH_WIDTH));
    v->drag_h = (int)roundf(CUI_CLAMP(south ? n->h + dy : bottom - graph_position(n->y + dy), 28, CED_GRAPH_HEIGHT));
    v->drag_x = east ? n->x : right - v->drag_w;
    v->drag_y = south ? n->y : bottom - v->drag_h;
}

static void clamp_pan(CedExtensionGraphView *v) {
    v->pan.x = cui__clampf(v->pan.x, -CED_GRAPH_MAX_COORDINATE, CED_GRAPH_MAX_COORDINATE);
    v->pan.y = cui__clampf(v->pan.y, -CED_GRAPH_MAX_COORDINATE, CED_GRAPH_MAX_COORDINATE);
}

static GraphTransform graph_scroll(cui_ctx *ui, CedExtensionGraphView *v, cui_canvas_result hit, cui_id wheel_id,
                                  cui_rect bounds, GraphTransform t) {
    if (!hit.hovered || ui->scroll_target != wheel_id || v->pressed || v->panning) return t;
    float dx = ui->scroll_dx, dy = ui->scroll_dy;
    if (!dx && !dy) return t;
    ui->scroll_dx = ui->scroll_dy = 0;
    if (ui->mods & CUI_MOD_PRIMARY) {
        cui_vec2 anchor = world(t, hit.mouse);
        v->zoom = cui__clampf(v->zoom * powf(1.12f, dy != 0 ? dy : dx), min_zoom, max_zoom);
        GraphTransform updated = graph_transform(bounds, v);
        cui_vec2 pos = screen(updated, anchor);
        v->pan.x += (pos.x - hit.mouse.x) / updated.scale;
        v->pan.y += (pos.y - hit.mouse.y) / updated.scale;
    } else if (ui->mods & CUI_MOD_SHIFT) {
        v->pan.x -= (dx != 0 ? dx : dy) * 40 / t.scale;
    } else {
        v->pan.x -= dx * 40 / t.scale;
        v->pan.y -= dy * 40 / t.scale;
    }
    clamp_pan(v);
    return graph_transform(bounds, v);
}

static bool graph_pan(cui_ctx *ui, CedExtensionGraphView *v, GraphTransform t, cui_canvas_result hit) {
    if (!v->panning && hit.hovered) {
        bool space = cui_key_down(ui, CUI_KEY_SPACE) && hit.pressed && cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
        if (space || cui_mouse_clicked(ui, CUI_MOUSE_MIDDLE)) {
            v->panning = true;
            v->pan_with_space = space;
            v->pan_pointer = hit.mouse;
            v->pressed = false;
        }
    }
    if (!v->panning) return false;
    cui_mouse_button button = v->pan_with_space ? CUI_MOUSE_LEFT : CUI_MOUSE_MIDDLE;
    if (!cui_mouse_down(ui, button) || !ui->mouse_inside) v->panning = false;
    else {
        v->pan.x += (v->pan_pointer.x - hit.mouse.x) / t.scale;
        v->pan.y += (v->pan_pointer.y - hit.mouse.y) / t.scale;
        clamp_pan(v);
        v->pan_pointer = hit.mouse;
        cui__set_cursor(ui, CUI_CURSOR_MOVE);
    }
    return true;
}

static cJSON *graph_input(cui_ctx *ui, CedExtensionGraphView *v, GraphPaint *p, cui_canvas_result hit,
                          bool editable, bool busy) {
    cui_rect bounds = {0, 0, hit.rect.w, hit.rect.h};
    GraphTransform t = graph_transform(bounds, v);
    if (t.scale <= 0 || !ui->window_focused || cui_is_disabled(ui) || v->editing) {
        v->pressed = v->panning = false;
        return NULL;
    }
    t = graph_scroll(ui, v, hit, p->wheel_id, bounds, t);
    if (graph_pan(ui, v, t, hit) || busy) return NULL;
    p->mouse = world(t, hit.mouse);
    p->hover = hit.hovered;
    if (p->hover) {
        p->hover_node = node_at(p->graph, p->mouse);
        if (editable) {
            p->hover_anchor = nearest_anchor(p->graph, p->mouse);
            int node = p->hover_node >= 0 ? p->hover_node : p->hover_anchor.node;
            if (!v->linking && node >= 0) {
                p->hover_corner = corner_at(&p->graph->nodes[node], p->mouse, t.scale);
                if (p->hover_corner >= 0) {
                    p->corner_node = node;
                    p->hover_anchor.node = -1;
                }
            }
        }
        if (p->corner_node < 0 && p->hover_node < 0 && p->hover_anchor.node < 0) p->hover_link = link_at(p, t);
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) v->linking = false;
    }
    if (p->hover && hit.pressed && cui_mouse_clicked(ui, CUI_MOUSE_LEFT)) {
        v->pressed = true;
        v->moved = false;
        v->press = p->mouse;
        v->press_anchor = p->hover_anchor;
        v->resize_corner = p->hover_corner;
        v->drag_node = editable && !v->linking && v->press_anchor.node < 0
            ? (p->hover_corner >= 0 ? p->corner_node : p->hover_node) : -1;
    }
    if (v->pressed ? v->resize_corner >= 0 : p->hover_corner >= 0) cui__set_cursor(ui, CUI_CURSOR_RESIZE_EW);
    else if (v->pressed && v->drag_node >= 0) cui__set_cursor(ui, CUI_CURSOR_MOVE);
    else if (p->hover_anchor.node >= 0 || p->hover_node >= 0 || p->hover_link >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    if (!v->pressed) return NULL;

    bool extend = (ui->mods & (CUI_MOD_SHIFT | CUI_MOD_CTRL | CUI_MOD_PRIMARY)) != 0;
    float dx = p->mouse.x - v->press.x, dy = p->mouse.y - v->press.y;
    if (ui->mouse_inside) {
        v->moved |= hypotf(dx, dy) * t.scale >= 3;
        if (v->drag_node >= 0) {
            const CedGraphNode *n = &p->graph->nodes[v->drag_node];
            if (v->moved && !v->selected_nodes[v->drag_node]) select_at(v, v->drag_node, -1, extend);
            drag_geometry(v, n, dx, dy);
        }
    }
    if (!hit.pressed) {
        cJSON *edit = ui->mreleased[CUI_MOUSE_LEFT] && ui->mouse_inside ? finish_gesture(v, p, editable, extend) : NULL;
        v->pressed = false;
        return edit;
    }
    return NULL;
}

enum { REMOVE_SELECTED_NODES = 1, REMOVE_SELECTED_LINKS = 2 };

static bool any_selected(const bool *selected, int count) {
    for (int i = 0; i < count; i++) if (selected[i]) return true;
    return false;
}

static bool append_removals(cJSON *edit, const char *name, int count, int single, const bool *selected) {
    cJSON *items = cJSON_AddArrayToObject(edit, name);
    if (!items) return false;
    for (int i = 0; i < count; i++) {
        if ((single == i || (selected && selected[i])) &&
            !ced_ext_json_append(items, cJSON_CreateNumber(i))) return false;
    }
    return true;
}

static cJSON *remove_elements(const CedExtensionGraph *g, const CedExtensionGraphView *v, int node, int link, int mask) {
    const bool *nodes = mask & REMOVE_SELECTED_NODES ? v->selected_nodes : NULL;
    const bool *links = mask & REMOVE_SELECTED_LINKS ? v->selected_links : NULL;
    if (node < 0 && link < 0 && !(nodes && any_selected(nodes, g->node_count)) &&
        !(links && any_selected(links, g->link_count))) return NULL;
    cJSON *edit = cJSON_CreateObject();
    if (edit && cJSON_AddStringToObject(edit, "type", "removeElements") &&
        append_removals(edit, "nodes", g->node_count, node, nodes) &&
        append_removals(edit, "links", g->link_count, link, links)) return edit;
    cJSON_Delete(edit);
    return NULL;
}

static cJSON *graph_menu(cui_ctx *ui, CedExtensionGraphView *v, const GraphPaint *paint) {
    const CedExtensionGraph *g = paint->graph;
    if (paint->hover && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) {
        v->menu_key = v->key;
        v->menu_node = paint->hover_node;
        v->menu_link = paint->hover_link;
        v->menu_point = paint->mouse;
        v->linking = v->pressed = false;
    }
    cJSON *edit = NULL;
    cui_context_menu(ui, "graph-context") {
        /* The canvas is the trigger, but the menu should fit its items. */
        ui->nodes[cui__current(ui)].min_w = 180;
        bool stale = v->menu_key != v->key;
        if (v->menu_node >= 0 && v->menu_node < g->node_count) {
            if (cui_menu_item(ui, "Edit Node…", .icon = CUI_ICON_SQUARE_PEN, .disabled = stale)) {
                const CedGraphNode *n = &g->nodes[v->menu_node];
                memcpy(v->label, n->label, sizeof v->label);
                v->node_width = n->w;
                v->node_height = n->h;
                v->editing = true;
            }
            if (cui_menu_item(ui, "Remove Node", .icon = CUI_ICON_TRASH_2, .danger = true, .disabled = stale))
                edit = remove_elements(g, v, v->menu_node, -1, 0);
        } else if (v->menu_link >= 0 && v->menu_link < g->link_count) {
            if (cui_menu_item(ui, "Remove Link", .icon = CUI_ICON_TRASH_2, .danger = true, .disabled = stale))
                edit = remove_elements(g, v, -1, v->menu_link, 0);
        } else if (cui_menu_item(ui, "Add Node", .icon = CUI_ICON_PLUS,
                                 .disabled = stale || g->node_count == CED_GRAPH_MAX_NODES)) {
            edit = position_edit("addNode", -1, graph_position(v->menu_point.x), graph_position(v->menu_point.y));
        }
        if (any_selected(v->selected_nodes, g->node_count) &&
            cui_menu_item(ui, "Remove Selected Nodes", .icon = CUI_ICON_TRASH_2, .danger = true, .disabled = stale))
            edit = remove_elements(g, v, -1, -1, REMOVE_SELECTED_NODES);
        if (any_selected(v->selected_links, g->link_count) &&
            cui_menu_item(ui, "Remove Selected Links", .icon = CUI_ICON_TRASH_2, .danger = true, .disabled = stale))
            edit = remove_elements(g, v, -1, -1, REMOVE_SELECTED_LINKS);
    }
    return edit;
}

static cJSON *node_dialog(cui_ctx *ui, CedExtensionGraphView *v) {
    cJSON *edit = NULL;
    cui_dialog(ui, "Edit Node", &v->editing, .width = 400) {
        bool submitted = (cui_input(ui, "node-label", v->label, sizeof v->label, .label = "Label", .autofocus = true)
                          & CUI_SUBMITTED) != 0;
        cui_number(ui, "node-width", &v->node_width, 40, CED_GRAPH_WIDTH, .label = "Width", .step = 1);
        cui_number(ui, "node-height", &v->node_height, 28, CED_GRAPH_HEIGHT, .label = "Height", .step = 1);
        int characters = 0;
        for (const unsigned char *p = (const unsigned char *)v->label; *p; p++)
            if ((*p & 0xc0) != 0x80) characters++;
        if (characters > 128) cui_secondary(ui, "Labels can contain up to 128 characters.");
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Close", .variant = CUI_BUTTON_DEFAULT)) v->editing = false;
            bool save = cui_button(ui, "Save", .variant = CUI_BUTTON_PRIMARY, .disabled = characters > 128);
            if (v->editing && characters <= 128 && (save || submitted)) {
                edit = cJSON_CreateObject();
                if (!edit || !cJSON_AddStringToObject(edit, "type", "editNode") ||
                    !cJSON_AddNumberToObject(edit, "node", v->menu_node) || !cJSON_AddStringToObject(edit, "label", v->label) ||
                    !cJSON_AddNumberToObject(edit, "width", round(v->node_width)) ||
                    !cJSON_AddNumberToObject(edit, "height", round(v->node_height))) {
                    cJSON_Delete(edit);
                    edit = NULL;
                } else v->editing = false;
            }
        }
    }
    return edit;
}

static cui_canvas_result graph_canvas(cui_ctx *ui, GraphPaint *paint, bool *keyboard) {
    cui_id id = cui_make_id(ui, "extension-graph");
    cui_interact input = cui__interact(ui, id, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    cui_rect rect = cui__prev_rect(ui, id);
    int index = cui__leaf(ui, id, CUI_FILL, CUI_FILL);
    cui__mark_interactive(ui, index, CUI_IF_FOCUSABLE);
    ui->nodes[index].paint = paint_graph;
    ui->nodes[index].paint_data = paint;
    ui->last_id = id;
    paint->focus_visible = input.focus_visible;
    *keyboard = input.keyboard_focused && ui->window_focused && !ui->modal_layer && !ui->prev_modal_layer;
    return (cui_canvas_result){.rect = rect, .hovered = input.hovered, .pressed = input.pressed,
        .mouse = {ui->mx - rect.x, ui->my - rect.y}};
}

cJSON *ced_extension_graph_body(cui_ctx *ui, CedExtensionGraphView *v, const CedExtensionPreview *preview, bool editable, bool busy) {
    if (!v->zoom) {
        v->zoom = 1;
        v->direction = GRAPH_FORWARD;
    }
    if (v->key != preview->key) {
        v->key = preview->key;
        v->editing = v->linking = v->pressed = false;
        v->status = NULL;
    }
    if (v->serial != preview->serial) {
        const CedExtensionGraph *g = preview->graph;
        bool moved = v->geometry_pending && v->drag_node >= 0 && v->drag_node < g->node_count &&
            g->nodes[v->drag_node].x == v->drag_x && g->nodes[v->drag_node].y == v->drag_y &&
            g->nodes[v->drag_node].w == v->drag_w && g->nodes[v->drag_node].h == v->drag_h;
        if (!moved) clear_selection(v);
    }
    if (!busy || v->serial != preview->serial) v->geometry_pending = false;
    v->serial = preview->serial;

    cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
        if (cui_icon_button(ui, CUI_ICON_ZOOM_OUT, .size = CUI_SIZE_SM, .tooltip = "Zoom out", .disabled = v->zoom <= min_zoom))
            v->zoom = fmaxf(min_zoom, v->zoom / 1.25f);
        if (cui_icon_button(ui, CUI_ICON_ZOOM_IN, .size = CUI_SIZE_SM, .tooltip = "Zoom in", .disabled = v->zoom >= max_zoom))
            v->zoom = fminf(max_zoom, v->zoom * 1.25f);

        if (editable) {
            cui_push_disabled(ui, busy || cui_is_disabled(ui));
            char label[40];
            snprintf(label, sizeof label, "Style: %s##style", ced_graph_styles[v->style]);
            if (cui_button(ui, label, .size = CUI_SIZE_SM)) v->style = (v->style + 1) % GRAPH_STYLE_COUNT;
            snprintf(label, sizeof label, "Arrow: %s##direction", ced_graph_directions[v->direction]);
            if (cui_button(ui, label, .size = CUI_SIZE_SM)) v->direction = (v->direction + 1) % GRAPH_DIRECTION_COUNT;
            cui_pop_disabled(ui);
        }
    }
    const char *status = "Read-only diagram.";
    if (editable) status = v->status ? v->status :
        v->linking ? "Choose a border point to finish the link. Esc cancels." : "Right-click the canvas or a node to open its menu.";
    cui_secondary(ui, status);
    GraphPaint *paint = cui__frame_alloc(ui, sizeof *paint);
    *paint = (GraphPaint){.graph = preview->graph, .view = v, .hover_node = -1, .hover_link = -1,
        .hover_corner = -1, .corner_node = -1, .hover_anchor = {.node = -1},
        .wheel_id = cui_make_id(ui, "extension-graph-wheel")};
    bool keyboard;
    cui_canvas_result hit = graph_canvas(ui, paint, &keyboard);
    cui_popup *menu = cui__popup_get(ui, cui_make_id(ui, "graph-context") ^ 0xC7Cu, false);
    bool menu_open = menu && menu->open;
    /* CUI keeps a context menu open on its trigger, which here fills the canvas. */
    if (menu_open && ui->hot_layer != menu->id &&
        (cui_mouse_clicked(ui, CUI_MOUSE_LEFT) || cui_mouse_clicked(ui, CUI_MOUSE_MIDDLE)))
        cui__popup_set(ui, menu->id, false);

    cJSON *edit = NULL;
    if (!menu_open || cui_mouse_clicked(ui, CUI_MOUSE_RIGHT))
        edit = graph_input(ui, v, paint, hit, editable, busy);
    if (!edit && editable && !busy && keyboard && !v->editing && !v->pressed && !v->panning &&
        (cui_key_pressed(ui, CUI_KEY_DELETE) || cui_key_pressed(ui, CUI_KEY_BACKSPACE))) {
        edit = remove_elements(preview->graph, v, -1, -1, REMOVE_SELECTED_NODES | REMOVE_SELECTED_LINKS);
        if (edit) {
            ui->kpressed[CUI_KEY_DELETE] = ui->kpressed[CUI_KEY_BACKSPACE] = 0;
            v->linking = false;
        }
    }
    if (!edit && editable && !busy && !v->editing && !v->panning) edit = graph_menu(ui, v, paint);
    if (!edit) edit = node_dialog(ui, v);
    return edit;
}
