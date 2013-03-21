/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


#include <IOKit/IOHibernatePrivate.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <mach/mach.h>
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <unistd.h>
#include <asl.h>
#include "PrivateLib.h"
#include "BatteryTimeRemaining.h"

#ifndef kIOHIDIdleTimeKy
#define kIOHIDIdleTimeKey                   "HIDIdleTime"
#endif

#ifndef kIOPMMaintenanceScheduleImmediate
#define kIOPMMaintenanceScheduleImmediate   "MaintenanceImmediate"
#endif

#define kIOPMRootDomainWakeReasonKey        "Wake Reason"
#define kIOPMRootDomainWakeTypeKey          "Wake Type"


enum
{
    PowerManagerScheduledShutdown = 1,
    PowerManagerScheduledSleep,
    PowerManagerScheduledRestart
};

/* If the battery doesn't specify an alternative time, we wait 16 seconds
   of ignoring the battery's (or our own) time remaining estimate. 
*/   
enum
{
    kInvalidWakeSecsDefault = 16
};

#define kPowerManagerActionNotificationName "com.apple.powermanager.action"
#define kPowerManagerActionKey "action"
#define kPowerManagerValueKey "value"

// Tracks system battery state
CFMutableSetRef             _publishedBatteryKeysSet = NULL;
// Track real batteries
static CFMutableSetRef     physicalBatteriesSet = NULL;
static int                 physicalBatteriesCount = 0;
static IOPMBattery         **physicalBatteriesArray = NULL;

#ifndef __I_AM_PMSET__
// Track simulated debug batteries
extern int                  _showWhichBatteries;
static CFMutableSetRef     simulatedBatteriesSet = NULL;
static int                 simulatedBatteriesCount = 0;
static IOPMBattery         **simulatedBatteriesArray = NULL;
#endif


/******
 * Do not remove DUMMY macros
 *
 * The following DUMMY macros aren't used in the source code, they're
 * a placeholder for the 'genstrings' tool to read the strings we're using.
 * 'genstrings' will encode these strings in a Localizable.strings file.
 * Note: if you add to or modify these strings, you must re-run genstrings and 
 * replace this project's Localizable.strings file with the output.
 ******/

#define DUMMY_UPS_HEADER(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("WARNING!"), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("Warning!"), \
            NULL);
            
#define DUMMY_UPS_BODY(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("YOUR COMPUTER IS NOW RUNNING ON UPS BACKUP BATTERY. SAVE YOUR DOCUMENTS AND SHUTDOWN SOON."), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("Your computer is now running on UPS backup battery power. Save your documents and shut down soon."), \
            NULL);


#define DUMMY_ASSERTION_STRING_TTY(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("A remote user is connected. That prevents system sleep."), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("A remote user is connected. That prevents system sleep."), \
            NULL);

#define DUMMY_CAFFEINATE_REASON_STRING(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("THE CAFFEINATE TOOL IS PREVENTING SLEEP."), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("The caffeinate tool is preventing sleep."), \
            NULL);

#ifndef __I_AM_PMSET__
// dynamicStoreNotifyCallBack is defined in pmconfigd.c
// is not defined in pmset! so we don't compile this code in pmset.

extern SCDynamicStoreRef    gSCDynamicStore;

__private_extern__ SCDynamicStoreRef _getSharedPMDynamicStore(void)
{
    return gSCDynamicStore;
}
#endif

__private_extern__ CFRunLoopRef         _getPMRunLoop(void)
{
    static CFRunLoopRef     pmRLS = NULL;
    
    if (!pmRLS) {
        pmRLS = CFRunLoopGetCurrent();
    }
    
    return pmRLS;
}

__private_extern__ dispatch_queue_t     _getPMDispatchQueue(void)
{
    static dispatch_queue_t pmQ = NULL;
    
    if (!pmQ) {
        pmQ = dispatch_queue_create("Power Management configd queue", NULL);
    }
    
    return pmQ;
}


__private_extern__ io_registry_entry_t getRootDomain(void)
{
    static io_registry_entry_t gRoot = MACH_PORT_NULL;

    if (MACH_PORT_NULL == gRoot)
        gRoot = IORegistryEntryFromPath( kIOMasterPortDefault, 
                kIOPowerPlane ":/IOPowerConnection/IOPMrootDomain");

    return gRoot;
}

__private_extern__ IOReturn 
_setRootDomainProperty(
    CFStringRef                 key, 
    CFTypeRef                   val) 
{
    return IORegistryEntrySetCFProperty(getRootDomain(), key, val);
}

__private_extern__ CFTypeRef 
_copyRootDomainProperty(
    CFStringRef                 key)
{
    return IORegistryEntryCreateCFProperty(getRootDomain(), key, kCFAllocatorDefault, 0);
}


__private_extern__ bool 
_getUUIDString(
	char *buf, 
	int buflen)
{
	bool			ret = false;
 	CFStringRef		uuidString = NULL;

	uuidString = IOPMSleepWakeCopyUUID();

	if (uuidString) {
		if (!CFStringGetCString(uuidString, buf, buflen, 
								kCFStringEncodingUTF8)) 
		{
			goto exit;
		}

		ret = true;
	}
exit:
	if (uuidString) CFRelease(uuidString);
	return ret;
}

__private_extern__ bool
_getSleepReason(
	char *buf,
	int buflen)
{
	bool			ret = false;
 	io_service_t    iopm_rootdomain_ref = getRootDomain();
 	CFStringRef		sleepReasonString = NULL;

	sleepReasonString = IORegistryEntryCreateCFProperty(
							iopm_rootdomain_ref,
							CFSTR("Last Sleep Reason"),
							kCFAllocatorDefault, 0);

	if (sleepReasonString) {
		if (!CFStringGetCString(sleepReasonString, buf, buflen, 
								kCFStringEncodingUTF8)) 
		{
			goto exit;
		}

		ret = true;
	}
exit:
	if (sleepReasonString) CFRelease(sleepReasonString);
	return ret;
}

__private_extern__ bool
_getWakeReason(
	char *buf,
	int buflen)
{
	bool			ret = false;
 	io_service_t    iopm_rootdomain_ref = getRootDomain();
 	CFStringRef		wakeReasonString = NULL;
 	CFStringRef		wakeTypeString = NULL;

    // This property may not exist on all platforms.
	wakeReasonString = IORegistryEntryCreateCFProperty(
							iopm_rootdomain_ref,
              CFSTR(kIOPMRootDomainWakeReasonKey),
							kCFAllocatorDefault, 0);

	if (wakeReasonString) {
		if (CFStringGetCString(wakeReasonString, buf, buflen, 
								kCFStringEncodingUTF8) && buf[0]) 
		{
       ret = true;
		}
	}

  if ( ret ) 
    goto exit;

  // If there is no Wake Reason, try getting Wake Type.
  // That sheds some light on why system woke up.
  wakeTypeString = IORegistryEntryCreateCFProperty(
              iopm_rootdomain_ref,
              CFSTR(kIOPMRootDomainWakeTypeKey),
              kCFAllocatorDefault, 0);

  if (wakeTypeString) {
    if (CFStringGetCString(wakeTypeString, buf, buflen, 
                kCFStringEncodingUTF8) && buf[0]) 
    {
      ret = true;
    }
  }
	if (wakeTypeString) CFRelease(wakeTypeString);

exit:
	if (wakeReasonString) CFRelease(wakeReasonString);
	return ret;
}

__private_extern__ bool
_getHibernateState(
	uint32_t *hibernateState)
{
	bool			ret = false;
 	io_service_t    iopm_rootdomain_ref = getRootDomain();
 	CFDataRef		hstateData = NULL;
  uint32_t    *hstatePtr;

    // This property may not exist on all platforms.
	hstateData = IORegistryEntryCreateCFProperty(
							iopm_rootdomain_ref,
              CFSTR(kIOHibernateStateKey),
							kCFAllocatorDefault, 0);

	if ((hstateData) && (hstatePtr = (uint32_t *)CFDataGetBytePtr(hstateData))) {
    *hibernateState = *hstatePtr;

		ret = true;
	}

	if (hstateData) CFRelease(hstateData);
	return ret;
}

__private_extern__ 
const char *stringForLWCode(uint8_t code) 
{
    const char *string;
    switch (code) 
    {
        default:
            string = "OK";
    }
    return string;
}

__private_extern__
const char *stringForPMCode(uint8_t code) 
{
    const char *string = "";

    switch (code)
    {
        case kIOPMTracePointSystemUp:
            string = "On";
            break;
        case kIOPMTracePointSleepStarted:
            string = "SleepStarted";
            break;
        case kIOPMTracePointSleepApplications:
            string = "SleepApps";
            break;
        case kIOPMTracePointSleepPriorityClients:
            string = "SleepPriority";
            break;
        case kIOPMTracePointSleepWillChangeInterests:
            string = "SleepWillChangeInterests";
            break;
        case kIOPMTracePointSleepPowerPlaneDrivers:
            string = "SleepDrivers";
            break;
        case kIOPMTracePointSleepDidChangeInterests:
            string = "SleepDidChangeInterests";
            break;
        case kIOPMTracePointSleepCapabilityClients:
            string = "SleepCapabilityClients";
            break;
        case kIOPMTracePointSleepPlatformActions:
            string = "SleepPlatformActions";
            break;
        case kIOPMTracePointSleepCPUs:
            string = "SleepCPUs";
            break;
        case kIOPMTracePointSleepPlatformDriver:
            string = "SleepPlatformDriver";
            break;
        case kIOPMTracePointSystemSleep:
            string = "SleepPlatform";
            break;
        case kIOPMTracePointHibernate:
            string = "Hibernate";
            break;
        case kIOPMTracePointWakePlatformDriver:
            string = "WakePlatformDriver";
            break;
        case kIOPMTracePointWakePlatformActions:
            string = "WakePlatformActions";
            break;
        case kIOPMTracePointWakeCPUs:
            string = "WakeCPUs";
            break;
        case kIOPMTracePointWakeWillPowerOnClients:
            string = "WakeWillPowerOnClients";
            break;
        case kIOPMTracePointWakeWillChangeInterests:
            string = "WakeWillChangeInterests";
            break;
        case kIOPMTracePointWakeDidChangeInterests:
            string = "WakeDidChangeInterests";
            break;
        case kIOPMTracePointWakePowerPlaneDrivers:
            string = "WakeDrivers";
            break;
        case kIOPMTracePointWakeCapabilityClients:
            string = "WakeCapabilityClients";
            break;
        case kIOPMTracePointWakeApplications:
            string = "WakeApps";
            break;
        case kIOPMTracePointSystemLoginwindowPhase:
            string = "WakeLoginWindow";
            break;
        case kIOPMTracePointDarkWakeEntry:
            string = "DarkWakeEntry";
            break;
        case kIOPMTracePointDarkWakeExit:
            string = "DarkWakeExit";
            break;
    }
    return string;
}


static void sendNotification(int command)
{
#if !TARGET_OS_EMBEDDED
    CFMutableDictionaryRef   dict = NULL;
    int numberOfSeconds = 600;
    
    CFNumberRef secondsValue = CFNumberCreate( NULL, kCFNumberIntType, &numberOfSeconds );
    CFNumberRef commandValue = CFNumberCreate( NULL, kCFNumberIntType, &command );

    dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, 
                                    &kCFTypeDictionaryKeyCallBacks, 
                                    &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(dict, CFSTR(kPowerManagerActionKey), commandValue);
    CFDictionarySetValue(dict, CFSTR(kPowerManagerValueKey), secondsValue);

    CFNotificationCenterPostNotificationWithOptions ( 
					    CFNotificationCenterGetDistributedCenter(),
                                            CFSTR(kPowerManagerActionNotificationName), 
                                            NULL, dict, 
                                            (kCFNotificationPostToAllSessions | kCFNotificationDeliverImmediately));
    CFRelease(dict);
    CFRelease(secondsValue);
    CFRelease(commandValue);
#endif
}


__private_extern__ void _askNicelyThenShutdownSystem(void)
{
    sendNotification( PowerManagerScheduledShutdown );
}

__private_extern__ void _askNicelyThenSleepSystem(void)
{
    sendNotification( PowerManagerScheduledSleep );
}

__private_extern__ void _askNicelyThenRestartSystem(void)
{
    sendNotification( PowerManagerScheduledRestart );
}

__private_extern__ CFAbsoluteTime _CFAbsoluteTimeFromPMEventTimeStamp(uint64_t kernelPackedTime)
{
    uint32_t    cal_sec = (uint32_t)(kernelPackedTime >> 32);
    uint32_t    cal_micro = (uint32_t)(kernelPackedTime & 0xFFFFFFFF);
    CFAbsoluteTime timeKernelEpoch = (CFAbsoluteTime)(double)cal_sec + (double)cal_micro/1000.0;

    // Adjust from kernel 1970 epoch to CF 2001 epoch
    timeKernelEpoch -= kCFAbsoluteTimeIntervalSince1970;
    
    return timeKernelEpoch;
}



static void _unpackBatteryState(IOPMBattery *b, CFDictionaryRef prop)    
{
    CFBooleanRef    boo;
    CFNumberRef     n;
    
    if(!isA_CFDictionary(prop)) return;
    
    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSExternalConnectedKey));
    b->externalConnected = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSExternalChargeCapableKey));
    b->externalChargeCapable = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSBatteryInstalledKey));
    b->isPresent = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSIsChargingKey));
    b->isCharging = (kCFBooleanTrue == boo);

    b->failureDetected = (CFStringRef)CFDictionaryGetValue(prop, CFSTR(kIOPMPSErrorConditionKey));

    b->batterySerialNumber = (CFStringRef)CFDictionaryGetValue(prop, CFSTR("BatterySerialNumber"));

    b->chargeStatus = (CFStringRef)CFDictionaryGetValue(prop, CFSTR(kIOPMPSBatteryChargeStatusKey));

    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCurrentCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->currentCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSMaxCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->maxCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSDesignCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->designCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSTimeRemainingKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->hwAverageTR);
    }
    

    n = CFDictionaryGetValue(prop, CFSTR("InstantAmperage"));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->instantAmperage);
    }    
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSAmperageKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->avgAmperage);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSMaxErrKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->maxerr);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCycleCountKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->cycleCount);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSLocationKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->location);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSInvalidWakeSecondsKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->invalidWakeSecs);
    } else {
        b->invalidWakeSecs = kInvalidWakeSecsDefault;
    }
    n = CFDictionaryGetValue(prop, CFSTR("PermanentFailureStatus"));
    if (n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->pfStatus);
    } else {
        b->pfStatus = 0;
    }

    return;
}

/*
 * _batteries
 */
__private_extern__ IOPMBattery **_batteries(void)
{
#ifndef __I_AM_PMSET__
    if (kBatteryShowFake == _showWhichBatteries)
        return simulatedBatteriesArray;
    else 
#endif
        return physicalBatteriesArray;
}

/*
 * _batteryCount
 */
__private_extern__ int  _batteryCount(void)
{
#ifndef __I_AM_PMSET__
    if (kBatteryShowFake == _showWhichBatteries)
        return simulatedBatteriesCount;
    else 
#endif
        return physicalBatteriesCount;
}

__private_extern__ IOPMBattery *_newBatteryFound(io_registry_entry_t where)
{
    IOPMBattery *new_battery = NULL;
    static int new_battery_index = 0;
    // Populate new battery in array
    new_battery = calloc(1, sizeof(IOPMBattery));
    new_battery->me = where;
    new_battery->name = CFStringCreateWithFormat(
                            kCFAllocatorDefault, 
                            NULL, 
                            CFSTR("InternalBattery-%d"), 
                            new_battery_index);                            
    new_battery->dynamicStoreKey = SCDynamicStoreKeyCreate(
                            kCFAllocatorDefault, 
                            CFSTR("%@%@/InternalBattery-%d"),
                            kSCDynamicStoreDomainState, 
                            CFSTR(kIOPSDynamicStorePath), 
                            new_battery_index);

    if (new_battery->dynamicStoreKey) {
        if (!_publishedBatteryKeysSet) {
            _publishedBatteryKeysSet = CFSetCreateMutable(0, 1, &kCFTypeSetCallBacks);
        }
        if (_publishedBatteryKeysSet) {
            CFSetAddValue(_publishedBatteryKeysSet, new_battery->dynamicStoreKey);
        }
    }

    new_battery_index++;
    _batteryChanged(new_battery);
//    asl_log(NULL, NULL, ASL_LEVEL_ERR, "New battery found at index %d\n", new_battery_index-1);
    // Check whether new_battery is a software simulated battery,
    // or a real physical battery.
    if (new_battery->properties
        && CFDictionaryGetValue(new_battery->properties, CFSTR("AppleSoftwareSimulatedBattery")))
    {
//        asl_log(NULL, NULL, ASL_LEVEL_ERR, "New battery -> Simulated battery");
        /* Software simulated battery. Not a real battery. */
#ifndef __I_AM_PMSET__
        if (!simulatedBatteriesSet) {
            simulatedBatteriesSet = CFSetCreateMutable(0, 1, NULL);
        }
        CFSetAddValue(simulatedBatteriesSet, new_battery);
        simulatedBatteriesCount = CFSetGetCount(simulatedBatteriesSet);
        if (simulatedBatteriesArray) {
            free(simulatedBatteriesArray);
            simulatedBatteriesArray = NULL;
        }
        simulatedBatteriesArray = calloc(simulatedBatteriesCount, sizeof(void *));
        CFSetGetValues(simulatedBatteriesSet, (const void **)simulatedBatteriesArray);
#endif
    } else {
        /* Real, physical battery found */
        if (!physicalBatteriesSet) {
            physicalBatteriesSet = CFSetCreateMutable(0, 1, NULL);
        }
        CFSetAddValue(physicalBatteriesSet, new_battery);
        physicalBatteriesCount = CFSetGetCount(physicalBatteriesSet);
        if (physicalBatteriesArray) {
            free(physicalBatteriesArray);
            physicalBatteriesArray = NULL;
        }
        physicalBatteriesArray = calloc(physicalBatteriesCount, sizeof(void *));
        CFSetGetValues(physicalBatteriesSet, (const void **)physicalBatteriesArray);
    }
    
    return new_battery;
}


__private_extern__ void _batteryChanged(IOPMBattery *changed_battery)
{
    kern_return_t       kr;

    if(!changed_battery) { 
        // This is unexpected; we're not tracking this battery
        return;
    }
    
    // Free the last set of properties
    if(changed_battery->properties) { 
        CFRelease(changed_battery->properties);
        changed_battery->properties = NULL;
    }
    
    kr = IORegistryEntryCreateCFProperties(
                            changed_battery->me, 
                            &(changed_battery->properties),
                            kCFAllocatorDefault, 0);
    if(KERN_SUCCESS != kr) {
        changed_battery->properties = NULL;
        goto exit;
    }

    _unpackBatteryState(changed_battery, changed_battery->properties);    
exit:
    return;
}

__private_extern__ bool _batteryHas(IOPMBattery *b, CFStringRef property)
{
    if(!property || !b->properties) return false;
    
    // If the battery's descriptior dictionary has an entry at all for the
    // given 'property' it is supported, i.e. the battery 'has' it.
    return CFDictionaryGetValue(b->properties, property) ? true : false;
}


#if HAVE_CF_USER_NOTIFICATION

__private_extern__ CFUserNotificationRef _showUPSWarning(void)
{
    CFMutableDictionaryRef      alert_dict;
    SInt32                      error;
    CFUserNotificationRef       note_ref;
    CFBundleRef                 myBundle;
    CFStringRef                 header_unlocalized;
    CFStringRef                 message_unlocalized;
    CFURLRef                    bundle_url;

    myBundle = CFBundleGetBundleWithIdentifier(kPowerdBundleIdentifier);
    if (!myBundle)
        return NULL;
    
    alert_dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, 
                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if(!alert_dict) 
        return NULL;

    bundle_url = CFBundleCopyBundleURL(myBundle);
    CFDictionarySetValue(alert_dict, kCFUserNotificationLocalizationURLKey, bundle_url);
    CFRelease(bundle_url);

    header_unlocalized = CFSTR("WARNING!");
    message_unlocalized = CFSTR("YOUR COMPUTER IS NOW RUNNING ON UPS BACKUP BATTERY. SAVE YOUR DOCUMENTS AND SHUTDOWN SOON.");

    CFDictionaryAddValue(alert_dict, kCFUserNotificationAlertHeaderKey, header_unlocalized);
    CFDictionaryAddValue(alert_dict, kCFUserNotificationAlertMessageKey, message_unlocalized);
    
    note_ref = CFUserNotificationCreate(kCFAllocatorDefault, 0, 0, &error, alert_dict);
    CFRelease(alert_dict);

    asl_log(0, 0, ASL_LEVEL_ERR, "PowerManagement: UPS low power warning\n");
    
    return note_ref;
}

#endif


/***************************************************************************/
/***************************************************************************/
/***************************************************************************/
/*      AAAAAAAAAAAAAA      SSSSSSSSSSSSSS       LLLLLLLLLLLLLLLL          */
/***************************************************************************/
/***************************************************************************/
/***************************************************************************/

static bool powerString(char *powerBuf, int bufSize)
{
    IOPMBattery                                 **batteries;
    int                                         batteryCount = 0;
    uint32_t                                    capPercent = 0;
    int                                         i;
    batteryCount = _batteryCount();
    if (0 < batteryCount) {
        batteries = _batteries();
        for (i=0; i< batteryCount; i++) {
            if (batteries[i]->isPresent
                && (0 != batteries[i]->maxCap)) {
                capPercent += (batteries[i]->currentCap * 100) / batteries[i]->maxCap;        
            }
        }
        snprintf(powerBuf, bufSize, "%s %d", 
               batteries[0]->externalConnected ? "AC":"BATT", capPercent);
        return true;
    } else {
        return false;
    }
}

__private_extern__ void logASLMessageSleep(
    const char *sig, 
    const char *uuidStr, 
    CFAbsoluteTime date,
    const char *failureStr
)
{
    static int              sleepCyclesCount = 0;
    aslmsg                  startMsg;
    char                    uuidString[150];
    char                    sleepReasonBuf[50];
    char                    powerLevelBuf[50];
    char                    numbuf[15];
    bool                    success = true;
    char                    messageString[200];
    bool                    showBatteries = false;
    const char *            detailString = NULL;

    startMsg = asl_new(ASL_TYPE_MSG);

    asl_set(startMsg, kMsgTracerDomainKey, kMsgTracerDomainPMSleep);

    asl_set(startMsg, kMsgTracerSignatureKey, sig);
    
    if (!strncmp(sig, kMsgTracerSigSuccess, sizeof(kMsgTracerSigSuccess)))
    {
        success = true;
        if (_getSleepReason(sleepReasonBuf, sizeof(sleepReasonBuf)))
            detailString = sleepReasonBuf;
    } else {
        success = false;
        detailString = failureStr;
    }

    if (success)
    {
        // Value == Sleep Cycles Count
        // Note: unknown on the failure case, so we won't publish the sleep count
        // unless sig == success
        snprintf(numbuf, 10, "%d", sleepCyclesCount++);
        asl_set(startMsg, kMsgTracerValueKey, numbuf);
    }
    
    asl_set(startMsg, kMsgTracerResultKey, 
                    success ? kMsgTracerResultSuccess : kMsgTracerResultFailure);
    
    // UUID
    if (uuidStr) {
        asl_set(startMsg, kMsgTracerUUIDKey, uuidStr);  // Caller Provided
    } else if (_getUUIDString(uuidString, sizeof(uuidString))) {
        asl_set(startMsg, kMsgTracerUUIDKey, uuidString);
    }

    showBatteries = powerString(powerLevelBuf, sizeof(powerLevelBuf));

    snprintf(messageString, sizeof(messageString), "Sleep: %s - %s%s%s\n", sig, 
                                showBatteries ? powerLevelBuf: "AC",
                                detailString ? " - ":"",
                                detailString ? detailString:"");
    asl_set(startMsg, ASL_KEY_MSG, messageString);
    
    asl_set(startMsg, ASL_KEY_LEVEL, ASL_STRING_NOTICE);
    asl_set(startMsg, kPMASLMessageKey, kPMASLMessageLogValue);
    asl_send(NULL, startMsg);
    asl_free(startMsg);
}

/*****************************************************************************/

static void stringForShutdownCode(char *buf, int buflen, int shutdowncode)
{
    if (3 == shutdowncode)
    {
        snprintf(buf, buflen, "Power Button Shutdown");
    } else if (5 == shutdowncode)
    {
        snprintf(buf, buflen, "Normal Shutdown");
    } else {
        snprintf(buf, buflen, "Shutdown Cause=%d\n", shutdowncode);
    }
}

__private_extern__ void logASLMessageFilteredFailure(
    uint32_t pmFailureStage,
    const char *pmFailureString,
    const char *uuidStr, 
    int shutdowncode)
{
    aslmsg      no_problem_msg;
    char        shutdownbuf[40];
    char        messagebuf[128];

    stringForShutdownCode(shutdownbuf, sizeof(shutdownbuf), shutdowncode);
    
    snprintf(messagebuf, sizeof(messagebuf), "Sleep - Filtered Sleep Failure Report - %s - %s",
                shutdownbuf, pmFailureString ? pmFailureString : "Failure Phase Unknown");
    
    no_problem_msg = asl_new(ASL_TYPE_MSG);
    if (uuidStr)
        asl_set(no_problem_msg, kMsgTracerUUIDKey, uuidStr);
    asl_set(no_problem_msg, kMsgTracerDomainKey, kMsgTracerDomainFilteredFailure);
    asl_set(no_problem_msg, kMsgTracerSignatureKey, shutdownbuf);
    asl_set(no_problem_msg, kMsgTracerResultKey, kMsgTracerResultSuccess);
    asl_set(no_problem_msg, ASL_KEY_MSG, messagebuf);
    asl_set(no_problem_msg, ASL_KEY_LEVEL, ASL_STRING_NOTICE);
    
    asl_set(no_problem_msg, kPMASLMessageKey, kPMASLMessageLogValue);
    asl_send(NULL, no_problem_msg);
    asl_free(no_problem_msg);
}

/*****************************************************************************/

__private_extern__ void logASLMessageWake(
    const char *sig, 
    const char *uuidStr, 
    CFAbsoluteTime date, 
    const char *failureStr
)
{
    aslmsg                  startMsg;
    char                    uuidString[150];
    bool                    success = true;
    char                    powerLevelBuf[50];
    char                    messageString[200];
    char                    wakeReasonBuf[50];
    const char *            detailString = NULL;
    bool                    showBatteries = false;
    uint32_t                hstate = 0;

    startMsg = asl_new(ASL_TYPE_MSG);
    
    asl_set(startMsg, kMsgTracerDomainKey, kMsgTracerDomainPMWake);

    asl_set(startMsg, kMsgTracerSignatureKey, sig);    

    if (!strncmp(sig, kMsgTracerSigSuccess, sizeof(kMsgTracerSigSuccess)))
    {
        success = true;
        if (_getWakeReason(wakeReasonBuf, sizeof(wakeReasonBuf)))
            detailString = wakeReasonBuf;
    } else {
        success = false;
        detailString = failureStr;
    }

    if (_getUUIDString(uuidString, sizeof(uuidString))) {
        asl_set(startMsg, kMsgTracerUUIDKey, uuidString);    
    }
    
    showBatteries = powerString(powerLevelBuf, sizeof(powerLevelBuf));
    if (_getHibernateState(&hstate) && (hstate == kIOHibernateStateWakingFromHibernate) ) 
    {
       snprintf(messageString, sizeof(messageString), "Wake from Standby: %s - %s%s%s\n", sig,
                            showBatteries ? powerLevelBuf: "AC",
                            detailString ? " - ":"",
                            detailString ? detailString:"");
    }
    else
    {

        snprintf(messageString, sizeof(messageString), "Wake: %s - %s%s%s\n", sig,
                            showBatteries ? powerLevelBuf: "AC",
                            detailString ? " - ":"",
                            detailString ? detailString:"");
    }
    asl_set(startMsg, ASL_KEY_MSG, messageString);

    asl_set(startMsg, kMsgTracerResultKey, 
                    success ? kMsgTracerResultSuccess : kMsgTracerResultFailure);

    asl_set(startMsg, ASL_KEY_LEVEL, ASL_STRING_NOTICE);

    asl_set(startMsg, kPMASLMessageKey, kPMASLMessageLogValue);
    asl_send(NULL, startMsg);
    asl_free(startMsg);

    // Log hibernation stats if coming out of standby mode
    if (hstate == kIOHibernateStateWakingFromHibernate)
       logASLMessageHibernateStatistics( );
}

/*****************************************************************************/

//static int maintenanceWakesCount = 0;

__private_extern__ void logASLMessageSystemPowerState(bool inS3, int runState)
{
    aslmsg                  startMsg;
    char                    uuidString[150];
    bool                    success = true;
    char                    messageString[200];
    const char *            detailString = NULL;
    
    startMsg = asl_new(ASL_TYPE_MSG);
    
    asl_set(startMsg, kMsgTracerDomainKey, kMsgTraceRDomainPMSystemPowerState);

    if (_getUUIDString(uuidString, sizeof(uuidString))) {
        asl_set(startMsg, kMsgTracerUUIDKey, uuidString);    
    }
    
    if (kRStateNormal == runState)
    {
        detailString = " - On (S0)";
    } else if (kRStateDark == runState || kRStateMaintenance == runState)
    {
        detailString = " - Dark";
    }
    
    snprintf(messageString, sizeof(messageString), "SystemPowerState: %s%s\n", 
             inS3 ? "asleep" : "awake",
             (!inS3 && detailString) ? detailString : "");
    asl_set(startMsg, ASL_KEY_MSG, messageString);
    
    asl_set(startMsg, kMsgTracerResultKey, 
            success ? kMsgTracerResultSuccess : kMsgTracerResultFailure);
    
    asl_set(startMsg, ASL_KEY_LEVEL, ASL_STRING_NOTICE);
    
    asl_set(startMsg, kPMASLMessageKey, kPMASLMessageLogValue);
    asl_send(NULL, startMsg);
    asl_free(startMsg);
    return;
}

/*****************************************************************************/

__private_extern__ void logASLMessageHibernateStatistics(void)
{
    aslmsg                  statsMsg;
    CFDataRef               statsData = NULL;
    PMStatsStruct           *stats = NULL;
    uint64_t                writeHIBImageMS = 0;
    uint64_t                readHIBImageMS = 0;
    CFNumberRef             hibernateModeNum = NULL;
    int                     hibernateMode = 0;
    char                    valuestring[25];
    char                    uuidString[150];

    hibernateModeNum = (CFNumberRef)_copyRootDomainProperty(CFSTR(kIOHibernateModeKey));
    if (!hibernateModeNum)
        goto exit;
    CFNumberGetValue(hibernateModeNum, kCFNumberIntType, &hibernateMode);
    CFRelease(hibernateModeNum);

    statsData = (CFDataRef)_copyRootDomainProperty(CFSTR(kIOPMSleepStatisticsKey));
    if (!statsData || !(stats = (PMStatsStruct *)CFDataGetBytePtr(statsData)))
    {
        goto exit;
    } else {
        writeHIBImageMS = (stats->hibWrite.stop - stats->hibWrite.start)/1000000UL;
    
        readHIBImageMS =(stats->hibRead.stop - stats->hibRead.start)/1000000UL;
    
        CFRelease(statsData);
    }
    
    statsMsg = asl_new(ASL_TYPE_MSG);

    asl_set(statsMsg, kMsgTracerDomainKey, kMsgTracerDomainHibernateStatistics);

    asl_set(statsMsg, ASL_KEY_LEVEL, ASL_STRING_NOTICE);

    if (_getUUIDString(uuidString, sizeof(uuidString))) {
        asl_set(statsMsg, kMsgTracerUUIDKey, uuidString);    
    }

    snprintf(valuestring, sizeof(valuestring), "hibernatemode=%d", hibernateMode);
    asl_set(statsMsg, kMsgTracerSignatureKey, valuestring);

    if (0 != readHIBImageMS) {
        // We woke from hib image and lost contents of memory. We do not
        // have a valid timing reading for hib write image.
        asl_set(statsMsg, kMsgTracerValueKey, kMsgTracerValueUndefined);
    } else {
        snprintf(valuestring, sizeof(valuestring), "%qd", writeHIBImageMS);
        asl_set(statsMsg, kMsgTracerValueKey, valuestring);
    }
    
    // If readHibImageMS == zero, that means we woke from the contents of memory
    // and did not read the hibernate image.
    snprintf(valuestring, sizeof(valuestring), "%qd", readHIBImageMS);
    asl_set(statsMsg, kMsgTracerValue2Key, valuestring);

    asl_set(statsMsg, ASL_KEY_MSG, "Hibernate Statistics");
    asl_set(statsMsg, kPMASLMessageKey, kPMASLMessageLogValue);
    asl_send(NULL, statsMsg);
    asl_free(statsMsg);    
exit:
    return;
}

/*****************************************************************************/

__private_extern__ void logASLMessageApplicationResponse(
    CFStringRef     logSourceString,
    CFStringRef     appNameString,
    CFStringRef     responseTypeString,
    CFNumberRef     responseTime
)
{
    aslmsg                  appMessage;
    char                    appName[128];
    char                    *appNamePtr = NULL;
    int                     time = 0;
    char                    valuestring[25];
    char                    uuidString[128];
    char                    messageString[128];
    char                    logSource[128];
    char                    *logSourcePtr = NULL;
    char *                  useDomain = NULL;
    int                     j = 0;
    bool                    fromKernel = false;

    // String identifying the source of the log is required.
    if (!logSourceString)
        return;
    if (CFEqual(logSourceString, kAppResponseLogSourceKernel))
        fromKernel = true;

    if (responseTypeString && CFEqual(responseTypeString, CFSTR(kIOPMStatsResponseTimedOut))) {
        useDomain = kMsgTracerDomainAppResponseTimedOut;
    } else if (responseTypeString && CFEqual(responseTypeString, CFSTR(kIOPMStatsResponseCancel))) {
        useDomain = kMsgTracerDomainAppResponseCancel;
    } else if (responseTypeString && CFEqual(responseTypeString, CFSTR(kIOPMStatsResponseSlow))) {
        useDomain = kMsgTracerDomainAppResponseSlow;
    }

    if (!useDomain) 
        return;

    appMessage = asl_new(ASL_TYPE_MSG);

    asl_set(appMessage, kMsgTracerDomainKey, useDomain);

    // Message = Failing process name
    if (appNameString)
    {
        if (CFStringGetCString(appNameString, appName, sizeof(appName), kCFStringEncodingUTF8))
        {
            if (!fromKernel)
            {
                appNamePtr = &appName[0];
            }
            else
            {
                // appName is of the string format "pid %d, %s" with 
                // an integer pid and a, string process name.
                // Strip off everything preceding the process name 
                // before we use it to log the process name alone. 

                for(j=0; j<(strlen(appName)-2); j++)
                {
                    if ((',' == appName[j]) && (' ' == appName[j+1]))
                    {
                        appNamePtr = &appName[j+2];
                        break;
                    }
                }
            }
        }
    }
    if (!appNamePtr) {
        appNamePtr = "AppNameUnknown";
    }

    asl_set(appMessage, kMsgTracerSignatureKey, appNamePtr);
    
    // UUID
    if (_getUUIDString(uuidString, sizeof(uuidString))) {
        asl_set(appMessage, kMsgTracerUUIDKey, uuidString);    
    }

    // Value == Time
    if (responseTime) {
        if (CFNumberGetValue(responseTime, kCFNumberIntType, &time)) {
            snprintf(valuestring, sizeof(valuestring), "%d", time);
            asl_set(appMessage, kMsgTracerValueKey, valuestring);
        }
    }

    if (CFStringGetCString(logSourceString, logSource, sizeof(logSource), kCFStringEncodingUTF8))
        logSourcePtr = &logSource[0];
    else
        logSourcePtr = "SourceNameUnknown";

    if (time == 0) {
        snprintf(messageString, sizeof(messageString),
            "%s %s %s", logSourcePtr, appNamePtr, useDomain);
    } else {
        snprintf(messageString, sizeof(messageString),
            "%s %s %s %d ms\n", logSourcePtr, appNamePtr, useDomain, time);
    }           

    asl_set(appMessage, kMsgTracerResultKey, kMsgTracerResultNoop); 

    asl_set(appMessage, ASL_KEY_MSG, messageString);
    
    asl_set(appMessage, ASL_KEY_LEVEL, ASL_STRING_NOTICE);    

    // Post one MessageTracer message per errant app response
    asl_set(appMessage, kPMASLMessageKey, kPMASLMessageLogValue);
    asl_send(NULL, appMessage);
    asl_free(appMessage);
}

/*****************************************************************************/

/* logASLMessageKernelApplicationResponses
 *
 * Logs one ASL message for each errant application notification received. 
 *
 */
__private_extern__ void logASLMessageKernelApplicationResponses(void)
{
    CFArrayRef              appFailuresArray = NULL;
    CFDictionaryRef         *appFailures = NULL;
    CFStringRef             appNameString = NULL;
    CFNumberRef             timeNum = NULL;
    CFStringRef             responseTypeString = NULL;
    int                     appFailuresCount = 0;
    int                     i = 0;

    appFailuresArray = (CFArrayRef)_copyRootDomainProperty(CFSTR(kIOPMSleepStatisticsAppsKey));
    
    if (!appFailuresArray 
        || (0 == (appFailuresCount = CFArrayGetCount(appFailuresArray)))) 
    {
        goto exit;
    }

    appFailures = (CFDictionaryRef *)calloc(appFailuresCount, sizeof(CFDictionaryRef));

    CFArrayGetValues(appFailuresArray, CFRangeMake(0, appFailuresCount), 
                        (const void **)appFailures);

    for (i=0; i<appFailuresCount; i++) 
    {
        if (!isA_CFDictionary(appFailures[i])) {
            continue;
        }
        
        appNameString       = CFDictionaryGetValue(appFailures[i], CFSTR(kIOPMStatsNameKey));
        timeNum             = CFDictionaryGetValue(appFailures[i], CFSTR(kIOPMStatsTimeMSKey));
        responseTypeString  = CFDictionaryGetValue(appFailures[i], CFSTR(kIOPMStatsApplicationResponseTypeKey));

        logASLMessageApplicationResponse(
            kAppResponseLogSourceKernel,
            appNameString,
            responseTypeString,
            timeNum);
    }

exit:
    if (appFailuresArray)
        CFRelease(appFailuresArray);

    if (appFailures)
        free(appFailures);
}

/*****************************************************************************/
/*****************************************************************************/


/***************************************************************************/
/***************************************************************************/
/***************************************************************************/
/***************************************************************************/



/************************* One off hack for AppleSMC
 *************************
 ************************* Send AppleSMC a kCFPropertyTrue
 ************************* on time discontinuities.
 *************************/

static CFMachPortRef        calChangeReceivePort = NULL;

static void setSMCProperty(void)
{
    static io_registry_entry_t     _smc = MACH_PORT_NULL;

    if(MACH_PORT_NULL == _smc) {
        _smc = IOServiceGetMatchingService( MACH_PORT_NULL,
                        IOServiceMatching("AppleSMCFamily"));
    }
    
    if(!_smc) {
        return;
    }
    
    // And simply AppleSMC with kCFBooleanTrue to let them know time is changed.
    // We don't pass any information down.
    IORegistryEntrySetCFProperty( _smc, 
                        CFSTR("TheTimesAreAChangin"), 
                        kCFBooleanTrue);
}

static void handleMachCalendarMessage(CFMachPortRef port, void *msg, 
                                            CFIndex size, void *info)
{
    kern_return_t  result;
    mach_port_t    mport = CFMachPortGetPort(port); 
    mach_port_t	   host_port;
	
    // Re-register for notification
    host_port = mach_host_self();
    result = host_request_notification(host_port, HOST_NOTIFY_CALENDAR_CHANGE, mport);
    if (host_port) {
        mach_port_deallocate(mach_task_self(), host_port);
    }
    if (result != KERN_SUCCESS) {
        return;
    }

    setSMCProperty();
}


static void registerForCalendarChangedNotification(void)
{
    mach_port_t tport;
    mach_port_t host_port;
    kern_return_t result;
    CFRunLoopSourceRef rls;

    // allocate the mach port we'll be listening to
    result = mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE, &tport);
    if (result != KERN_SUCCESS) {
        return;
    }

    calChangeReceivePort = CFMachPortCreateWithPort(
            kCFAllocatorDefault,
            tport,
            (CFMachPortCallBack)handleMachCalendarMessage,
            NULL, /* context */
            false); /* shouldFreeInfo */
    if (calChangeReceivePort) {
        rls = CFMachPortCreateRunLoopSource(
                kCFAllocatorDefault, 
                calChangeReceivePort,
                0); /* index Order */
        if (rls) {
            CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
            CFRelease(rls);
        }
        CFRelease(calChangeReceivePort);
    }
    
	// register for notification
    host_port = mach_host_self();
    host_request_notification(host_port,HOST_NOTIFY_CALENDAR_CHANGE, tport);
    if (host_port) {
        mach_port_deallocate(mach_task_self(), host_port);
    }
}


__private_extern__ int
callerIsRoot(
    int uid,
    int gid
)
{
    return (0 == uid);
}

__private_extern__ int
callerIsAdmin(
    int uid,
    int gid
)
{
    int         ngroups = NGROUPS_MAX+1;
    int         group_list[NGROUPS_MAX+1];
    int         i;
    struct group    *adminGroup;
    struct passwd   *pw;
        
    
    pw = getpwuid(uid);
    if (!pw) 
        return false;
    
    getgrouplist(pw->pw_name, pw->pw_gid, group_list, &ngroups);

    adminGroup = getgrnam("admin");
    if (adminGroup != NULL) {
        gid_t    adminGid = adminGroup->gr_gid;
        for(i=0; i<ngroups; i++)
        {
            if (group_list[i] == adminGid) {
                return TRUE;    // if a member of group "admin"
            }
        }
    }
    return false;
}

__private_extern__ int
callerIsConsole(
    int uid,
    int gid)
{
#if TARGET_OS_EMBEDDED
	return false;
#else
    CFStringRef                 user_name = NULL;
    uid_t                       console_uid;
    gid_t                       console_gid;
    
    user_name = (CFStringRef)SCDynamicStoreCopyConsoleUser(NULL, 
                                    &console_uid, &console_gid);

    if(user_name) {
        CFRelease(user_name);
        return ((uid == console_uid) && (gid == console_gid));
    } else {
        // no data returned re: console user's uid or gid; return "false"
        return false;
    }
#endif /* !TARGET_OS_EMBEDDED */
}


void _oneOffHacksSetup(void) 
{
#if !TARGET_OS_EMBEDDED
    registerForCalendarChangedNotification();
#endif
}

static const CFTimeInterval kTimeNSPerSec = 1000000000.0;

CFTimeInterval _getHIDIdleTime(void)
{
    static io_registry_entry_t hidsys = IO_OBJECT_NULL;
    CFNumberRef     hidsys_idlenum = NULL;
    CFTimeInterval  ret_time = 0.0;
    uint64_t        idle_nanos = 0;
    
    if (IO_OBJECT_NULL == hidsys) {
        hidsys = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOHIDSystem"));
    }
    if (!hidsys)
        goto exit;
    
    hidsys_idlenum = IORegistryEntryCreateCFProperty(hidsys, CFSTR(kIOHIDIdleTimeKey), 0, 0);

    if (!isA_CFNumber(hidsys_idlenum))
        goto exit;
    
    if (CFNumberGetValue(hidsys_idlenum, kCFNumberSInt64Type, &idle_nanos))
    {
        ret_time = ((CFTimeInterval)idle_nanos)/kTimeNSPerSec;
    }

exit:
    if (hidsys_idlenum)
        CFRelease(hidsys_idlenum);
    return ret_time;
}

/************************************************************************/
/************************************************************************/
/************************************************************************/
/************************************************************************/
/************************************************************************/

// Code to read AppleSMC

/************************************************************************/
/************************************************************************/
/************************************************************************/
/************************************************************************/
/************************************************************************/
// Forwards
static IOReturn _smcWriteKey(
    uint32_t key,
    uint8_t *outBuf,
    uint8_t outBufMax);
static IOReturn _smcReadKey(
    uint32_t key,
    uint8_t *outBuf,
    uint8_t *outBufMax);

/************************************************************************/
__private_extern__ IOReturn _getSystemManagementKeyInt32(
    uint32_t key, 
    uint32_t *val)
{
#if !TARGET_OS_EMBEDDED    
    uint8_t readKeyLen = 4;
    return _smcReadKey(key, (void *)val, &readKeyLen);
#else
    return kIOReturnNotReadable;
#endif
}
/************************************************************************/
__private_extern__ IOReturn _getACAdapterInfo(
    uint64_t *val)
{
#if !TARGET_OS_EMBEDDED    
    uint8_t readKeyLen = 8;
    return _smcReadKey('ACID', (void *)val, &readKeyLen);
#else
    return kIOReturnNotReadable;
#endif
}
/************************************************************************/
__private_extern__ PowerSources _getPowerSource(void)
{
#if !TARGET_OS_EMBEDDED    
   IOPMBattery      **batteries;

   if (_batteryCount() && (batteries = _batteries()) 
            && (!batteries[0]->externalConnected) )
      return kBatteryPowered;
   else
      return kACPowered;
#else
    return kBatteryPowered;
#endif
}


/************************************************************************/
__private_extern__ IOReturn _smcWakeTimerPrimer(void)
{
#if !TARGET_OS_EMBEDDED    
    uint8_t  buf[2];
    
    buf[0] = 0; 
    buf[1] = 1;
    return _smcWriteKey('CLWK', buf, 2);
#else
    return kIOReturnNotReadable;
#endif
}

/************************************************************************/
__private_extern__ IOReturn _smcWakeTimerGetResults(uint16_t *mSec)
{
#if !TARGET_OS_EMBEDDED    
    uint8_t     size = 2;
    uint8_t     buf[2];
    IOReturn    ret;
    ret = _smcReadKey('CLWK', buf, &size);

    if (kIOReturnSuccess == ret) {
        *mSec = buf[0] | (buf[1] << 8);
    }

    return ret;
#else
    return kIOReturnNotReadable;
#endif
}


/************************************************************************/
/************************************************************************/
#if !TARGET_OS_EMBEDDED

// Todo: verify kSMCKeyNotFound
enum {
    kSMCKeyNotFound = 0x84
};

/* Do not modify - defined by AppleSMC.kext */
enum {
	kSMCSuccess	= 0,
	kSMCError	= 1
};
enum {
	kSMCUserClientOpen  = 0,
	kSMCUserClientClose = 1,
	kSMCHandleYPCEvent  = 2,	
    kSMCReadKey         = 5,
	kSMCWriteKey        = 6,
	kSMCGetKeyCount     = 7,
	kSMCGetKeyFromIndex = 8,
	kSMCGetKeyInfo      = 9
};
/* Do not modify - defined by AppleSMC.kext */
typedef struct SMCVersion 
{
    unsigned char    major;
    unsigned char    minor;
    unsigned char    build;
    unsigned char    reserved;
    unsigned short   release;
    
} SMCVersion;
/* Do not modify - defined by AppleSMC.kext */
typedef struct SMCPLimitData 
{
    uint16_t    version;
    uint16_t    length;
    uint32_t    cpuPLimit;
    uint32_t    gpuPLimit;
    uint32_t    memPLimit;

} SMCPLimitData;
/* Do not modify - defined by AppleSMC.kext */
typedef struct SMCKeyInfoData 
{
    IOByteCount         dataSize;
    uint32_t            dataType;
    uint8_t             dataAttributes;

} SMCKeyInfoData;
/* Do not modify - defined by AppleSMC.kext */
typedef struct {
    uint32_t            key;
    SMCVersion          vers;
    SMCPLimitData       pLimitData;
    SMCKeyInfoData      keyInfo;
    uint8_t             result;
    uint8_t             status;
    uint8_t             data8;
    uint32_t            data32;    
    uint8_t             bytes[32];
}  SMCParamStruct;

static IOReturn callSMCFunction(
    int which, 
    SMCParamStruct *inputValues, 
    SMCParamStruct *outputValues);

/************************************************************************/
// Methods
static IOReturn _smcWriteKey(
    uint32_t key,
    uint8_t *outBuf,
    uint8_t outBufMax)
{
    SMCParamStruct  stuffMeIn;
    SMCParamStruct  stuffMeOut;
    IOReturn        ret;
    int             i;

    if (key == 0) 
        return kIOReturnCannotWire;

    bzero(&stuffMeIn, sizeof(SMCParamStruct));
    bzero(&stuffMeOut, sizeof(SMCParamStruct));

    // Determine key's data size
    stuffMeIn.data8             = kSMCGetKeyInfo;
    stuffMeIn.key               = key;
    
    ret = callSMCFunction(kSMCHandleYPCEvent, &stuffMeIn, &stuffMeOut);
    if (kIOReturnSuccess != ret) {
        goto exit;
    }
    
    if (stuffMeOut.result == kSMCKeyNotFound) {
        ret = kIOReturnNotFound;
        goto exit;
    } else if (stuffMeOut.result != kSMCSuccess) {
        ret = kIOReturnInternalError;
        goto exit;
    }
    
    // Write Key
    stuffMeIn.data8             = kSMCWriteKey;
    stuffMeIn.key               = key;    
    stuffMeIn.keyInfo.dataSize  = stuffMeOut.keyInfo.dataSize;
    if (outBuf) {
        if (outBufMax > 32) outBufMax = 32;
        for (i=0; i<outBufMax; i++) {
            stuffMeIn.bytes[i] = outBuf[i];
        }
    }
    bzero(&stuffMeOut, sizeof(SMCParamStruct));
    ret = callSMCFunction(kSMCHandleYPCEvent, &stuffMeIn, &stuffMeOut);
    
    if (stuffMeOut.result != kSMCSuccess) {
        ret = kIOReturnInternalError;
        goto exit;
    }

exit:
    return ret;
}

static IOReturn _smcReadKey(
    uint32_t key,
    uint8_t *outBuf,
    uint8_t *outBufMax)
{
    SMCParamStruct  stuffMeIn;
    SMCParamStruct  stuffMeOut;
    IOReturn        ret;
    int             i;

    if (key == 0 || outBuf == NULL) 
        return kIOReturnCannotWire;

    // Determine key's data size
    bzero(outBuf, *outBufMax);
    bzero(&stuffMeIn, sizeof(SMCParamStruct));
    bzero(&stuffMeOut, sizeof(SMCParamStruct));
    stuffMeIn.data8 = kSMCGetKeyInfo;
    stuffMeIn.key = key;

    ret = callSMCFunction(kSMCHandleYPCEvent, &stuffMeIn, &stuffMeOut);
    if (kIOReturnSuccess != ret) {
        goto exit;
    }
    
    if (stuffMeOut.result == kSMCKeyNotFound) {
        ret = kIOReturnNotFound;
        goto exit;
    } else if (stuffMeOut.result != kSMCSuccess) {
        ret = kIOReturnInternalError;
        goto exit;
    }

    // Get Key Value
    stuffMeIn.data8 = kSMCReadKey;
    stuffMeIn.key = key;
    stuffMeIn.keyInfo.dataSize = stuffMeOut.keyInfo.dataSize;
    bzero(&stuffMeOut, sizeof(SMCParamStruct));
    ret = callSMCFunction(kSMCHandleYPCEvent, &stuffMeIn, &stuffMeOut);
    if (stuffMeOut.result == kSMCKeyNotFound) {
        ret = kIOReturnNotFound;
        goto exit;
    } else if (stuffMeOut.result != kSMCSuccess) {
        ret = kIOReturnInternalError;
        goto exit;
    }

    if (*outBufMax > stuffMeIn.keyInfo.dataSize)
        *outBufMax = stuffMeIn.keyInfo.dataSize;

    // Byte-swap data returning from the SMC.
    // The data at key 'ACID' are not provided by the SMC and do 
    // NOT need to be byte-swapped.
    for (i=0; i<*outBufMax; i++) 
    {
        if ('ACID' == key)
        {
            // Do not byte swap
            outBuf[i] = stuffMeOut.bytes[i];
        } else {
            // Byte swap
            outBuf[i] = stuffMeOut.bytes[*outBufMax - (i + 1)];
        }
    }
exit:
    return ret;
}

static IOReturn callSMCFunction(
    int which, 
    SMCParamStruct *inputValues, 
    SMCParamStruct *outputValues) 
{
    IOReturn result = kIOReturnError;

    size_t         inStructSize = sizeof(SMCParamStruct);
    size_t         outStructSize = sizeof(SMCParamStruct);
    
    io_connect_t    _SMCConnect = IO_OBJECT_NULL;
    io_service_t    smc = IO_OBJECT_NULL;

    smc = IOServiceGetMatchingService(
        kIOMasterPortDefault, 
        IOServiceMatching("AppleSMC"));
    if (IO_OBJECT_NULL == smc) {
        return kIOReturnNotFound;
    }
    
    result = IOServiceOpen(smc, mach_task_self(), 1, &_SMCConnect);        
    if (result != kIOReturnSuccess || 
        IO_OBJECT_NULL == _SMCConnect) {
        _SMCConnect = IO_OBJECT_NULL;
        goto exit;
    }
    
    result = IOConnectCallMethod(_SMCConnect, kSMCUserClientOpen, 
                    NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    if (result != kIOReturnSuccess) {
        goto exit;
    }
    
    result = IOConnectCallStructMethod(_SMCConnect, which, 
                        inputValues, inStructSize,
                        outputValues, &outStructSize);

exit:    
    if (IO_OBJECT_NULL != _SMCConnect) {
        IOConnectCallMethod(_SMCConnect, kSMCUserClientClose, 
                    NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
        IOServiceClose(_SMCConnect);    
    }

    return result;
}

#endif /* TARGET_OS_EMBEDDED */

/*****************************************************************************/
/*****************************************************************************/

__private_extern__ IOReturn 
_pm_scheduledevent_choose_best_wake_event(
    int                 selector,
    CFAbsoluteTime      chosenTime)
{
    static CFAbsoluteTime      chosenMaintTime = 0;
    static CFAbsoluteTime      chosenWakeTime = 0;

    static bool PMConnectionReported = false;
    static bool AutoWakeReported = false;

    CFDateRef       theChosenDate = NULL;
    CFStringRef     scheduleWakeType = NULL;
    CFAbsoluteTime  scheduleTime = 0;
    
    IOReturn        ret = kIOReturnSuccess;
    
    if (kChooseReset == selector)
    {
        chosenMaintTime = chosenWakeTime = 0;
        PMConnectionReported = AutoWakeReported = false;
//        asl_log(NULL, NULL, ASL_LEVEL_ERR, "PMConfigd choose event: RESET");
    } else     
    if (kChooseMaintenance == selector)
    {
        PMConnectionReported = true;
        chosenMaintTime = chosenTime;
//        asl_log(NULL, NULL, ASL_LEVEL_ERR, "PMConfigd choose event: Maintenance for delta %d secs", chosenTime == 0 ? 0 : (int)(chosenTime - CFAbsoluteTimeGetCurrent()));
    } else
    if (kChooseFullWake == selector)
    {
        AutoWakeReported = true;
        chosenWakeTime = chosenTime;
//        asl_log(NULL, NULL, ASL_LEVEL_ERR, "PMConfigd choose event: Full wake for delta %d secs", chosenTime == 0 ? 0 : (int)(chosenTime - CFAbsoluteTimeGetCurrent()));
    }

    /*****/
    
    if (!(PMConnectionReported && AutoWakeReported))
    {
        // Wait for the other one to respond.
        ret = kIOReturnSuccess;
        goto exit;
    } else
    if (chosenMaintTime == 0
      && chosenWakeTime == 0) 
    {
        // nothing to schedule. bail.
        ret = kIOReturnSuccess;
        goto exit;
    } else
    if (chosenMaintTime == 0
        || (chosenWakeTime != 0 
        && chosenWakeTime <= chosenMaintTime))
    {
        // Schedule the full wake.
        scheduleWakeType = CFSTR(kIOPMAutoWakeScheduleImmediate);
        scheduleTime = chosenWakeTime;
    } else
    if (chosenWakeTime == 0
        || (chosenMaintTime != 0
        && chosenWakeTime > chosenMaintTime))
    {
        // Schedule the maintenance wake.
        scheduleWakeType = CFSTR(kIOPMMaintenanceScheduleImmediate);
        scheduleTime = chosenMaintTime;
    }

    if (scheduleWakeType
        && scheduleTime != 0)
    {
        theChosenDate = CFDateCreate(0, scheduleTime);
        if (theChosenDate) 
        {
            ret = IOPMSchedulePowerEvent(theChosenDate, NULL, scheduleWakeType);

//        asl_log(NULL, NULL, ASL_LEVEL_ERR, "PMConfigd choose event: Scheduled event %s", 
//            (scheduleWakeType == CFSTR(kIOPMAutoWakeScheduleImmediate) ?
//                    kIOPMAutoWakeScheduleImmediate : kIOPMMaintenanceScheduleImmediate));
        
            CFRelease(theChosenDate);
        }
    }

exit:
    return ret;
}


/*
 *
 * Find HID service. Only used by wakeDozingMachine
 *
 */
#if HAVE_HID_SYSTEM
static kern_return_t openHIDService(io_connect_t *connection)
{
    kern_return_t       kr;
    io_service_t        service;
    io_connect_t        hid_connect = MACH_PORT_NULL;
    
    service = IOServiceGetMatchingService(MACH_PORT_NULL, 
                                          IOServiceMatching(kIOHIDSystemClass));
    if (MACH_PORT_NULL == service) {
        return kIOReturnNotFound;
    }
    
    kr = IOServiceOpen( service, mach_task_self(), 
                       kIOHIDParamConnectType, &hid_connect);    
    
    IOObjectRelease(service);
    
    if (kr != KERN_SUCCESS) {
        return kr;
    }
    
    *connection = hid_connect;
    return kr;
}

#endif /* HAVE_HID_SYSTEM */

/*
 *
 * Wakes a dozing machine by posting a NULL HID event
 * Will thus also wake displays on a running machine running
 *
 */
void wakeDozingMachine(void)
{
#if HAVE_HID_SYSTEM
    IOGPoint loc;
    kern_return_t kr;
    NXEvent nullEvent = {NX_NULLEVENT, {0, 0}, 0, -1, 0};
    static io_connect_t io_connection = MACH_PORT_NULL;
    
    // If the HID service has never been opened, do it now
    if (io_connection == MACH_PORT_NULL) 
    {
        kr = openHIDService(&io_connection);
        if (kr != KERN_SUCCESS) 
        {
            io_connection = MACH_PORT_NULL;
            return;
        }
    }
    
    // Finally, post a NULL event
    IOHIDPostEvent( io_connection, NX_NULLEVENT, loc, 
                    &nullEvent.data, FALSE, 0, FALSE );
#endif /* HAVE_HID_SYSTEM */
    return;
}







