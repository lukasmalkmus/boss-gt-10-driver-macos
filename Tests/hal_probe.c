// Reads the plug-in's diagnostic property through the HAL, because the plug-in
// host process has no usable log. Run it while an app records from the GT-10.

#include <CoreAudio/AudioHardware.h>
#include <stdio.h>
#include <string.h>

static AudioObjectID FindDevice(void) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef uid              = CFSTR("GT10Audio:Device");
    AudioObjectID device         = kAudioObjectUnknown;
    UInt32 size                  = sizeof device;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &device);
    return device;
}

int main(void) {
    const AudioObjectID device = FindDevice();
    if (device == kAudioObjectUnknown) {
        printf("device GT10Audio:Device not found\n");
        return 1;
    }
    AudioObjectPropertyAddress a = {'GTdg', kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef text             = NULL;
    UInt32 size                  = sizeof text;
    const OSStatus err           = AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &text);
    if (err != noErr || text == NULL) {
        printf("diagnostics unavailable: err %d\n", (int)err);
        return 1;
    }
    char buffer[512];
    CFStringGetCString(text, buffer, sizeof buffer, kCFStringEncodingUTF8);
    CFRelease(text);
    printf("%s\n", buffer);
    return 0;
}
