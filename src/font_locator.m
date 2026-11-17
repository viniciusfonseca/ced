#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include "editor_font.h"
#include <stdio.h>
#include <string.h>

bool ced_font_find(const char *family, char *path, char *error) {
    @autoreleasepool {
        NSString *name=[NSString stringWithUTF8String:family];
        NSFont *font=name ? [NSFont fontWithName:name size:14] : nil;
        if (!font) {
            for (NSArray *member in [[NSFontManager sharedFontManager] availableMembersOfFontFamily:name]) {
                NSFontTraitMask traits=[member[3] unsignedIntegerValue];
                if (!(traits & (NSBoldFontMask|NSItalicFontMask))) { font=[NSFont fontWithName:member[0] size:14]; break; }
            }
        }
        if (!font) { snprintf(error,CED_ERROR,"Installed font not found: %.255s",family); return false; }
        CTFontRef face=CTFontCreateWithName((__bridge CFStringRef)font.fontName,14,NULL);
        if (!face) { snprintf(error,CED_ERROR,"Could not resolve this font."); return false; }
        NSURL *url=CFBridgingRelease(CTFontCopyAttribute(face,kCTFontURLAttribute)); CFRelease(face);
        const char *file=url.path.fileSystemRepresentation;
        if (!file || strlen(file)>=CED_PATH) { snprintf(error,CED_ERROR,"Could not locate the installed font file."); return false; }
        snprintf(path,CED_PATH,"%s",file); return true;
    }
}
