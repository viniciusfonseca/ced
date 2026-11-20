#include "extension_ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

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
