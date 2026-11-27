#include "graph.h"
#include "json.h"
#include "file_operations.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const ced_graph_sides[] = {"top", "right", "bottom", "left"};
const char *const ced_graph_styles[] = {"solid", "dotted", "dashed"};
const char *const ced_graph_directions[] = {"none", "forward", "backward", "both"};

static int choice(const cJSON *value, const char *key, const char *const choices[], int count) {
    const char *text = ced_ext_json_string(value, key);
    if (text) for (int i = 0; i < count; i++) if (!strcmp(text, choices[i])) return i;
    return -1;
}

static int integer(const cJSON *value, const char *key, int max) {
    uint64_t n;
    return ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(value, key), &n) && n <= (uint64_t)max ? (int)n : -1;
}

static bool coordinate(const cJSON *value, const char *key, int *out) {
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(value, key);
    if (!cJSON_IsNumber(n) || !(fabs(n->valuedouble) <= CED_GRAPH_MAX_COORDINATE) ||
        trunc(n->valuedouble) != n->valuedouble) return false;
    *out = (int)n->valuedouble;
    return true;
}

static bool decode_node(const cJSON *value, CedGraphNode *node, char *error) {
    static const char *const fields[] = {"label", "x", "y", "width", "height", NULL};
    if (!ced_ext_json_fields(value, fields, error)) return false;
    const char *label = ced_ext_json_string(value, "label");
    node->w = integer(value, "width", CED_GRAPH_WIDTH);
    node->h = integer(value, "height", CED_GRAPH_HEIGHT);
    if (!label || strlen(label) >= sizeof node->label || node->w < 40 || node->h < 28 ||
        !coordinate(value, "x", &node->x) || !coordinate(value, "y", &node->y)) return false;
    memcpy(node->label, label, strlen(label) + 1);
    return true;
}

static bool decode_anchor(const cJSON *value, int nodes, CedGraphAnchor *out, char *error) {
    static const char *const fields[] = {"node", "side", "offset", NULL};
    if (!ced_ext_json_fields(value, fields, error) || !nodes) return false;
    int node = integer(value, "node", nodes - 1);
    int side = choice(value, "side", ced_graph_sides, GRAPH_SIDE_COUNT);
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(value, "offset");
    if (node < 0 || side < 0 || !cJSON_IsNumber(offset) ||
        !(offset->valuedouble >= 0 && offset->valuedouble <= 1)) return false;
    *out = (CedGraphAnchor){node, side, offset->valuedouble};
    return true;
}

static bool decode_link(const cJSON *value, CedExtensionGraph *graph, CedGraphLink *link, char *error) {
    static const char *const fields[] = {"from", "to", "style", "direction", NULL};
    if (!ced_ext_json_fields(value, fields, error)) return false;
    link->style = choice(value, "style", ced_graph_styles, GRAPH_STYLE_COUNT);
    link->direction = choice(value, "direction", ced_graph_directions, GRAPH_DIRECTION_COUNT);
    if (link->style < 0 || link->direction < 0 ||
        !decode_anchor(cJSON_GetObjectItemCaseSensitive(value, "from"), graph->node_count, &link->from, error) ||
        !decode_anchor(cJSON_GetObjectItemCaseSensitive(value, "to"), graph->node_count, &link->to, error)) return false;
    return link->from.node != link->to.node || link->from.side != link->to.side || link->from.offset != link->to.offset;
}

CedExtensionGraph *ced_extension_graph_decode(const cJSON *value, char *error) {
    static const char *const fields[] = {"nodes", "links", NULL};
    if (!ced_ext_json_fields(value, fields, error)) return NULL;
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(value, "nodes");
    const cJSON *links = cJSON_GetObjectItemCaseSensitive(value, "links");
    if (!cJSON_IsArray(nodes) || !cJSON_IsArray(links) || cJSON_GetArraySize(nodes) > CED_GRAPH_MAX_NODES ||
        cJSON_GetArraySize(links) > CED_GRAPH_MAX_LINKS) {
        snprintf(error, CED_ERROR, "Invalid extension graph size or fields.");
        return NULL;
    }
    CedExtensionGraph *graph = calloc(1, sizeof *graph);
    if (!graph) {
        snprintf(error, CED_ERROR, "Could not allocate extension graph.");
        return NULL;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, nodes) {
        if (!decode_node(item, &graph->nodes[graph->node_count], error)) goto invalid;
        graph->node_count++;
    }
    cJSON_ArrayForEach(item, links) {
        if (!decode_link(item, graph, &graph->links[graph->link_count], error)) goto invalid;
        graph->link_count++;
    }
    return graph;
invalid:
    if (!*error) snprintf(error, CED_ERROR, "Invalid extension graph node or link.");
    free(graph);
    return NULL;
}
