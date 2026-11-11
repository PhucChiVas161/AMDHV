// Virtualization.framework diagnostic; --start tests disposable EFI lifecycle without a disk.
#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
@interface ProbeDelegate : NSObject <VZVirtualMachineDelegate>
@property(nonatomic, strong) NSError *failure;
@property(nonatomic) BOOL guestStopped;
@end
@implementation ProbeDelegate
- (void)guestDidStopVirtualMachine:(VZVirtualMachine *)vm {
    (void)vm;
    self.guestStopped = YES;
    fprintf(stderr, "Guest requested VM stop (delegate callback)\n");
}
- (void)virtualMachine:(VZVirtualMachine *)vm didStopWithError:(NSError *)error {
    (void)vm;
    self.failure = error;
    fprintf(stderr, "VM stopped with error: %s (%ld) %s\n", error.domain.UTF8String,
        (long)error.code, error.localizedDescription.UTF8String);
}
@end
static BOOL awaitCompletion(BOOL (^done)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:15];
    while (!done() && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    return done();
}
int main(int argc, const char **argv) {
    BOOL start = argc >= 2 && strcmp(argv[1], "--start") == 0;
    BOOL linux = argc >= 2 && strcmp(argv[1], "--linux") == 0;
    unsigned seconds = 15;
    BOOL argsOK = argc == 1 || (start && argc == 2);
    if (start && argc == 4 && strcmp(argv[2], "--seconds") == 0) {
        char *end = NULL;
        long value = strtol(argv[3], &end, 10);
        argsOK = end != argv[3] && *end == 0 && value >= 1 && value <= 300;
        if (argsOK) seconds = (unsigned)value;
    }
    if (linux && (argc == 4 || (argc == 6 && strcmp(argv[4], "--seconds") == 0))) {
        argsOK = YES; start = YES;
        if (argc == 6) {
            char *end = NULL;long value = strtol(argv[5],&end,10);
            argsOK = end != argv[5] && *end == 0 && value >= 1 && value <= 300;
            if (argsOK) seconds = (unsigned)value;
        }
    }
    if (!argsOK) { fprintf(stderr, "Usage: vzprobe [--start [--seconds 1..300]] | --linux KERNEL INITRD [--seconds 1..300]\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        BOOL supported = VZVirtualMachine.isSupported;
        VZVirtualMachineConfiguration *config = [VZVirtualMachineConfiguration new];
        config.CPUCount = 1;
        config.memorySize = 512ull * 1024 * 1024;
        NSError *error = nil;
        NSFileManager *fm = NSFileManager.defaultManager;
        NSURL *directory = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
            [@"fixvm-vzprobe-" stringByAppendingString:NSUUID.UUID.UUIDString]] isDirectory:YES];
        if (![fm createDirectoryAtURL:directory withIntermediateDirectories:NO attributes:nil error:&error]) {
            fprintf(stderr, "temporary directory: %s\n", error.localizedDescription.UTF8String); return 2;
        }
        VZEFIBootLoader *loader = [[VZEFIBootLoader alloc] init];
        loader.variableStore = [[VZEFIVariableStore alloc] initCreatingVariableStoreAtURL:
            [directory URLByAppendingPathComponent:@"efi-vars"] options:0 error:&error];
        config.bootLoader = loader;
        if (linux) {
            VZLinuxBootLoader *linuxLoader = [[VZLinuxBootLoader alloc] initWithKernelURL:
                [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]]];
            linuxLoader.initialRamdiskURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[3]]];
            linuxLoader.commandLine = @"console=hvc0 earlyprintk=serial nokaslr loglevel=7 panic=0";
            const char *extra = getenv("FIXVM_LINUX_EXTRA_CMDLINE");
            if (extra && *extra) {
                linuxLoader.commandLine = [linuxLoader.commandLine stringByAppendingFormat:@" %s", extra];
                printf("DIAGNOSTIC Linux extra command line: %s\n", extra);
            }
            config.bootLoader = linuxLoader;
        }
        NSURL *consoleURL = [directory URLByAppendingPathComponent:@"console.log"];
        VZVirtioConsoleDeviceSerialPortConfiguration *console = [[VZVirtioConsoleDeviceSerialPortConfiguration alloc] init];
        console.attachment = [[VZFileSerialPortAttachment alloc] initWithURL:consoleURL append:NO error:&error];
        config.serialPorts = @[console];
        BOOL valid = loader.variableStore && console.attachment && [config validateWithError:&error];
        NSDictionary *report = @{
            @"supported": @(supported),
            @"configurationValid": @(valid),
            @"cpuMinimum": @(VZVirtualMachineConfiguration.minimumAllowedCPUCount),
            @"cpuMaximum": @(VZVirtualMachineConfiguration.maximumAllowedCPUCount),
            @"memoryMinimum": @(VZVirtualMachineConfiguration.minimumAllowedMemorySize),
            @"memoryMaximum": @(VZVirtualMachineConfiguration.maximumAllowedMemorySize),
            @"errorDomain": error.domain ?: @"",
            @"errorCode": @(error.code),
            @"errorDescription": error.localizedDescription ?: @"",
            @"scope": linux ? @"Disposable Linux direct boot; no disk, network, or container proof" :
                start ? @"Disposable EFI start/stop requested; no disk or Linux boot" : @"EFI configuration validation only; no VM start or Linux boot"
        };
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:NULL];
        if (json) { fwrite(json.bytes, 1, json.length, stdout); putchar('\n'); }
        BOOL lifecycleOK = !start;
        if (start && supported && valid) {
            VZVirtualMachine *vm = [[VZVirtualMachine alloc] initWithConfiguration:config];
            ProbeDelegate *delegate = [ProbeDelegate new];
            vm.delegate = delegate;
            __block BOOL done = NO;
            __block NSError *startError = nil;
            [vm startWithCompletionHandler:^(NSError *failure) { startError = failure; done = YES; }];
            BOOL finished = awaitCompletion(^BOOL { return done; });
            printf("start completed=%d state=%ld error=%s (%ld) %s\n", finished, (long)vm.state,
                startError.domain.UTF8String ?: "", (long)startError.code,
                startError.localizedDescription.UTF8String ?: "");
            lifecycleOK = finished && !startError && vm.state == VZVirtualMachineStateRunning;
            if (lifecycleOK) {
                NSDate *began = [NSDate date];
                NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
                while (deadline.timeIntervalSinceNow > 0 && !delegate.failure &&
                       vm.state == VZVirtualMachineStateRunning)
                    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
                lifecycleOK = !delegate.failure && vm.state == VZVirtualMachineStateRunning;
                printf("observation limit=%us elapsed=%.3fs completed=%d state=%ld guestStop=%d\n",
                    seconds, -began.timeIntervalSinceNow, lifecycleOK, (long)vm.state, delegate.guestStopped);
            }
            if (vm.canStop) {
                done = NO;
                __block NSError *stopError = nil;
                [vm stopWithCompletionHandler:^(NSError *failure) { stopError = failure; done = YES; }];
                finished = awaitCompletion(^BOOL { return done; });
                printf("stop completed=%d state=%ld error=%s\n", finished, (long)vm.state,
                    stopError.localizedDescription.UTF8String ?: "");
                lifecycleOK &= finished && !stopError && vm.state == VZVirtualMachineStateStopped;
            }
            printf("%s: %s lifecycle with %u-second observation; no boot proof\n", lifecycleOK ? "PASS" : "FAIL", linux?"Linux direct":"EFI", seconds);
        }
        NSError *cleanupError = nil;
        NSData *consoleData = [NSData dataWithContentsOfURL:consoleURL];
        printf("console bytes=%lu\n", (unsigned long)consoleData.length);
        if (consoleData.length) { fwrite(consoleData.bytes,1,consoleData.length,stdout);putchar('\n'); }
        if (linux && consoleData.length) {
            NSString *text = [[NSString alloc] initWithData:consoleData encoding:NSUTF8StringEncoding];
            BOOL guestFault = text && ([text containsString:@"Oops:"] || [text containsString:@"Kernel panic"] ||
                [text containsString:@"BUG: unable to handle"]);
            if (guestFault) {
                fprintf(stderr,"FAIL: Linux console reports a guest kernel fault despite the VM lifecycle state\n");
                lifecycleOK = NO;
            }
        }
        BOOL cleaned = [fm removeItemAtURL:directory error:&cleanupError];
        if (!cleaned) fprintf(stderr, "temporary store cleanup: %s\n", cleanupError.localizedDescription.UTF8String);
        if (!json || !cleaned) return 2;
        return supported && valid && lifecycleOK ? 0 : 1;
    }
}
