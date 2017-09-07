/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VoldNativeService.h"
#include "VolumeManager.h"
#include "MoveTask.h"

#include <fstream>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <private/android_filesystem_config.h>

#ifndef LOG_TAG
#define LOG_TAG "vold"
#endif

using android::base::StringPrintf;
using std::endl;

namespace android {
namespace vold {

namespace {

constexpr const char* kDump = "android.permission.DUMP";

static binder::Status ok() {
    return binder::Status::ok();
}

static binder::Status exception(uint32_t code, const std::string& msg) {
    return binder::Status::fromExceptionCode(code, String8(msg.c_str()));
}

static binder::Status error(const std::string& msg) {
    PLOG(ERROR) << msg;
    return binder::Status::fromServiceSpecificError(errno, String8(msg.c_str()));
}

static binder::Status translate(uint32_t status) {
    if (status == 0) {
        return binder::Status::ok();
    } else {
        return binder::Status::fromExceptionCode(status);
    }
}

binder::Status checkPermission(const char* permission) {
    pid_t pid;
    uid_t uid;

    if (checkCallingPermission(String16(permission), reinterpret_cast<int32_t*>(&pid),
            reinterpret_cast<int32_t*>(&uid))) {
        return ok();
    } else {
        return exception(binder::Status::EX_SECURITY,
                StringPrintf("UID %d / PID %d lacks permission %s", uid, pid, permission));
    }
}

binder::Status checkUid(uid_t expectedUid) {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid == expectedUid || uid == AID_ROOT) {
        return ok();
    } else {
        return exception(binder::Status::EX_SECURITY,
                StringPrintf("UID %d is not expected UID %d", uid, expectedUid));
    }
}

#define ENFORCE_UID(uid) {                                  \
    binder::Status status = checkUid((uid));                \
    if (!status.isOk()) {                                   \
        return status;                                      \
    }                                                       \
}

#define ACQUIRE_LOCK std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());

}  // namespace

status_t VoldNativeService::start() {
    IPCThreadState::self()->disableBackgroundScheduling(true);
    status_t ret = BinderService<VoldNativeService>::publish();
    if (ret != android::OK) {
        return ret;
    }
    sp<ProcessState> ps(ProcessState::self());
    ps->startThreadPool();
    ps->giveThreadPoolName();
    return android::OK;
}

status_t VoldNativeService::dump(int fd, const Vector<String16> & /* args */) {
    auto out = std::fstream(StringPrintf("/proc/self/fd/%d", fd));
    const binder::Status dump_permission = checkPermission(kDump);
    if (!dump_permission.isOk()) {
        out << dump_permission.toString8() << endl;
        return PERMISSION_DENIED;
    }

    ACQUIRE_LOCK;
    out << "vold is happy!" << endl;
    out.flush();
    return NO_ERROR;
}

binder::Status VoldNativeService::reset() {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->reset());
}

binder::Status VoldNativeService::shutdown() {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->shutdown());
}

binder::Status VoldNativeService::setDebug(bool debug) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->setDebug(debug));
}

binder::Status VoldNativeService::onUserAdded(int32_t userId, int32_t userSerial) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->onUserAdded(userId, userSerial));
}

binder::Status VoldNativeService::onUserRemoved(int32_t userId) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->onUserRemoved(userId));
}

binder::Status VoldNativeService::onUserStarted(int32_t userId) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->onUserStarted(userId));
}

binder::Status VoldNativeService::onUserStopped(int32_t userId) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->onUserStopped(userId));
}

binder::Status VoldNativeService::partition(const std::string& diskId, int32_t partitionType, int32_t ratio) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    auto disk = VolumeManager::Instance()->findDisk(diskId);
    if (disk == nullptr) {
        return error("Failed to find disk " + diskId);
    }
    switch (partitionType) {
    case PARTITION_TYPE_PUBLIC: return translate(disk->partitionPublic());
    case PARTITION_TYPE_PRIVATE: return translate(disk->partitionPrivate());
    case PARTITION_TYPE_MIXED: return translate(disk->partitionMixed(ratio));
    default: return error("Unknown type " + std::to_string(partitionType));
    }
}

binder::Status VoldNativeService::forgetPartition(const std::string& partGuid) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->forgetPartition(partGuid));
}

binder::Status VoldNativeService::mount(const std::string& volId, int32_t mountFlags, int32_t mountUserId) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    auto vol = VolumeManager::Instance()->findVolume(volId);
    if (vol == nullptr) {
        return error("Failed to find volume " + volId);
    }

    vol->setMountFlags(mountFlags);
    vol->setMountUserId(mountUserId);

    int res = vol->mount();
    if (mountFlags & MOUNT_FLAG_PRIMARY) {
        VolumeManager::Instance()->setPrimary(vol);
    }
    return translate(res);
}

binder::Status VoldNativeService::unmount(const std::string& volId) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    auto vol = VolumeManager::Instance()->findVolume(volId);
    if (vol == nullptr) {
        return error("Failed to find volume " + volId);
    }
    return translate(vol->unmount());
}

binder::Status VoldNativeService::format(const std::string& volId, const std::string& fsType) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    auto vol = VolumeManager::Instance()->findVolume(volId);
    if (vol == nullptr) {
        return error("Failed to find volume " + volId);
    }
    return translate(vol->format(fsType));
}

binder::Status VoldNativeService::benchmark(const std::string& volId, int64_t* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    *_aidl_return = VolumeManager::Instance()->benchmarkPrivate(volId);
    return ok();
}

binder::Status VoldNativeService::moveStorage(const std::string& fromVolId, const std::string& toVolId) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    auto fromVol = VolumeManager::Instance()->findVolume(fromVolId);
    auto toVol = VolumeManager::Instance()->findVolume(toVolId);
    if (fromVol == nullptr) {
        return error("Failed to find volume " + fromVolId);
    } else if (toVol == nullptr) {
        return error("Failed to find volume " + toVolId);
    }
    (new android::vold::MoveTask(fromVol, toVol))->start();
    return ok();
}

binder::Status VoldNativeService::remountUid(int32_t uid, int32_t remountMode) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    std::string tmp;
    switch (remountMode) {
    case REMOUNT_MODE_NONE: tmp = "none"; break;
    case REMOUNT_MODE_DEFAULT: tmp = "default"; break;
    case REMOUNT_MODE_READ: tmp = "read"; break;
    case REMOUNT_MODE_WRITE: tmp = "write"; break;
    default: return error("Unknown mode " + std::to_string(remountMode));
    }
    return translate(VolumeManager::Instance()->remountUid(uid, tmp));
}

binder::Status VoldNativeService::mkdirs(const std::string& path) {
    ENFORCE_UID(AID_SYSTEM);
    ACQUIRE_LOCK;

    return translate(VolumeManager::Instance()->mkdirs(path.c_str()));
}

}  // namespace vold
}  // namespace android
