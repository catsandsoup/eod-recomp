#ifdef __APPLE__
#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#include "microphone_permission.h"
#include <memory>
namespace microphone_permission {
State state(){
    switch([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]){
        case AVAuthorizationStatusAuthorized:return State::Granted;
        case AVAuthorizationStatusDenied:return State::Denied;
        case AVAuthorizationStatusRestricted:return State::Restricted;
        case AVAuthorizationStatusNotDetermined:return State::NotRequested;
    }
    return State::Unavailable;
}
void request(std::function<void(State)>completed){
    auto callback=std::make_shared<std::function<void(State)>>(std::move(completed));
    if(state()!=State::NotRequested){dispatch_async(dispatch_get_main_queue(),^{if(*callback)(*callback)(state());});return;}
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL){
        dispatch_async(dispatch_get_main_queue(),^{if(*callback)(*callback)(state());});
    }];
}
void open_settings(){
    [[NSWorkspace sharedWorkspace]openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"]];
}
}
#endif
