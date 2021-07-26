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

#if TARGET_OS_IPHONE
@import MetricKit;

#import "Crashlytics/Crashlytics/Controllers/FIRCLSExistingReportManager.h"
#import "Crashlytics/Crashlytics/Models/FIRCLSFileManager.h"

#if __has_include(<FBLPromises/FBLPromises.h>)
#import <FBLPromises/FBLPromises.h>
#else
#import "FBLPromises.h"
#endif

NS_ASSUME_NONNULL_BEGIN

API_AVAILABLE(ios(14.0))
@interface FIRCLSMetricKitManager : NSObject <MXMetricManagerSubscriber>

- (instancetype)initWithManagerData:(FIRCLSManagerData *)managerData
              existingReportManager:(FIRCLSExistingReportManager *)existingReportManager
                        fileManager:(FIRCLSFileManager *)fileManager;

- (instancetype)init NS_UNAVAILABLE;
- (void)registerMetricKitManager;
- (FBLPromise *)waitForMetricKitDataAvailable;

@end

#endif

NS_ASSUME_NONNULL_END
