#ifndef CED_EXTENSION_GRAPH_UI_H
#define CED_EXTENSION_GRAPH_UI_H

#include "manager.h"
#include <cui/cui.h>

typedef struct {
    uint64_t key, serial;
    float zoom;
    int style, direction;
    bool linking, pressed, moved, geometry_pending;
    bool selected_nodes[CED_GRAPH_MAX_NODES], selected_links[CED_GRAPH_MAX_LINKS];
    bool panning, pan_with_space;
    cui_vec2 pan, pan_pointer;
    uint64_t menu_key;
    int menu_node, menu_link;
    cui_vec2 menu_point;
    bool editing;
    char label[sizeof(((CedGraphNode *)0)->label)];
    double node_width, node_height;
    int drag_node, drag_x, drag_y, drag_w, drag_h, resize_corner;
    cui_vec2 press;
    CedGraphAnchor from, press_anchor;
    const char *status;
} CedExtensionGraphView;

/* Returns an owned graphEdit object after a completed gesture, or NULL. */
cJSON *ced_extension_graph_body(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview, bool editable, bool busy);

#endif
