#ifndef CED_EXTENSION_GRAPH_H
#define CED_EXTENSION_GRAPH_H

#include <cJSON.h>
#include <stdbool.h>

enum {
    CED_GRAPH_WIDTH = 960, CED_GRAPH_HEIGHT = 620, CED_GRAPH_TOP = 84,
    CED_GRAPH_MAX_NODES = 64, CED_GRAPH_MAX_LINKS = 256, CED_GRAPH_MAX_COORDINATE = (1 << 23) - 1
};
enum { GRAPH_TOP, GRAPH_RIGHT, GRAPH_BOTTOM, GRAPH_LEFT, GRAPH_SIDE_COUNT };
enum { GRAPH_SOLID, GRAPH_DOTTED, GRAPH_DASHED, GRAPH_STYLE_COUNT };
enum { GRAPH_NONE, GRAPH_FORWARD, GRAPH_BACKWARD, GRAPH_BOTH, GRAPH_DIRECTION_COUNT };

extern const char *const ced_graph_sides[GRAPH_SIDE_COUNT];
extern const char *const ced_graph_styles[GRAPH_STYLE_COUNT];
extern const char *const ced_graph_directions[GRAPH_DIRECTION_COUNT];

typedef struct { int x, y, w, h; char label[128 * 4 + 1]; } CedGraphNode;
typedef struct { int node, side; double offset; } CedGraphAnchor;
typedef struct { CedGraphAnchor from, to; int style, direction; } CedGraphLink;
typedef struct {
    int node_count, link_count;
    CedGraphNode nodes[CED_GRAPH_MAX_NODES];
    CedGraphLink links[CED_GRAPH_MAX_LINKS];
} CedExtensionGraph;

CedExtensionGraph *ced_extension_graph_decode(const cJSON *value, char *error);

#endif
