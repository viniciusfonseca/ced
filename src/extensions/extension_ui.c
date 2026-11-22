#include "extension_ui.h"
#include "cui_internal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

const CedExtensionTab *ced_extension_tab_find(CedExtensions *m, const char *id,
    CedExtensionInstance **owner, size_t *index) {
    if (!m || !*id) return NULL;
    for (size_t n = 0; n < m->count; n++) {
        CedExtensionInstance *instance = &m->instances[n];
        if (!instance->record.valid || !instance->record.enabled || (m->reload && !instance->rebind_workspace)) continue;
        for (size_t t = 0; t < instance->record.manifest.tab_count; t++) {
            const CedExtensionTab *tab = &instance->record.manifest.tabs[t];
            if (strcmp(id, tab->id)) continue;
            if (owner) *owner = instance;
            if (index) *index = t;
            return tab;
        }
    }
    return NULL;
}

bool ced_extension_tab_buttons(cui_ctx *ui, CedExtensions *m, bool terminal, char *selected) {
    bool clicked = false;
    if (!m) return false;
    for (size_t n = 0; n < m->count; n++) {
        CedExtensionInstance *instance = &m->instances[n];
        const CedExtensionRecord *record = &instance->record;
        if (!record->valid || !record->enabled || (m->reload && !instance->rebind_workspace)) continue;
        for (size_t t = 0; t < record->manifest.tab_count; t++) {
            const CedExtensionTab *tab = &record->manifest.tabs[t];
            if (tab->terminal != terminal) continue;
            cui_push_id(ui, tab->id);
            const cui_button_opts options = {.size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                .tooltip = tab->title, .selected = !strcmp(tab->id, selected)};
            if (terminal ? cui_button_ex(ui, tab->title, &options) : cui_icon_button_ex(ui, CUI_ICON_PACKAGE, &options)) {
                snprintf(selected, 257, "%s", tab->id);
                clicked = true;
            }
            cui_pop_id(ui);
        }
    }
    return clicked;
}

bool ced_extension_has_tabs(const CedExtensions *m, bool terminal) {
    if (!m) return false;
    for (size_t n = 0; n < m->count; n++) {
        const CedExtensionInstance *instance = &m->instances[n];
        const CedExtensionRecord *record = &instance->record;
        if (!record->valid || !record->enabled || (m->reload && !instance->rebind_workspace)) continue;
        for (size_t t = 0; t < record->manifest.tab_count; t++)
            if (record->manifest.tabs[t].terminal == terminal) return true;
    }
    return false;
}

static bool composer_selector(const cJSON *selector) {
    const char *placement = ced_ext_json_string(selector, "placement");
    return placement && !strcmp(placement, "composer");
}

static void tab_selectors(cui_ctx *ui, CedExtensionInstance *instance, size_t index,
    const cJSON *selectors, bool has_composer, bool *busy) {
    const cJSON *entry;
    bool any = false;
    cJSON_ArrayForEach(entry, selectors)
        if (!has_composer || !composer_selector(entry)) any = true;
    if (!any) return;
    cui_row(ui, .width = CUI_FILL, .gap = 6) {
        const cJSON *selector;
        cJSON_ArrayForEach(selector, selectors) {
            if (has_composer && composer_selector(selector)) continue;
            const char *label = ced_ext_json_string(selector, "label");
            const char *id = ced_ext_json_string(selector, "id");
            const char *selected = ced_ext_json_string(selector, "selected");
            const char *labels[CED_EXTENSION_TAB_CHOICES], *values[CED_EXTENSION_TAB_CHOICES];
            int choice_count = 0, current = -1;
            const cJSON *option;
            cJSON_ArrayForEach(option, cJSON_GetObjectItemCaseSensitive(selector, "options")) {
                values[choice_count] = ced_ext_json_string(option, "id");
                labels[choice_count] = ced_ext_json_string(option, "label");
                if (!strcmp(selected, values[choice_count])) current = choice_count;
                choice_count++;
            }
            bool disabled = *busy || choice_count == 0 || cui_is_disabled(ui) ||
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(selector, "disabled"));
            bool changed = cui_select_ex(ui, id, labels, choice_count, &current, &(cui_select_opts){
                .label = label, .placeholder = "Unavailable",
                .width = CUI_FILL, .disabled = disabled, .searchable = choice_count > 8});
            if (changed && !disabled && current >= 0 && current < choice_count)
                *busy = ced_extensions_tab_action(instance, index, id, values[current]);
        }
    }
}

static void tab_buttons(cui_ctx *ui, CedExtensionInstance *instance, size_t index,
    const cJSON *actions, bool *busy) {
    const cJSON *action;
    cJSON_ArrayForEach(action, actions) {
        const char *id = ced_ext_json_string(action, "id");
        cui_push_id(ui, id);
        if (cui_button(ui, ced_ext_json_string(action, "label"), .size = CUI_SIZE_SM,
                .variant = CUI_BUTTON_SECONDARY, .disabled = *busy))
            *busy = ced_extensions_tab_action(instance, index, id, "");
        cui_pop_id(ui);
    }
}

static bool tab_list_button(cui_ctx *ui, const char *label, const char *icon, bool busy) {
    int node = ui->node_count;
    bool clicked = cui_button(ui, label, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
        .width = CUI_FILL, .icon = icon, .tooltip = icon ? NULL : label, .disabled = busy);
    ui->nodes[node].justify = CUI_JUSTIFY_START;
    ui->nodes[node].flags |= CUI_NF_CLIP;
    return clicked;
}

static void tab_groups(cui_ctx *ui, CedExtensionInstance *instance, size_t index,
    const cJSON *groups, bool *busy) {
    cui_theme *theme = cui_theme_get(ui);
    float radius = theme->radius_control;
    theme->radius_control = 0;
    const cJSON *group, *item;
    cJSON_ArrayForEach(group, groups) {
        const char *create = ced_ext_json_string(group, "create");
        cui_push_id(ui, create);
        cui_column(ui, .width = CUI_FILL, .gap = 0, .pad_y = 4) {
            cui_text(ui, ced_ext_json_string(group, "label"), .width = CUI_FILL, .size = 12,
                .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
            cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(group, "items")) {
                const char *id = ced_ext_json_string(item, "id");
                cui_push_id(ui, id);
                cui_row(ui, .width = CUI_FILL, .gap = 0, .align = CUI_ALIGN_CENTER, .clip = true) {
                    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "busy"))) {
                        cui_row(ui, .id = "session-progress", .pad_x = 5, .align = CUI_ALIGN_CENTER) {
                            cui_spinner(ui, 14, .style = CUI_SPINNER_ARC);
                        }
                    }
                    if (tab_list_button(ui, ced_ext_json_string(item, "label"), NULL, *busy))
                        *busy = ced_extensions_tab_action(instance, index, id, "");
                    if (cui_icon_button(ui, CUI_ICON_TRASH_2, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                            .tooltip = "Delete session", .disabled = *busy))
                        *busy = ced_extensions_tab_action(instance, index, ced_ext_json_string(item, "delete"), "");
                }
                cui_pop_id(ui);
            }
            if (tab_list_button(ui, "New Session", CUI_ICON_PLUS, *busy))
                *busy = ced_extensions_tab_action(instance, index, create, "");
        }
        cui_pop_id(ui);
    }
    theme->radius_control = radius;
}

static bool tab_activity(cui_ctx *ui, const cJSON *message, const char *role) {
    bool reasoning = !strcmp(role, "reasoning");
    if (!reasoning && strcmp(role, "tool")) return false;
    const char *id = ced_ext_json_string(message, "id");
    const char *text = ced_ext_json_string(message, "text");
    const char *status = ced_ext_json_string(message, "status");
    bool running = status && !strcmp(status, "running");
    cui_push_id(ui, "chat-activity");
    if (reasoning) {
        cui_reasoning(ui, id, .thinking = running, .title = running ? "Thinking" : "Reasoning") {
            cui_markdown(ui, "markdown", text, .compact = true, .streaming = running, .color = CUI_COLOR_TEXT_SECONDARY);
        }
    } else {
        cui_task_status task = CUI_TASK_PENDING;
        if (running) task = CUI_TASK_RUNNING;
        else if (!strcmp(status, "done")) task = CUI_TASK_DONE;
        else if (!strcmp(status, "failed")) task = CUI_TASK_FAILED;
        cui_tool_call(ui, id, text, .status = task, .input = ced_ext_json_string(message, "input"),
            .output = ced_ext_json_string(message, "output"));
    }
    cui_pop_id(ui);
    return true;
}

typedef struct {
    int length, visible;
    uint32_t hash;
    float characters;
    bool initialized;
} ChatTyping;

static const char *chat_typed_text(cui_ctx *ui, const char *id, const char *text, bool animate) {
    ChatTyping *typing = cui_state(ui, cui_make_id(ui, id), sizeof *typing);
    int length = (int)strlen(text);
    if ((!typing->initialized && !animate) || cui_theme_get(ui)->motion <= 0 ||
        (typing->initialized && (length < typing->length ||
            cui__hash(text, (size_t)typing->length, 0) != typing->hash))) {
        typing->visible = length;
        typing->characters = 0;
    }
    typing->initialized = true;
    typing->length = length;
    typing->hash = cui__hash(text, (size_t)length, 0);
    if (typing->visible < length) {
        // Match the CUI typewriter's catch-up behavior while keeping Markdown in the bubble.
        float boost = fmaxf(1, (length - typing->visible) / 120.0f);
        typing->characters += cui_delta_time(ui) * 140 * boost;
        while (typing->characters >= 1 && typing->visible < length) {
            typing->visible = cui__utf8_next(text, length, typing->visible);
            typing->characters--;
        }
        if (typing->visible < length) {
            cui_request_redraw(ui);
            return cui__frame_printf(ui, "%.*s|", typing->visible, text);
        }
        typing->characters = 0;
    }
    return text;
}

static void tab_chat(cui_ctx *ui, const cJSON *chat, const char *text, bool animate) {
    const char *author = ced_ext_json_string(chat, "author");
    const char *previous_role = NULL;
    const cJSON *message;
    cJSON_ArrayForEach(message, cJSON_GetObjectItemCaseSensitive(chat, "messages")) {
        const char *role = ced_ext_json_string(message, "role");
        if (tab_activity(ui, message, role)) {
            previous_role = NULL;
            continue;
        }
        const char *next_role = ced_ext_json_string(message->next, "role");
        bool mine = !strcmp(role, "user");
        const char *body = ced_ext_json_string(message, "text");
        const char *id = mine ? NULL : ced_ext_json_string(message, "id");
        if (id) {
            cui_push_id(ui, "chat-typewriter");
            body = chat_typed_text(ui, id, body, animate);
            cui_pop_id(ui);
        }
        int row = ui->node_count;
        cui_chat_message(ui, body, .mine = mine, .author = author,
            .grouped = previous_role && !strcmp(role, previous_role),
            .last_in_group = !next_role || strcmp(role, next_role), .markdown = !mine);
        // CUI can wrap inside long words, but its default minimum width uses the longest word.
        for (int n = row + 1; n < ui->node_count; n++) {
            cui_node *node = &ui->nodes[n];
            if ((node->flags & (CUI_NF_TEXT | CUI_NF_WRAP)) == (CUI_NF_TEXT | CUI_NF_WRAP))
                node->flags |= CUI_NF_ADAPTIVE;
        }
        if (id) {
            // The CUI bubble otherwise fades back in for each newly revealed prefix.
            ui->nodes[row].opacity = 1;
            ui->nodes[row].ty = 0;
        }
        previous_role = role;
    }
    if (!*text) return;
    const char *status = ced_ext_json_string(chat, "status");
    if (!status) cui_text(ui, text, .width = CUI_FILL);
    else if (!strcmp(status, "pending")) cui_text(ui, text, .width = CUI_FILL, .color = CUI_COLOR_TEXT_SECONDARY);
    else cui_agent_status(ui, text, .status = !strcmp(status, "failed") ? CUI_TASK_FAILED : CUI_TASK_RUNNING);
}

static float chat_scroll_bottom(const cui_scroll_state *scroll) {
    return fmaxf(0, scroll->content_h - scroll->view_h);
}

static void chat_scroll_layout(cui_ctx *ui, cui_node *node, void *data) {
    const CedExtensionTabState *tab = data;
    const cui_scroll_state *scroll = cui_state(ui, node->id, sizeof *scroll);
    if (fabsf(chat_scroll_bottom(scroll) - tab->chat_bottom) > 0.5f) cui_request_redraw(ui);
}

static void tab_chat_scroll(cui_ctx *ui, CedExtensionTabState *tab, const cJSON *chat, const char *text) {
    const cui_id id = cui_make_id(ui, "chat");
    cui_scroll_state *scroll = cui_state(ui, id, sizeof *scroll);
    float bottom = chat_scroll_bottom(scroll);
    if (!tab->chat_open) {
        tab->chat_open = true;
        tab->chat_following = true;
        tab->chat_bottom = bottom;
        scroll->y = scroll->ty = bottom;
    }
    cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL,
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_END) {
        cui_scroll(ui, .id = "chat", .height = CUI_FILL, .width = CUI_FILL, .gap = 4) {
            cui_node *node = &ui->nodes[cui__current(ui)];
            node->paint = chat_scroll_layout;
            node->paint_data = tab;
            // Read the target after CUI handles the wheel and scrollbar.
            if (scroll->ty + 0.5f >= bottom) tab->chat_following = true;
            else if (scroll->ty + 0.5f < fminf(tab->chat_bottom, bottom) || scroll->drag_y)
                tab->chat_following = false;
            tab->chat_bottom = bottom;
            if (tab->chat_following && scroll->ty < bottom) {
                cui_scroll_to(ui, "chat", bottom);
                cui_request_redraw(ui);
            }
            tab_chat(ui, chat, text, tab->chat_animate);
        }
        if (!tab->chat_following) {
            cui_row(ui, .width = CUI_FIT, .pad_bottom = 10) {
                if (cui__icon_button_id(ui, cui_make_id(ui, "chat-jump"), CUI_ICON_ARROW_DOWN,
                        &(cui_button_opts){.size = CUI_SIZE_SM, .variant = CUI_BUTTON_SECONDARY,
                                           .tooltip = "Jump to latest"})) {
                    tab->chat_following = true;
                    cui_scroll_to(ui, "chat", bottom);
                    cui_request_redraw(ui);
                }
            }
        }
    }
}

/* The native picker has a short menu. Larger catalogs are searchable from it. */
enum { COMPOSER_QUICK_CHOICES = 8, COMPOSER_LABEL_BYTES = 128 };

static void fit_model_label(cui_ctx *ui, cui_model *model, char *label, size_t capacity, float available) {
    const char *name = model->name;
    const cui_theme *theme = cui_theme_get(ui);
    float size = theme->text_sm * theme->font_scale;
    size_t length = strlen(name);
    if (!length || cui_measure_text(ui, CUI_FONT_MEDIUM, size, name, (int)length).width <= available) return;
    available -= cui_measure_text(ui, CUI_FONT_MEDIUM, size, "…", -1).width;
    if (available < 0) {
        model->name = "";
        model->detail = name;
        return;
    }
    size_t end = length;
    do {
        do { end--; } while (end && !ced_utf8_boundary(name, length, end));
    } while (end && cui_measure_text(ui, CUI_FONT_MEDIUM, size, name, (int)end).width > available);
    snprintf(label, capacity, "%.*s…", (int)end, name);
    model->name = label;
    model->detail = name;
}

typedef struct {
    const char *id, *label;
    const char *values[CED_EXTENSION_TAB_CHOICES];
    cui_command catalog[CED_EXTENSION_TAB_CHOICES];
    cui_model quick[COMPOSER_QUICK_CHOICES];
    int mapped[COMPOSER_QUICK_CHOICES];
    int count, current, quick_count, picked;
    bool disabled, overflow;
    char compact[COMPOSER_LABEL_BYTES + 4];
} ComposerPicker;

static void composer_picker(ComposerPicker *picker, const cJSON *selector, bool disabled, const char *browse_label) {
    if (!selector) return;
    picker->id = ced_ext_json_string(selector, "id");
    picker->label = ced_ext_json_string(selector, "label");
    const char *selected = ced_ext_json_string(selector, "selected");
    picker->current = -1;
    const cJSON *option;
    cJSON_ArrayForEach(option, cJSON_GetObjectItemCaseSensitive(selector, "options")) {
        int n = picker->count++;
        picker->values[n] = ced_ext_json_string(option, "id");
        picker->catalog[n].title = ced_ext_json_string(option, "label");
        picker->catalog[n].group = picker->label;
        if (!strcmp(selected, picker->values[n])) picker->current = n;
    }
    picker->disabled = disabled || !picker->count || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(selector, "disabled"));
    picker->overflow = picker->count + (picker->current < 0) > COMPOSER_QUICK_CHOICES;
    if (picker->current < 0 || picker->overflow || picker->disabled) {
        picker->mapped[0] = picker->current;
        picker->quick[0].name = picker->current < 0 ? picker->label : picker->catalog[picker->current].title;
        picker->quick_count = 1;
    }
    if (picker->disabled) return;
    if (picker->overflow) {
        picker->mapped[1] = -1;
        picker->quick[1].name = browse_label;
        picker->quick_count = 2;
    } else {
        for (int n = 0; n < picker->count; n++) {
            if (n == picker->current) picker->picked = picker->quick_count;
            picker->mapped[picker->quick_count] = n;
            picker->quick[picker->quick_count++].name = picker->catalog[n].title;
        }
    }
}

static int composer_picker_choice(cui_ctx *ui, const ComposerPicker *picker, const char *browse_id, bool changed) {
    if (!picker->id) return -1;
    bool *browse = cui_state(ui, cui_make_id(ui, browse_id), sizeof *browse);
    if (picker->disabled || !picker->overflow) *browse = false;
    int choice = -1;
    if (changed && !picker->disabled) {
        choice = picker->mapped[picker->picked];
        if (picker->overflow && picker->picked == 1) *browse = true;
    }
    if (picker->overflow) {
        cui_push_id(ui, browse_id);
        int result = cui_command_palette(ui, browse, picker->catalog, picker->count);
        cui_pop_id(ui);
        if (result >= 0 && !picker->disabled) choice = result;
    }
    return choice;
}

static bool draw_composer_picker(cui_ctx *ui, const char *id, ComposerPicker *picker) {
    cui_push_disabled(ui, picker->disabled);
    bool changed = cui_model_picker(ui, id, picker->quick, picker->quick_count, &picker->picked, .ghost = true);
    cui_tooltip(ui, picker->label);
    cui_pop_disabled(ui);
    return changed;
}

static void tab_composer(cui_ctx *ui, CedExtensionInstance *instance, size_t index,
    const cJSON *input, const cJSON *selectors, bool *busy) {
    CedExtensionTabState *state = &instance->tabs[index];
    const char *stop = ced_ext_json_string(input, "stop");
    bool disabled = *busy || state->submitted || stop || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "disabled"));
    const char *const scopes[] = {NULL, "secondary-picker", "approval-picker"};
    const char *const browse[] = {"browse-models", "browse-effort", "browse-approval"};
    const char *const labels[] = {"Browse all models…", "Browse all options…", "Browse all approval modes…"};
    ComposerPicker pickers[3] = {0};
    const cJSON *selector;
    int count = 0;
    cJSON_ArrayForEach(selector, selectors) {
        if (composer_selector(selector)) {
            composer_picker(&pickers[count], selector, disabled, labels[count]);
            count++;
        }
    }
    float width = cui_get_rect(ui, cui_make_id(ui, "tab-input")).w;
    if (width > 0 && count) {
        // Reserve space for Attach, Send, the chevrons, padding, and gaps.
        float available = fmaxf(0, width - (80 + 43 * count));
        float secondary = count > 1 ? fminf(160, available / (count + 0.5f)) : 0;
        for (int n = 0; n < count; n++) {
            ComposerPicker *picker = &pickers[n];
            float label_width = n ? secondary : available - secondary * (count - 1);
            fit_model_label(ui, &picker->quick[picker->picked], picker->compact, sizeof picker->compact, label_width);
        }
    }

    int event = 0;
    bool changed[3] = {0};
    cui_push_disabled(ui, *busy || state->submitted || (disabled && !stop));
    cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END) {
        event = cui_chat_composer(ui, "tab-input", state->input, sizeof state->input,
            .placeholder = ced_ext_json_string(input, "placeholder"), .width = CUI_FILL, .max_lines = 3, .busy = stop != NULL);
        if (count) {
            // The attach button occupies 32 px plus the composer's 10 px inset and 6 px gap.
            cui_row(ui, .width = CUI_FIT, .pad_left = 48, .pad_bottom = 11, .gap = 6) {
                cui_push_id(ui, "tab-input");
                for (int n = 0; n < count; n++) {
                    if (scopes[n]) cui_push_id(ui, scopes[n]);
                    changed[n] = draw_composer_picker(ui, n ? pickers[n].id : "model", &pickers[n]);
                    if (scopes[n]) cui_pop_id(ui);
                }
                cui_pop_id(ui);
            }
        }
    }
    cui_pop_disabled(ui);

    int choices[3];
    for (int n = 0; n < count; n++)
        choices[n] = composer_picker_choice(ui, &pickers[n], browse[n], changed[n]);
    if (stop && !*busy && (event & CUI_COMPOSER_STOP)) {
        *busy = ced_extensions_tab_action(instance, index, stop, "");
        return;
    }
    if (disabled || *busy) return;
    for (int n = 0; n < count; n++) {
        ComposerPicker *picker = &pickers[n];
        if (choices[n] >= 0 && choices[n] != picker->current) {
            *busy = ced_extensions_tab_action(instance, index, picker->id, picker->values[choices[n]]);
            return;
        }
    }
    if ((event & CUI_COMPOSER_SEND) && *state->input) {
        *busy = ced_extensions_tab_action(instance, index, ced_ext_json_string(input, "id"), state->input);
        state->submitted = *busy;
    } else if (event & CUI_COMPOSER_ATTACH) {
        cui_toast_ex(ui, "File attachments are unavailable in this chat.", NULL);
    }
}

void ced_extension_tab_body(cui_ctx *ui, CedExtensions *m, const char *selected) {
    CedExtensionInstance *instance;
    size_t index;
    if (!ced_extension_tab_find(m, selected, &instance, &index)) return;
    cui_push_id(ui, selected);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 6) {
        CedExtensionTabState *state = &instance->tabs[index];
        if (instance->rebind_workspace && !m->trusted) {
            cui_secondary(ui, "Loading workspace…");
        } else if (!m->trusted) {
            if (cui_button(ui, "Trust Workspace", .size = CUI_SIZE_SM, .disabled = m->job.running))
                m->trust_open = true;
        } else {
            bool busy = m->job.running || instance->state == EXT_STOPPING || *instance->pending;
            if (!state->requested && !busy && instance->state != EXT_FAILED && !*instance->error) {
                state->requested = true;
                ced_extensions_tab_action(instance, index, "open", "");
                busy = *instance->command != 0;
            }
            if (*instance->error) cui_text(ui, instance->error, .size = 12, .width = CUI_FILL);
            if (state->model) {
                const cJSON *model = state->model;
                const char *heading = ced_ext_json_string(model, "heading");
                const cJSON *chat = cJSON_GetObjectItemCaseSensitive(model, "chat");
                const cJSON *actions = cJSON_GetObjectItemCaseSensitive(model, "actions");
                const cJSON *input = cJSON_GetObjectItemCaseSensitive(model, "input");
                const cJSON *selectors = cJSON_GetObjectItemCaseSensitive(model, "selectors");
                const cJSON *groups = cJSON_GetObjectItemCaseSensitive(model, "groups");
                const char *text = ced_ext_json_string(model, "text");
                if (chat) {
                    cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER, .wrap = true) {
                        const char *back = ced_ext_json_string(chat, "back");
                        cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER) {
                            if (back && cui__icon_button_id(ui, cui_make_id(ui, "chat-back"), CUI_ICON_CHEVRON_LEFT,
                                    &(cui_button_opts){.size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "Back to sessions", .disabled = busy}))
                                busy = ced_extensions_tab_action(instance, index, back, "");
                            if (*heading) cui_text(ui, heading, .font = CUI_FONT_MEDIUM, .nowrap = true, .width = CUI_FILL);
                        }
                        tab_buttons(ui, instance, index, actions, &busy);
                    }
                } else if (*heading) cui_text(ui, heading, .font = CUI_FONT_MEDIUM, .width = CUI_FILL);
                tab_selectors(ui, instance, index, selectors, chat && input, &busy);
                if (chat) tab_chat_scroll(ui, state, chat, text);
                else {
                    state->chat_open = false;
                    cui_scroll(ui, .id = "contents", .height = CUI_FILL, .width = CUI_FILL, .gap = 4) {
                        if (*text) cui_text(ui, text, .width = CUI_FILL);
                        if (groups) tab_groups(ui, instance, index, groups, &busy);
                        tab_buttons(ui, instance, index, actions, &busy);
                    }
                }
                const cJSON *approval = cJSON_GetObjectItemCaseSensitive(chat, "approval");
                if (approval) {
                    cui_push_id(ui, "chat-approval");
                    cui_column(ui, .width = CUI_FILL, .pad = 6, .gap = 4, .bg = CUI_COLOR_SURFACE_MUTED) {
                        cui_text(ui, ced_ext_json_string(approval, "title"), .width = CUI_FILL, .font = CUI_FONT_MEDIUM);
                        cui_row(ui, .width = CUI_FILL, .gap = 4, .wrap = true) {
                            tab_buttons(ui, instance, index, cJSON_GetObjectItemCaseSensitive(approval, "actions"), &busy);
                        }
                    }
                    cui_pop_id(ui);
                }
                if (input) {
                    const char *input_id = ced_ext_json_string(input, "id");
                    const char *placeholder = ced_ext_json_string(input, "placeholder");
                    bool disabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "disabled"));
                    cui_push_id(ui, input_id);
                    if (chat) tab_composer(ui, instance, index, input, selectors, &busy);
                    else cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_END) {
                        int changed = cui_input(ui, "tab-input", state->input, sizeof state->input,
                            .placeholder = placeholder, .width = CUI_FILL,
                            .disabled = disabled || state->submitted);
                        bool send = cui_button(ui, ced_ext_json_string(input, "label"), .size = CUI_SIZE_SM,
                            .variant = CUI_BUTTON_PRIMARY, .disabled = busy || disabled || !*state->input);
                        if (!busy && !disabled && *state->input && (send || (changed & CUI_SUBMITTED))) {
                            busy = ced_extensions_tab_action(instance, index, input_id, state->input);
                            state->submitted = busy;
                        }
                    }
                    cui_pop_id(ui);
                }
                if (!busy && !*instance->error && ced_ext_now() >= state->next_refresh &&
                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(model, "refresh"))) {
                    state->next_refresh = ced_ext_now() + .75;
                    ced_extensions_tab_action(instance, index, "refresh", "");
                }
            } else if (busy) cui_secondary(ui, "Loading…");
            else if (cui_button(ui, "Open", .size = CUI_SIZE_SM)) {
                if (instance->state == EXT_FAILED && !instance->process) instance->state = EXT_REGISTERED;
                ced_extensions_tab_action(instance, index, "open", "");
            }
        }
    }
    cui_pop_id(ui);
}

bool ced_extensions_panel(cui_ctx *ui, const CedExtensions *m) {
    bool install = false;
    bool updating = m && (m->job.running || m->reload);
    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 6) {
        cui_text(ui, "Extensions", .font = CUI_FONT_MEDIUM, .width = CUI_FILL);
        install = cui_icon_button(ui, CUI_ICON_PLUS, .size = CUI_SIZE_SM,
            .variant = CUI_BUTTON_GHOST, .tooltip = "Install Extension from Folder",
            .disabled = !m || updating || m->review_open);
    }
    if (!m) {
        cui_secondary(ui, "Extensions are disabled.");
        return false;
    }
    if (updating) cui_secondary(ui, "Updating extensions...");
    else if (!m->count) cui_secondary(ui, "No installed extensions");
    if (*m->error) cui_text(ui, m->error, .size = 12, .width = CUI_FILL);
    cui_scroll(ui, .id = "installed-extensions", .height = CUI_FILL, .gap = 8) {
        for (size_t n = 0; n < m->count; n++) {
            const CedExtensionInstance *instance = &m->instances[n];
            const CedExtensionManifest *manifest = &instance->record.manifest;
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                cui_text(ui, *manifest->name ? manifest->name : manifest->id,
                    .size = 13, .font = CUI_FONT_MEDIUM, .nowrap = true, .width = CUI_FILL);
                char detail[128];
                snprintf(detail, sizeof detail, "%s%s%s", manifest->version,
                    *manifest->version ? " · " : "", ced_extensions_status(instance));
                cui_text(ui, detail, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY,
                    .nowrap = true, .width = CUI_FILL);
            }
        }
    }
    return install;
}

void ced_extensions_ui(cui_ctx *ui, CedExtensions *m) {
    if (!m) return;
    cui_dialog(ui, "Manage Extensions · Experimental", &m->manage_open, .width = 720) {
        cui_text(ui, m->trusted ? "Trusted workspace" : "Untrusted workspace", .size = 13);
        cui_text(ui, m->workspace, .size = 13, .nowrap = true, .width = CUI_FILL);
        if (!m->trusted) { if (cui_button(ui, "Trust Workspace", .size = CUI_SIZE_SM)) m->trust_open = true; }
        else if (cui_button(ui, "Revoke Workspace Trust", .size = CUI_SIZE_SM) && !m->job.running) {
            m->trusted = false;
            ced_extensions_job(m, EXT_REVOKE, NULL);
        }
        if (m->job.running || m->reload) cui_text(ui, "Updating extensions...", .size = 13);
        if (*m->error) cui_text(ui, m->error, .size = 13, .nowrap = true, .width = CUI_FILL);
        if (!m->count) cui_text(ui, "No installed extensions", .size = 13);
        CedCommandContext active = m->host.context(m->host.user);
        CedExtensionDocument document;
        for (size_t n = 0; active.has_document && n < 32 && m->host.document && m->host.document(m->host.user, n, &document); n++) {
            if (document.document->id != active.document_id) continue;
            char label[CED_PATH + 32];
            snprintf(label, sizeof label, "Active document: %s", *document.document->path ? document.document->path : "Untitled");
            cui_text(ui, label, .size = 13, .nowrap = true, .width = CUI_FILL);
            break;
        }
        cui_scroll(ui, .id = "extension-list", .height = CUI_PX(fmaxf(100, fminf(340, cui_viewport_size(ui).y - 460))), .gap = 12) {
            cui_theme *theme = cui_theme_get(ui);
            float radius = theme->radius_control;
            theme->radius_control = 0;
            for (size_t n = 0; n < m->count; n++) {
                CedExtensionInstance *i = &m->instances[n];
                cui_push_id_int(ui, (int)n);
                char label[700]; snprintf(label, sizeof label, "%s %s · %s", i->record.manifest.name, i->record.manifest.version, ced_extensions_status(i));
                cui_text(ui, label, .size = 13);
                cui_text(ui, i->record.manifest.id, .size = 13);
                cui_text(ui, i->record.source, .size = 12, .nowrap = true, .width = CUI_FILL);
                const char *permissions = (i->record.granted & CED_EXT_EDIT) ? "Permissions: read, edit" :
                    (i->record.granted & CED_EXT_READ) ? "Permissions: read" : "Permissions: none";
                cui_text(ui, permissions, .size = 12);
                char statistics[160];
                snprintf(statistics, sizeof statistics, "Pending: %d · Queued: %zu bytes · Activation: %.1f ms",
                    *i->pending ? 1 : 0, i->process ? ced_extension_process_queued(i->process) : 0, i->activation_ms);
                cui_text(ui, statistics, .size = 12);
                if (*i->record.error) cui_text(ui, i->record.error, .size = 13);
                if (*i->error) cui_text(ui, i->error, .size = 13);
                cui_row(ui, .gap = 6) {
                    if (i->record.valid && cui_button(ui, i->record.enabled ? "Disable" : "Enable", .size = CUI_SIZE_SM) && !m->job.running)
                        ced_extensions_job(m, i->record.enabled ? EXT_DISABLE : EXT_ENABLE, i->record.manifest.id);
                    if (i->record.valid && i->record.enabled && cui_button(ui, "Restart", .size = CUI_SIZE_SM)) {
                        ced_extensions_stop(i, "restart");
                        if (!i->process) i->state = EXT_REGISTERED;
                        *i->error = 0;
                    }
                    if (cui_button(ui, "Uninstall", .size = CUI_SIZE_SM) && !m->job.running) ced_extensions_job(m, EXT_UNINSTALL, i->record.manifest.id);
                    if (*i->session && m->host.open_log && cui_button(ui, "Open Log", .size = CUI_SIZE_SM)) {
                        char relative[300], path[CED_PATH];
                        snprintf(relative, sizeof relative, "logs/extensions/%s/%s.log", i->session, i->record.manifest.id);
                        if (ced_join(path, sizeof path, m->profile, relative)) m->host.open_log(m->host.user, path);
                        m->manage_open = false;
                    }
                }
                CedCommandContext context = m->host.context(m->host.user);
                if (m->trusted && i->record.valid && (i->record.granted & CED_EXT_READ) && context.has_document) {
                    size_t granted = i->grant_count;
                    for (size_t g = 0; g < i->grant_count; g++) if (i->document_grants[g] == context.document_id) { granted = g; break; }
                    if (granted < i->grant_count) {
                        if (cui_button(ui, "Revoke Active Document Access", .size = CUI_SIZE_SM)) {
                            ced_extensions_grant_document(i, context.document_id, false);
                        }
                    } else if (i->grant_count < 32 && cui_button(ui, "Allow Active Document for This Session", .size = CUI_SIZE_SM))
                        ced_extensions_grant_document(i, context.document_id, true);
                }
                cui_pop_id(ui);
            }
            theme->radius_control = radius;
        }
        cui_dialog_footer(ui) { if (cui_button(ui, "Close", .size = CUI_SIZE_SM)) m->manage_open = false; }
    }
    cui_dialog(ui, "Install Extension", &m->review_open, .width = 620) {
        cui_scroll(ui, .id = "extension-review", .height = CUI_PX(fmaxf(120, fminf(360, cui_viewport_size(ui).y - 220))), .gap = 12) {
            CedExtensionRecord *r = &m->review;
            cui_text(ui, r->manifest.name, .size = 13);
            cui_text(ui, r->manifest.id, .size = 13);
            cui_text(ui, r->manifest.version, .size = 13);
            cui_text(ui, r->source, .size = 13);
            cui_text(ui, r->hash, .size = 13);
            for (size_t n = 0; n < r->manifest.tab_count; n++) {
                const CedExtensionTab *tab = &r->manifest.tabs[n];
                cui_textf(ui, "%s tab: %s", tab->terminal ? "Terminal" : "Sidebar", tab->title);
            }
            if (r->manifest.capabilities & CED_EXT_READ) cui_text(ui, "Permission: read authorized open documents", .size = 13);
            if (r->manifest.capabilities & CED_EXT_EDIT) cui_text(ui, "Permission: edit authorized open documents", .size = 13);
            cJSON *requirement;
            cJSON_ArrayForEach(requirement, r->manifest.requirements) {
                char text[1028]; snprintf(text, sizeof text, "Requires %s %s", ced_ext_json_string(requirement, "name"), ced_ext_json_string(requirement, "minVersion"));
                cui_text(ui, text, .size = 13);
            }
            if (r->manifest.executable) cui_text(ui, "This executable is not sandboxed and runs with your user permissions.", .size = 13);
            if (*m->error) cui_text(ui, m->error, .size = 13, .nowrap = true, .width = CUI_FILL);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel", .size = CUI_SIZE_SM)) m->review_open = false;
            if (cui_button(ui, "Allow and Install", .size = CUI_SIZE_SM) && !m->job.running) ced_extensions_job(m, EXT_INSTALL, NULL);
        }
    }
    cui_dialog(ui, "Trust Workspace?", &m->trust_open, .width = 620) {
        cui_text(ui, m->workspace, .size = 13, .nowrap = true, .width = CUI_FILL);
        cui_text(ui, "Allow installed extensions to run in this workspace?", .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel", .size = CUI_SIZE_SM)) m->trust_open = false;
            if (cui_button(ui, "Trust", .size = CUI_SIZE_SM) && !m->job.running) {
                ced_extensions_job(m, EXT_TRUST, NULL); m->trust_open = false;
            }
        }
    }

}
