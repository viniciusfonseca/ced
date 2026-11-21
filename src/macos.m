#import <Cocoa/Cocoa.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <cui/cui_app.h>
#include "cui_internal.h"

void ced_macos_set_icon(void) {
    NSString *path = [[NSBundle mainBundle] pathForResource:@"ced" ofType:@"icns"];
    NSImage *icon = path ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
    if (!icon) {
        unsigned long size = 0;
        const uint8_t *bytes = getsectiondata(&_mh_execute_header, "__DATA", "__ced_icon", &size);
        if (bytes && size) icon = [[NSImage alloc] initWithData:[NSData dataWithBytes:bytes length:size]];
    }
    if (icon) [NSApp setApplicationIconImage:icon];
}

void ced_macos_titlebar(cui_app *app) {
    NSWindow *window = (__bridge NSWindow *)cui_app_native_window(app);
    window.styleMask |= NSWindowStyleMaskFullSizeContentView;
    window.titlebarAppearsTransparent = YES;
    window.titleVisibility = NSWindowTitleHidden;
    if (@available(macOS 11.0, *)) window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
}

float ced_macos_layout_titlebar(cui_app *app, cui_rect toolbar) {
    NSWindow *window = (__bridge NSWindow *)cui_app_native_window(app);
    if (window.styleMask & NSWindowStyleMaskFullScreen) return 0;
    // AppKit lays out the traffic lights before applying the toolbar's center.
    if (toolbar.h > 0) [window.contentView.superview layoutSubtreeIfNeeded];
    float right = 0;
    for (NSWindowButton kind = NSWindowCloseButton; kind <= NSWindowZoomButton; kind++) {
        NSButton *button = [window standardWindowButton:kind];
        if (!button || button.hidden) continue;
        if (toolbar.h > 0 && button.superview) {
            NSPoint center = [button.superview convertPoint:NSMakePoint(0, toolbar.y + toolbar.h * 0.5)
                                                  fromView:window.contentView];
            NSRect frame = button.frame;
            NSPoint origin = NSMakePoint(NSMinX(frame), center.y - NSHeight(frame) * 0.5);
            if (origin.y != NSMinY(frame)) [button setFrameOrigin:origin];
        }
        NSRect rect = [window.contentView convertRect:button.bounds fromView:button];
        if (NSMaxX(rect) > right) right = (float)NSMaxX(rect);
    }
    return right;
}

/* Give the toolbar's free space native drag behavior. Forward Cmd-W before
 * the window menu consumes it, and normalize '+' to the equal key for CUI. */
void *ced_macos_window_events(cui_app *app, cui_id drag_region) {
    NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskLeftMouseDown;
    id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event) {
        NSWindow *window = (__bridge NSWindow *)cui_app_native_window(app);
        if (event.window != window) return event;
        if (event.type == NSEventTypeLeftMouseDown) {
            cui_ctx *ui = cui_app_ui(app);
            NSPoint point = [window.contentView convertPoint:event.locationInWindow fromView:nil];
            cui_rect region = cui_get_rect(ui, drag_region);
            if (!(window.styleMask & NSWindowStyleMaskFullScreen) &&
                !ui->modal_layer && !cui__keys_covered(ui) &&
                NSPointInRect(point, NSMakeRect(region.x, region.y, region.w, region.h))) {
                [window performWindowDragWithEvent:event];
                return nil;
            }
            return event;
        }
        NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        if (event.keyCode == 0x31 &&
            (mods & (NSEventModifierFlagControl | NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagShift)) == NSEventModifierFlagControl) {
            cui_input_key(cui_app_ui(app), CUI_KEY_SPACE, event.type == NSEventTypeKeyDown, CUI_MOD_CTRL);
            return nil;
        }
        if ((mods & NSEventModifierFlagCommand) && !(mods & (NSEventModifierFlagOption | NSEventModifierFlagControl))) {
            NSString *key = event.charactersIgnoringModifiers.lowercaseString;
            cui_key mapped = CUI_KEY_NONE;
            if ([key isEqualToString:@"w"] && !(mods & NSEventModifierFlagShift)) mapped = CUI_KEY_W;
            else if ([key isEqualToString:@"+"] || [key isEqualToString:@"="]) mapped = CUI_KEY_EQUAL;
            else if ([key isEqualToString:@"-"]) mapped = CUI_KEY_MINUS;
            if (mapped != CUI_KEY_NONE) {
                int modifiers = CUI_MOD_SUPER | ((mods & NSEventModifierFlagShift) ? CUI_MOD_SHIFT : 0);
                cui_input_key(cui_app_ui(app), mapped, event.type == NSEventTypeKeyDown, modifiers);
                return nil;
            }
        }
        return event;
    }];
    return (__bridge_retained void *)monitor;
}

void ced_macos_remove_window_events(void *handle) {
    if (handle) [NSEvent removeMonitor:(__bridge_transfer id)handle];
}
