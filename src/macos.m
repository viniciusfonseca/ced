#import <Cocoa/Cocoa.h>
#include <cui/cui_app.h>

/* Forward Cmd-W before the window menu consumes it, and normalize '+',
 * which CUI's Cocoa key mapping does not recognize, to the equal key. */
void *ced_macos_shortcuts(cui_app *app) {
    id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | NSEventMaskKeyUp) handler:^NSEvent *(NSEvent *event) {
        NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        if (event.window == (__bridge NSWindow *)cui_app_native_window(app) && event.keyCode == 0x31 &&
            (mods & (NSEventModifierFlagControl | NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagShift)) == NSEventModifierFlagControl) {
            cui_input_key(cui_app_ui(app), CUI_KEY_SPACE, event.type == NSEventTypeKeyDown, CUI_MOD_CTRL);
            return nil;
        }
        if (event.window == (__bridge NSWindow *)cui_app_native_window(app) &&
            (mods & NSEventModifierFlagCommand) && !(mods & (NSEventModifierFlagOption | NSEventModifierFlagControl))) {
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

void ced_macos_remove_shortcuts(void *handle) {
    if (handle) [NSEvent removeMonitor:(__bridge_transfer id)handle];
}
