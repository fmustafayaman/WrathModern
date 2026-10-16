#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Os/Mac/MacInput.h"
#include <cstdint>
#include <vector>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

// The application object, the game window and its view.
//
// The client drives its own loop, so NSApplication is set up but never run; OsMacPumpEvents hands it
// the queued events. Window and view events go to the Os input layer, which turns them into the
// client's OS_INPUT events the way the Win32 window procedure does.

static bool s_quitRequested;
static CGSize s_fixedDrawableSize;

// Keeps the layer's drawable size in step with the view unless a fixed size is set.
static void UpdateDrawableSize(NSView* view) {
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(view.layer);

    if (![layer isKindOfClass:[CAMetalLayer class]]) {
        return;
    }

    CGFloat scale = view.window ? view.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
    layer.contentsScale = scale;

    if (s_fixedDrawableSize.width > 0 && s_fixedDrawableSize.height > 0) {
        layer.drawableSize = s_fixedDrawableSize;
    } else {
        NSRect backing = [view convertRectToBacking:view.bounds];
        layer.drawableSize = CGSizeMake(MAX(1.0, backing.size.width), MAX(1.0, backing.size.height));
    }
}

@interface WMApplicationDelegate : NSObject <NSApplicationDelegate>
@end

@implementation WMApplicationDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    // Quit from the menu, the Dock or Cmd+Q asks the client to close, like Alt+F4 on Windows; the
    // client shuts down through its own path.
    OsMacInputClose();

    return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return NO;
}

@end

@interface WMGameWindow : NSWindow
@end

@implementation WMGameWindow

// Borderless fullscreen windows do not take key status by default.
- (BOOL)canBecomeKeyWindow {
    return YES;
}

- (BOOL)canBecomeMainWindow {
    return YES;
}

@end

@interface WMGameView : NSView <NSTextInputClient>
@end

@implementation WMGameView {
    NSTrackingArea* m_trackingArea;
    NSMutableAttributedString* m_markedText;
    NSRange m_markedSelection;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];

    if (self) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        m_markedText = [[NSMutableAttributedString alloc] init];
        m_markedSelection = NSMakeRange(0, 0);
    }

    return self;
}

- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.contentsGravity = kCAGravityResize;
    layer.needsDisplayOnBoundsChange = NO;
    layer.presentsWithTransaction = NO;

    return layer;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

- (BOOL)isOpaque {
    return YES;
}

- (void)updateTrackingAreas {
    if (m_trackingArea) {
        [self removeTrackingArea:m_trackingArea];
    }

    m_trackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:m_trackingArea];
    [super updateTrackingAreas];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    UpdateDrawableSize(self);
    OsMacInputResized();
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    UpdateDrawableSize(self);
    OsMacInputResized();
}

// Keyboard

- (void)keyDown:(NSEvent*)event {
    OsMacInputKeyDown(event);

    // Text goes through the input context so dead keys and input methods compose as they should.
    if (OsMacIMEEnabled() || !(event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) {
        [self interpretKeyEvents:@[ event ]];
    }
}

- (void)keyUp:(NSEvent*)event {
    OsMacInputKeyUp(event);
}

- (void)flagsChanged:(NSEvent*)event {
    OsMacInputFlagsChanged(event);
}

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    // Command acts as Control in the client. Let the menu take its own shortcuts (Cmd+Q, Cmd+H, Cmd+M);
    // every other Command combination is a key press for the client.
    if ([[NSApp mainMenu] performKeyEquivalent:event]) {
        return YES;
    }

    if (event.type == NSEventTypeKeyDown && (event.modifierFlags & NSEventModifierFlagCommand)) {
        OsMacInputKeyDown(event);
        return YES;
    }

    return NO;
}

- (void)doCommandBySelector:(SEL)selector {
    // Editing commands (arrows, Return, Escape) reach the client as key events already.
}

// Mouse

- (void)mouseDown:(NSEvent*)event {
    OsMacInputMouseButton(self, event, true);
}

- (void)mouseUp:(NSEvent*)event {
    OsMacInputMouseButton(self, event, false);
}

- (void)rightMouseDown:(NSEvent*)event {
    OsMacInputMouseButton(self, event, true);
}

- (void)rightMouseUp:(NSEvent*)event {
    OsMacInputMouseButton(self, event, false);
}

- (void)otherMouseDown:(NSEvent*)event {
    OsMacInputMouseButton(self, event, true);
}

- (void)otherMouseUp:(NSEvent*)event {
    OsMacInputMouseButton(self, event, false);
}

- (void)mouseMoved:(NSEvent*)event {
    OsMacInputMouseMoved(self, event);
}

- (void)mouseDragged:(NSEvent*)event {
    OsMacInputMouseMoved(self, event);
}

- (void)rightMouseDragged:(NSEvent*)event {
    OsMacInputMouseMoved(self, event);
}

- (void)otherMouseDragged:(NSEvent*)event {
    OsMacInputMouseMoved(self, event);
}

- (void)scrollWheel:(NSEvent*)event {
    OsMacInputScrollWheel(self, event);
}

// NSTextInputClient

static std::vector<uint16_t> UTF16(NSString* string) {
    std::vector<uint16_t> text(string.length);

    if (!text.empty()) {
        [string getCharacters:reinterpret_cast<unichar*>(text.data()) range:NSMakeRange(0, string.length)];
    }

    return text;
}

static NSString* PlainString(id string) {
    return [string isKindOfClass:[NSAttributedString class]] ? [static_cast<NSAttributedString*>(string) string] : static_cast<NSString*>(string);
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    NSString* text = PlainString(string);
    bool composing = m_markedText.length > 0;

    [m_markedText deleteCharactersInRange:NSMakeRange(0, m_markedText.length)];
    m_markedSelection = NSMakeRange(0, 0);

    // With the client's IME enabled, composed text is a composition result. Otherwise composition is
    // only dead-key input (^ then e), whose result is plain characters, as on Windows.
    if (composing && OsMacIMEEnabled()) {
        std::vector<uint16_t> utf16 = UTF16(text);
        OsMacIMECommit(utf16.data(), static_cast<uint32_t>(utf16.size()));
        OsMacIMEEndComposition();
        return;
    }

    for (NSUInteger i = 0; i < text.length; i++) {
        unichar c = [text characterAtIndex:i];

        // The client takes characters of the Basic Multilingual Plane, as WM_CHAR delivers them.
        if (CFStringIsSurrogateHighCharacter(c) || CFStringIsSurrogateLowCharacter(c)) {
            continue;
        }

        OsMacInputCharacter(c);
    }
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    NSString* text = PlainString(string);
    bool starting = m_markedText.length == 0 && text.length > 0;

    [m_markedText setAttributedString:[[NSAttributedString alloc] initWithString:text]];
    m_markedSelection = selectedRange;

    if (!OsMacIMEEnabled()) {
        return;
    }

    if (starting) {
        OsMacIMEBeginComposition();
    }

    std::vector<uint16_t> utf16 = UTF16(text);
    OsMacIMESetComposition(utf16.data(), static_cast<uint32_t>(utf16.size()), static_cast<uint32_t>(selectedRange.location), static_cast<uint32_t>(selectedRange.length));

    if (!text.length) {
        OsMacIMEEndComposition();
    }
}

- (void)unmarkText {
    if (m_markedText.length) {
        NSString* text = m_markedText.string;
        [m_markedText deleteCharactersInRange:NSMakeRange(0, m_markedText.length)];
        [self insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
    }
}

- (NSRange)selectedRange {
    return m_markedText.length ? m_markedSelection : NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange {
    return m_markedText.length ? NSMakeRange(0, m_markedText.length) : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText {
    return m_markedText.length > 0;
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    return nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    // The candidate window opens at the bottom left of the game view; the client draws its own
    // composition text.
    NSRect frame = [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];

    return NSMakeRect(frame.origin.x, frame.origin.y, 0, 0);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return NSNotFound;
}

@end

@interface WMWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation WMWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)window {
    OsMacInputClose();

    return NO;
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    OsMacInputFocus(true);
}

- (void)windowDidResignKey:(NSNotification*)notification {
    OsMacInputFocus(false);
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    OsMacInputFocus(false);
}

- (void)windowDidResize:(NSNotification*)notification {
    OsMacInputResized();
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    OsMacInputResized();
}

@end

static WMApplicationDelegate* s_applicationDelegate;
static WMWindowDelegate* s_windowDelegate;
static WMGameWindow* s_window;

static void BuildMainMenu() {
    NSString* name = [[NSProcessInfo processInfo] processName];
    NSMenu* menuBar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appItem];

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:name];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:name] action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];

    NSMenuItem* hideOthers = [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;

    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    [menuBar addItem:windowItem];

    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    windowItem.submenu = windowMenu;

    NSApp.mainMenu = menuBar;
    NSApp.windowsMenu = windowMenu;
}

void OsMacInitApplication() {
    if (s_applicationDelegate) {
        return;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        s_applicationDelegate = [[WMApplicationDelegate alloc] init];
        NSApp.delegate = s_applicationDelegate;

        BuildMainMenu();
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

static void ApplyWindowMode(WMGameWindow* window, const OsMacWindowParams& params) {
    NSScreen* screen = window.screen ? window.screen : [NSScreen mainScreen];

    if (params.m_fullscreen) {
        // Fullscreen takes the whole display without a mode change; the graphics device renders at the
        // requested resolution and the compositor scales it to the display.
        window.styleMask = NSWindowStyleMaskBorderless;
        window.level = NSMainMenuWindowLevel + 1;
        window.hasShadow = NO;
        [window setFrame:screen.frame display:YES];
        NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
        return;
    }

    NSApp.presentationOptions = NSApplicationPresentationDefault;
    window.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    window.level = NSNormalWindowLevel;
    window.hasShadow = YES;

    if (params.m_maximize) {
        [window setFrame:screen.visibleFrame display:YES];
        return;
    }

    // The requested size is in pixels; windows are laid out in points.
    CGFloat scale = screen.backingScaleFactor;
    NSSize content = NSMakeSize(params.m_width / scale, params.m_height / scale);
    NSRect visible = screen.visibleFrame;
    NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, content.width, content.height)];

    frame.size.width = MIN(frame.size.width, visible.size.width);
    frame.size.height = MIN(frame.size.height, visible.size.height);
    frame.origin.x = visible.origin.x + (visible.size.width - frame.size.width) / 2;
    frame.origin.y = visible.origin.y + (visible.size.height - frame.size.height) / 2;

    [window setFrame:frame display:YES];
}

void* OsMacCreateGameWindow(const OsMacWindowParams& params, const char* title) {
    OsMacInitApplication();

    @autoreleasepool {
        if (!s_window) {
            s_window = [[WMGameWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1024, 768) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
            s_window.releasedWhenClosed = NO;
            s_window.acceptsMouseMovedEvents = YES;
            s_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
            s_window.backgroundColor = [NSColor blackColor];

            s_windowDelegate = [[WMWindowDelegate alloc] init];
            s_window.delegate = s_windowDelegate;

            WMGameView* view = [[WMGameView alloc] initWithFrame:NSMakeRect(0, 0, 1024, 768)];
            s_window.contentView = view;
        }

        if (title) {
            s_window.title = [NSString stringWithUTF8String:title];
        }

        ApplyWindowMode(s_window, params);

        [s_window makeKeyAndOrderFront:nil];
        [s_window makeFirstResponder:s_window.contentView];

        UpdateDrawableSize(s_window.contentView);
        OsMacInputResized();

        return (__bridge void*)s_window;
    }
}

void OsMacDestroyGameWindow(void* window) {
    if (!window || (__bridge WMGameWindow*)window != s_window) {
        return;
    }

    @autoreleasepool {
        NSApp.presentationOptions = NSApplicationPresentationDefault;
        [s_window orderOut:nil];
        s_window.delegate = nil;
        s_window = nil;
        s_windowDelegate = nil;
    }
}

void* OsMacGetMetalLayer(void* window) {
    NSWindow* nsWindow = (__bridge NSWindow*)window;

    return nsWindow ? (__bridge void*)nsWindow.contentView.layer : nullptr;
}

void OsMacGetClientSize(void* window, int32_t* width, int32_t* height) {
    NSWindow* nsWindow = (__bridge NSWindow*)window;
    CAMetalLayer* layer = nsWindow ? static_cast<CAMetalLayer*>(nsWindow.contentView.layer) : nil;

    CGSize size = layer ? layer.drawableSize : CGSizeZero;

    if (size.width <= 0 || size.height <= 0) {
        NSRect bounds = nsWindow ? [nsWindow.contentView convertRectToBacking:nsWindow.contentView.bounds] : NSZeroRect;
        size = bounds.size;
    }

    *width = static_cast<int32_t>(size.width);
    *height = static_cast<int32_t>(size.height);
}

void OsMacSetDrawableSize(void* window, int32_t width, int32_t height) {
    NSWindow* nsWindow = (__bridge NSWindow*)window;
    s_fixedDrawableSize = width > 0 && height > 0 ? CGSizeMake(width, height) : CGSizeZero;

    if (nsWindow) {
        UpdateDrawableSize(nsWindow.contentView);
        OsMacInputResized();
    }
}

bool OsMacPumpEvents() {
    @autoreleasepool {
        while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]) {
            // NSApplication drops key-up events while Command is held; Command acts as Control in the
            // client, so deliver those to the window directly.
            if (event.type == NSEventTypeKeyUp && (event.modifierFlags & NSEventModifierFlagCommand) && event.window) {
                [event.window sendEvent:event];
                continue;
            }

            [NSApp sendEvent:event];
        }
    }

    return !s_quitRequested;
}
