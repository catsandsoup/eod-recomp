#include "plume_render_interface_types.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

namespace editor_plume {

plume::RenderWindow attach_surface(void *native_view) {
    NSView *view = (__bridge NSView *)native_view;
    NSWindow *window = [view window];
    if (window == nil) return {};
    view.wantsLayer = YES;
    CAMetalLayer *layer = [[CAMetalLayer alloc] init];
    [view.layer addSublayer:layer];
    return {(__bridge void *)window, (__bridge void *)layer};
}

void resize_surface(plume::RenderWindow window, void *native_view, uint32_t width, uint32_t height) {
    NSView *view = (__bridge NSView *)native_view;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)window.view;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.frame = view.bounds;
    layer.contentsScale = view.bounds.size.width > 0 ? width / view.bounds.size.width : 1;
    layer.drawableSize = CGSizeMake(width, height);
    [CATransaction commit];
}

void detach_surface(plume::RenderWindow window) {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)window.view;
    [layer removeFromSuperlayer];
    [layer release];
}

} // namespace editor_plume
