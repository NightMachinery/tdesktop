#include "purple/purple_archive_files.h"
#import <Foundation/Foundation.h>

namespace Purple {

bool DesktopArchiveExcludeBackup(const QString &path) {
	@autoreleasepool {
		const auto bytes = path.toUtf8();
		const auto name = [NSString stringWithUTF8String:bytes.constData()];
		if (!name) {
			return false;
		}
		const auto url = [NSURL fileURLWithPath:name];
		NSError* error = nil;
		if (![url setResourceValue:@YES
							forKey:NSURLIsExcludedFromBackupKey
							 error:&error]) {
			return false;
		}
		NSNumber* excluded = nil;
		return [url getResourceValue:&excluded
							  forKey:NSURLIsExcludedFromBackupKey
							   error:&error]
			   && excluded.boolValue;
	}
}

}
