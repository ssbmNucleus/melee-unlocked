// Native dialogs for a launch from Finder.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>
#include <filesystem>
#include <string>

namespace mac_host {
std::filesystem::path choose_disc(const std::string& message) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    NSOpenPanel* picker = [NSOpenPanel openPanel];
    picker.title = @"Choose your Melee disc";
    picker.message = [NSString stringWithUTF8String:message.c_str()];
    picker.allowedFileTypes = @[@"iso", @"gcm"];
    picker.canChooseDirectories = NO;
    picker.allowsMultipleSelection = NO;
    if ([picker runModal] != NSModalResponseOK) return {};
    return std::filesystem::path(picker.URL.fileSystemRepresentation);
  }
}
void show_launch_error(const std::string& message) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    alert.messageText = @"Nucleus Unlocked could not start";
    alert.informativeText = [NSString stringWithUTF8String:message.c_str()];
    [alert runModal];
  }
}
}
