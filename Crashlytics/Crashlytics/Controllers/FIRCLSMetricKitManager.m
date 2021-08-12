// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#import <Foundation/Foundation.h>

#import "Crashlytics/Crashlytics/Controllers/FIRCLSMetricKitManager.h"

#if CLS_METRICKIT_SUPPORTED

#import "Crashlytics/Crashlytics/Controllers/FIRCLSManagerData.h"
#import "Crashlytics/Crashlytics/Helpers/FIRCLSCallStackTree.h"
#import "Crashlytics/Crashlytics/Helpers/FIRCLSFile.h"
#import "Crashlytics/Crashlytics/Helpers/FIRCLSLogger.h"
#import "Crashlytics/Crashlytics/Models/FIRCLSExecutionIdentifierModel.h"
#import "Crashlytics/Crashlytics/Models/FIRCLSInternalReport.h"
#import "Crashlytics/Crashlytics/Public/FirebaseCrashlytics/FIRCrashlytics.h"
#import "Crashlytics/Crashlytics/Public/FirebaseCrashlytics/FIRCrashlyticsReport.h"

@interface FIRCLSMetricKitManager ()

@property FBLPromise *metricKitDataAvailable;
@property FIRCLSExistingReportManager *existingReportManager;
@property FIRCLSFileManager *fileManager;
@property FIRCLSManagerData *managerData;
@property BOOL metricKitPromiseFulfilled;

@end

@implementation FIRCLSMetricKitManager

- (instancetype)initWithManagerData:(FIRCLSManagerData *)managerData
              existingReportManager:(FIRCLSExistingReportManager *)existingReportManager
                        fileManager:(FIRCLSFileManager *)fileManager {
  _existingReportManager = existingReportManager;
  _fileManager = fileManager;
  _managerData = managerData;
  _metricKitPromiseFulfilled = NO;
  return self;
}

/*
 * Registers the MetricKit manager to receive MetricKit reports by adding self to the
 * MXMetricManager subscribers. Also initializes the promise that we'll use to ensure that any
 * MetricKit report files are included in Crashylytics fatal reports. If no crash occurred on the
 * last run of the app, this promise is immediately resolved so that the upload of any nonfatal
 * events can proceed.
 */
- (void)registerMetricKitManager {
  [[MXMetricManager sharedManager] addSubscriber:self];
  self.metricKitDataAvailable = [FBLPromise pendingPromise];

  // If there was no crash on the last run of the app or there's no diagnostic report in the
  // MetricKit directory, then we aren't expecting a MetricKit diagnostic report and should resolve
  // the promise immediately. If MetricKit captured a fatal event and Crashlytics did not, then
  // we'll still process the MetricKit crash but won't upload it until the app restarts again.
  if (![self.fileManager didCrashOnPreviousExecution] ||
      ![self.fileManager metricKitDiagnosticFileExists]) {
    @synchronized(self) {
      [self fulfillMetricKitPromise];
    }
  }

  // If we haven't resolved this promise within three seconds, resolve it now so that we're not
  // waiting indefinitely for MetricKit payloads that won't arrive.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), self.managerData.dispatchQueue,
                 ^{
                   @synchronized(self) {
                     if (!self.metricKitPromiseFulfilled) {
                       FIRCLSDebugLog(@"Resolving MetricKit promise after three seconds");
                       [self fulfillMetricKitPromise];
                     }
                   }
                 });

  FIRCLSDebugLog(@"Finished registering metrickit manager");
}

/*
 * This method receives diagnostic payloads from MetricKit whenever a fatal or nonfatal MetricKit
 * event occurs. If a fatal event, this method will be called when the app restarts. Since we're
 * including a MetricKit report file in the Crashlytics report to be sent to the backend, we need
 * to make sure that we process the payloads and write the included information to file before
 * the report is sent up. If this method is called due to a nonfatal event, it will be called
 * immediately after the event. Since we send nonfatal events on the next run of the app, we can
 * write out the information but won't need to resolve the promise.
 */
- (void)didReceiveDiagnosticPayloads:(NSArray<MXDiagnosticPayload *> *)payloads {
  BOOL processedFatalPayload = NO;
  for (MXDiagnosticPayload *diagnosticPayload in payloads) {
    if (!diagnosticPayload) {
      continue;
    }

    BOOL processedPayload = [self processMetricKitPayload:diagnosticPayload
                                           skipCrashEvent:processedFatalPayload];
    if (processedPayload && ([diagnosticPayload.crashDiagnostics count] > 0)) {
      processedFatalPayload = YES;
    }
  }
  // Once we've processed all the payloads, resolve the promise so that reporting uploading
  // continues. If there was not a crash on the previous run of the app, the promise will already
  // have been resolved.
  @synchronized(self) {
    [self fulfillMetricKitPromise];
  }
}

// Helper method to write a MetricKit payload's data to file.
- (BOOL)processMetricKitPayload:(MXDiagnosticPayload *)diagnosticPayload
                 skipCrashEvent:(BOOL)skipCrashEvent {
  BOOL writeFailed = NO;

  // Write out each type of diagnostic if it exists in the report
  BOOL hasCrash = [diagnosticPayload.crashDiagnostics count] > 0;
  BOOL hasHang = [diagnosticPayload.hangDiagnostics count] > 0;
  BOOL hasCPUException = [diagnosticPayload.cpuExceptionDiagnostics count] > 0;
  BOOL hasDiskWriteException = [diagnosticPayload.diskWriteExceptionDiagnostics count] > 0;

  // If there are no diagnostics in the report, return before writing out any files.
  if (!hasCrash && !hasHang && !hasCPUException && !hasDiskWriteException) {
    return false;
  }

  // TODO: Time stamp information is only available in begin and end time periods. Hopefully this
  // is updated with iOS 15.
  NSTimeInterval beginSecondsSince1970 = [diagnosticPayload.timeStampBegin timeIntervalSince1970];
  NSTimeInterval endSecondsSince1970 = [diagnosticPayload.timeStampEnd timeIntervalSince1970];

  // Get file path for the active reports directory.
  NSString *activePath = [[self.fileManager activePath] stringByAppendingString:@"/"];

  // If there is a crash diagnostic in the payload, then this method was called for a fatal event.
  // Also ensure that there is a report from the last run of the app that we can write to.
  NSString *metricKitFatalReportFile;
  NSString *metricKitNonfatalReportFile;
  NSString *metricKitReportFile;
  NSString *newestUnsentReportID =
      [self.existingReportManager.newestUnsentReport.reportID stringByAppendingString:@"/"];
  NSString *currentReportID =
      [_managerData.executionIDModel.executionID stringByAppendingString:@"/"];
  BOOL fatal = ([diagnosticPayload.crashDiagnostics count] > 0) && (newestUnsentReportID != nil) &&
               ([self.fileManager
                   fileExistsAtPath:[activePath stringByAppendingString:newestUnsentReportID]]);

  // Set the MetricKit paths appropriately depending on whether the diagnostic report came from
  // a fatal or nonfatal event. If fatal, use the report from the last run of the app and write data
  // to the fatal MetricKit file. Otherwise, use the report for the current run for nonfatal events
  // and write data to the nonfatal MetricKit file.
  if (fatal) {
    metricKitReportFile = [[activePath stringByAppendingString:newestUnsentReportID]
        stringByAppendingString:FIRCLSMetricKitFatalReportFile];
  } else {
    metricKitReportFile = [[activePath stringByAppendingString:currentReportID]
        stringByAppendingString:FIRCLSMetricKitNonfatalReportFile];
  }

  if (!metricKitReportFile) {
    FIRCLSDebugLog(@"Error finding MetricKit file");
    return NO;
  }

  FIRCLSDebugLog(@"File path for MetricKit report:  %@", metricKitReportFile);
  if (![_fileManager fileExistsAtPath:metricKitReportFile]) {
    [_fileManager createFileAtPath:metricKitReportFile contents:nil attributes:nil];
  }
  NSFileHandle *file = [NSFileHandle fileHandleForUpdatingAtPath:metricKitReportFile];
  if (file == nil) {
    FIRCLSDebugLog(@"Unable to create or open nonfatal MetricKit file.");
    return false;
  }

  // Write out time information to the MetricKit report file. Time needs to be a value for
  // backend serialization, so we write out end_time separately.
  // TODO: should we write out multiple time dictionaries if there are multiple diagnostics?
  NSData *newLineData = [@"\n" dataUsingEncoding:NSUTF8StringEncoding];
  NSDictionary *timeDictionary = @{
    @"time" : [NSNumber numberWithLong:beginSecondsSince1970],
    @"end_time" : [NSNumber numberWithLong:endSecondsSince1970]
  };
  writeFailed = ![self writeDictionaryToFile:timeDictionary file:file newLineData:newLineData];

  // For each diagnostic type, write out a section in the MetricKit report file. This section will
  // have subsections for threads, metadata, and event specific metadata.
  if (hasCrash && !skipCrashEvent) {
    MXCrashDiagnostic *crashDiagnostic = [diagnosticPayload.crashDiagnostics objectAtIndex:0];

    NSArray *threadArray = [self convertThreadsToArray:crashDiagnostic.callStackTree];
    NSDictionary *metadataDict = [self convertMetadataToDictionary:crashDiagnostic.metaData];

    NSString *nilString = @"";
    NSDictionary *crashDictionary = @{
      @"crash_event" : @{
        @"threads" : threadArray,
        @"metadata" : metadataDict,
        @"termination_reason" :
                (crashDiagnostic.terminationReason) ? crashDiagnostic.terminationReason : nilString,
        @"virtual_memory_region_info" : (crashDiagnostic.virtualMemoryRegionInfo)
            ? crashDiagnostic.virtualMemoryRegionInfo
            : nilString,
        @"exception_type" : crashDiagnostic.exceptionType,
        @"exception_code" : crashDiagnostic.exceptionCode,
        @"signal" : crashDiagnostic.signal,
        @"app_version" : crashDiagnostic.applicationVersion
      }
    };
    writeFailed = ![self writeDictionaryToFile:crashDictionary file:file newLineData:newLineData];
  }

  if (hasHang) {
    MXHangDiagnostic *hangDiagnostic = [diagnosticPayload.hangDiagnostics objectAtIndex:0];

    NSArray *threadArray = [self convertThreadsToArrayForNonfatal:hangDiagnostic.callStackTree];
    NSDictionary *metadataDict = [self convertMetadataToDictionary:hangDiagnostic.metaData];

    NSDictionary *hangDictionary = @{
      @"exception" : @{
        @"type" : @"metrickit",
        @"name" : @"MetricKit Hang Event",
        @"reason" : @"",
        @"time" : [NSNumber numberWithLong:beginSecondsSince1970],
        @"end_time" : [NSNumber numberWithLong:endSecondsSince1970],
        @"frames" : threadArray,
        @"metadata" : metadataDict,
        @"hang_duration" : [NSNumber numberWithDouble:[hangDiagnostic.hangDuration doubleValue]],
        @"app_version" : hangDiagnostic.applicationVersion
      }
    };

    writeFailed = ![self writeDictionaryToFile:hangDictionary file:file newLineData:newLineData];
  }

  if (hasCPUException) {
    MXCPUExceptionDiagnostic *cpuExceptionDiagnostic =
        [diagnosticPayload.cpuExceptionDiagnostics objectAtIndex:0];

    NSArray *threadArray =
        [self convertThreadsToArrayForNonfatal:cpuExceptionDiagnostic.callStackTree];
    NSDictionary *metadataDict = [self convertMetadataToDictionary:cpuExceptionDiagnostic.metaData];

    NSDictionary *cpuDictionary = @{
      @"exception" : @{
        @"type" : @"metrickit",
        @"name" : @"MetricKit CPU Exception Event",
        @"reason" : @"",
        @"time" : [NSNumber numberWithLong:beginSecondsSince1970],
        @"end_time" : [NSNumber numberWithLong:endSecondsSince1970],
        @"frames" : threadArray,
        @"metadata" : metadataDict,
        @"total_cpu_time" :
            [NSNumber numberWithDouble:[cpuExceptionDiagnostic.totalCPUTime doubleValue]],
        @"total_sampled_time" :
            [NSNumber numberWithDouble:[cpuExceptionDiagnostic.totalSampledTime doubleValue]],
        @"app_version" : cpuExceptionDiagnostic.applicationVersion
      }
    };
    writeFailed = ![self writeDictionaryToFile:cpuDictionary file:file newLineData:newLineData];
  }

  if (hasDiskWriteException) {
    MXDiskWriteExceptionDiagnostic *diskWriteExceptionDiagnostic =
        [diagnosticPayload.diskWriteExceptionDiagnostics objectAtIndex:0];

    NSArray *threadArray =
        [self convertThreadsToArrayForNonfatal:diskWriteExceptionDiagnostic.callStackTree];
    NSDictionary *metadataDict =
        [self convertMetadataToDictionary:diskWriteExceptionDiagnostic.metaData];

    NSDictionary *diskWriteDictionary = @{
      @"exception" : @{
        @"type" : @"metrickit",
        @"name" : @"MetricKit Disk Write Exception Event",
        @"reason" : @"",
        @"time" : [NSNumber numberWithLong:beginSecondsSince1970],
        @"end_time" : [NSNumber numberWithLong:endSecondsSince1970],
        @"frames" : threadArray,
        @"metadata" : metadataDict,
        @"app_version" : diskWriteExceptionDiagnostic.applicationVersion,
        @"total_writes_caused" :
            [NSNumber numberWithDouble:[diskWriteExceptionDiagnostic.totalWritesCaused doubleValue]]
      }
    };
    writeFailed = ![self writeDictionaryToFile:diskWriteDictionary
                                          file:file
                                   newLineData:newLineData];
  }

  return !writeFailed;
}
/*
 * Required for MXMetricManager subscribers. Since we aren't currently collecting any MetricKit
 * metrics, this method is left empty.
 */
- (void)didReceiveMetricPayloads:(NSArray<MXMetricPayload *> *)payloads {
}

- (FBLPromise *)waitForMetricKitDataAvailable {
  FBLPromise *result = nil;
  @synchronized(self) {
    result = self.metricKitDataAvailable;
  }
  return result;
}

/*
 * Helper method to convert threads for a MetricKit fatal diagnostic event to an array of threads.
 */
- (NSArray *)convertThreadsToArray:(MXCallStackTree *)mxCallStackTree {
  FIRCLSCallStackTree *tree = [[FIRCLSCallStackTree alloc] initWithMXCallStackTree:mxCallStackTree];
  return [tree getArrayRepresentation];
}

/*
 * Helper method to convert threads for a MetricKit nonfatal diagnostic event to an array of frames.
 */
- (NSArray *)convertThreadsToArrayForNonfatal:(MXCallStackTree *)mxCallStackTree {
  FIRCLSCallStackTree *tree = [[FIRCLSCallStackTree alloc] initWithMXCallStackTree:mxCallStackTree];
  return [tree getFramesOfBlamedThread];
}

/*
 * Helper method to convert metadata for a MetricKit diagnostic event to a dictionary. MXMetadata
 * has a dictionaryRepresentation method but it is deprecated.
 */
- (NSDictionary *)convertMetadataToDictionary:(MXMetaData *)metadata {
  NSError *error = nil;
  NSDictionary *metadataDictionary =
      [NSJSONSerialization JSONObjectWithData:[metadata JSONRepresentation] options:0 error:&error];
  return metadataDictionary;
}

/*
 * Helper method to fulfill the metricKitDataAvailable promise and track that it has been fulfilled.
 */
- (void)fulfillMetricKitPromise {
  if (self.metricKitPromiseFulfilled) return;

  [self.metricKitDataAvailable fulfill:nil];
  self.metricKitPromiseFulfilled = YES;
}

/*
 * Helper method to write a dictionary of event information to file. Returns whether it succeeded.
 */
- (BOOL)writeDictionaryToFile:(NSDictionary *)dictionary
                         file:(NSFileHandle *)file
                  newLineData:(NSData *)newLineData {
  NSError *dataError = nil;
  NSData *data = [NSJSONSerialization dataWithJSONObject:dictionary options:0 error:&dataError];
  if (dataError) {
    FIRCLSDebugLog(@"Unable to write out dictionary.");
    return NO;
  }

  [file seekToEndOfFile];
  [file writeData:data];
  [file writeData:newLineData];

  return YES;
}

@end

#endif  // CLS_METRICKIT_SUPPORTED
