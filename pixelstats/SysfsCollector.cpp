/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <pixelstats/SysfsCollector.h>

#define LOG_TAG "pixelstats-vendor"

#include <aidl/android/frameworks/stats/IStats.h>
#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <hardware/google/pixel/pixelstats/pixelatoms.pb.h>
#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <utils/Timers.h>

#include <cinttypes>
#include <sys/timerfd.h>
#include <mntent.h>
#include <string>

namespace {

using aidl::android::frameworks::stats::IStats;

std::shared_ptr<IStats> getStatsService() {
    const std::string instance = std::string() + IStats::descriptor + "/default";
    if (!AServiceManager_isDeclared(instance.c_str())) {
        ALOGE("IStats service is not registered.");
        return nullptr;
    }
    return IStats::fromBinder(ndk::SpAIBinder(AServiceManager_waitForService(instance.c_str())));
}

using aidl::android::frameworks::stats::VendorAtom;
using aidl::android::frameworks::stats::VendorAtomValue;
namespace PixelAtoms = android::hardware::google::pixel::PixelAtoms;

void reportSpeakerImpedance(const std::shared_ptr<IStats> &stats_client,
                            const PixelAtoms::VendorSpeakerImpedance &speakerImpedance) {
    // Load values array
    std::vector<VendorAtomValue> values(2);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(speakerImpedance.speaker_location());
    values[0] = tmp;
    tmp.set<VendorAtomValue::intValue>(speakerImpedance.impedance());
    values[1] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::VENDOR_SPEAKER_IMPEDANCE,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk())
        ALOGE("Unable to report VendorSpeakerImpedance to Stats service");
}

}  // namespace

namespace android {
namespace hardware {
namespace google {
namespace pixel {

using aidl::android::frameworks::stats::VendorAtom;
using aidl::android::frameworks::stats::VendorAtomValue;
using android::base::ReadFileToString;
using android::base::StartsWith;
using android::frameworks::stats::V1_0::ChargeCycles;
using android::frameworks::stats::V1_0::HardwareFailed;
using android::frameworks::stats::V1_0::SlowIo;
using android::frameworks::stats::V1_0::SpeechDspStat;
using android::hardware::google::pixel::PixelAtoms::BatteryCapacity;
using android::hardware::google::pixel::PixelAtoms::BootStatsInfo;
using android::hardware::google::pixel::PixelAtoms::F2fsStatsInfo;
using android::hardware::google::pixel::PixelAtoms::StorageUfsHealth;
using android::hardware::google::pixel::PixelAtoms::StorageUfsResetCount;
using android::hardware::google::pixel::PixelAtoms::VendorSpeakerImpedance;
using android::hardware::google::pixel::PixelAtoms::ZramBdStat;
using android::hardware::google::pixel::PixelAtoms::ZramMmStat;

SysfsCollector::SysfsCollector(const struct SysfsPaths &sysfs_paths)
    : kSlowioReadCntPath(sysfs_paths.SlowioReadCntPath),
      kSlowioWriteCntPath(sysfs_paths.SlowioWriteCntPath),
      kSlowioUnmapCntPath(sysfs_paths.SlowioUnmapCntPath),
      kSlowioSyncCntPath(sysfs_paths.SlowioSyncCntPath),
      kCycleCountBinsPath(sysfs_paths.CycleCountBinsPath),
      kImpedancePath(sysfs_paths.ImpedancePath),
      kCodecPath(sysfs_paths.CodecPath),
      kCodec1Path(sysfs_paths.Codec1Path),
      kSpeechDspPath(sysfs_paths.SpeechDspPath),
      kBatteryCapacityCC(sysfs_paths.BatteryCapacityCC),
      kBatteryCapacityVFSOC(sysfs_paths.BatteryCapacityVFSOC),
      kUFSLifetimeA(sysfs_paths.UFSLifetimeA),
      kUFSLifetimeB(sysfs_paths.UFSLifetimeB),
      kUFSLifetimeC(sysfs_paths.UFSLifetimeC),
      kUFSHostResetPath(sysfs_paths.UFSHostResetPath),
      kF2fsStatsPath(sysfs_paths.F2fsStatsPath),
      kZramMmStatPath("/sys/block/zram0/mm_stat"),
      kZramBdStatPath("/sys/block/zram0/bd_stat"),
      kEEPROMPath(sysfs_paths.EEPROMPath) {}

bool SysfsCollector::ReadFileToInt(const std::string &path, int *val) {
    return ReadFileToInt(path.c_str(), val);
}

bool SysfsCollector::ReadFileToInt(const char *const path, int *val) {
    std::string file_contents;

    if (!ReadFileToString(path, &file_contents)) {
        ALOGE("Unable to read %s - %s", path, strerror(errno));
        return false;
    } else if (StartsWith(file_contents, "0x")) {
        if (sscanf(file_contents.c_str(), "0x%x", val) != 1) {
            ALOGE("Unable to convert %s to hex - %s", path, strerror(errno));
            return false;
        }
    } else if (sscanf(file_contents.c_str(), "%d", val) != 1) {
        ALOGE("Unable to convert %s to int - %s", path, strerror(errno));
        return false;
    }
    return true;
}

/**
 * Read the contents of kCycleCountBinsPath and report them via IStats HAL.
 * The contents are expected to be N buckets total, the nth of which indicates the
 * number of times battery %-full has been increased with the n/N% full bucket.
 */
void SysfsCollector::logBatteryChargeCycles() {
    std::string file_contents;
    int val;
    std::vector<int> charge_cycles;
    if (kCycleCountBinsPath == nullptr || strlen(kCycleCountBinsPath) == 0) {
        ALOGV("Battery charge cycle path not specified");
        return;
    }
    if (!ReadFileToString(kCycleCountBinsPath, &file_contents)) {
        ALOGE("Unable to read battery charge cycles %s - %s", kCycleCountBinsPath, strerror(errno));
        return;
    }

    std::stringstream stream(file_contents);
    while (stream >> val) {
        charge_cycles.push_back(val);
    }
    ChargeCycles cycles;
    cycles.cycleBucket = charge_cycles;

    std::replace(file_contents.begin(), file_contents.end(), ' ', ',');
    stats_->reportChargeCycles(cycles);
}

/**
 * Read the contents of kEEPROMPath and report them.
 */
void SysfsCollector::logBatteryEEPROM() {
    if (kEEPROMPath == nullptr || strlen(kEEPROMPath) == 0) {
        ALOGV("Battery EEPROM path not specified");
        return;
    }

    battery_EEPROM_reporter_.checkAndReport(kEEPROMPath);
}

/**
 * Check the codec for failures over the past 24hr.
 */
void SysfsCollector::logCodecFailed() {
    std::string file_contents;
    if (kCodecPath == nullptr || strlen(kCodecPath) == 0) {
        ALOGV("Audio codec path not specified");
        return;
    }
    if (!ReadFileToString(kCodecPath, &file_contents)) {
        ALOGE("Unable to read codec state %s - %s", kCodecPath, strerror(errno));
        return;
    }
    if (file_contents == "0") {
        return;
    } else {
        HardwareFailed failed = {.hardwareType = HardwareFailed::HardwareType::CODEC,
                                 .hardwareLocation = 0,
                                 .errorCode = HardwareFailed::HardwareErrorCode::COMPLETE};
        stats_->reportHardwareFailed(failed);
    }
}

/**
 * Check the codec1 for failures over the past 24hr.
 */
void SysfsCollector::logCodec1Failed() {
    std::string file_contents;
    if (kCodec1Path == nullptr || strlen(kCodec1Path) == 0) {
        ALOGV("Audio codec1 path not specified");
        return;
    }
    if (!ReadFileToString(kCodec1Path, &file_contents)) {
        ALOGE("Unable to read codec1 state %s - %s", kCodec1Path, strerror(errno));
        return;
    }
    if (file_contents == "0") {
        return;
    } else {
        ALOGE("%s report hardware fail", kCodec1Path);
        HardwareFailed failed = {.hardwareType = HardwareFailed::HardwareType::CODEC,
                                 .hardwareLocation = 1,
                                 .errorCode = HardwareFailed::HardwareErrorCode::COMPLETE};
        stats_->reportHardwareFailed(failed);
    }
}

void SysfsCollector::reportSlowIoFromFile(const char *path,
                                          const SlowIo::IoOperation &operation_s) {
    std::string file_contents;
    if (path == nullptr || strlen(path) == 0) {
        ALOGV("slow_io path not specified");
        return;
    }
    if (!ReadFileToString(path, &file_contents)) {
        ALOGE("Unable to read slowio %s - %s", path, strerror(errno));
        return;
    } else {
        int32_t slow_io_count = 0;
        if (sscanf(file_contents.c_str(), "%d", &slow_io_count) != 1) {
            ALOGE("Unable to parse %s from file %s to int.", file_contents.c_str(), path);
        } else if (slow_io_count > 0) {
            SlowIo slowio = {.operation = operation_s, .count = slow_io_count};
            stats_->reportSlowIo(slowio);
        }
        // Clear the stats
        if (!android::base::WriteStringToFile("0", path, true)) {
            ALOGE("Unable to clear SlowIO entry %s - %s", path, strerror(errno));
        }
    }
}

/**
 * Check for slow IO operations.
 */
void SysfsCollector::logSlowIO() {
    reportSlowIoFromFile(kSlowioReadCntPath, SlowIo::IoOperation::READ);
    reportSlowIoFromFile(kSlowioWriteCntPath, SlowIo::IoOperation::WRITE);
    reportSlowIoFromFile(kSlowioUnmapCntPath, SlowIo::IoOperation::UNMAP);
    reportSlowIoFromFile(kSlowioSyncCntPath, SlowIo::IoOperation::SYNC);
}

/**
 * Report the last-detected impedance of left & right speakers.
 */
void SysfsCollector::logSpeakerImpedance(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (kImpedancePath == nullptr || strlen(kImpedancePath) == 0) {
        ALOGV("Audio impedance path not specified");
        return;
    }
    if (!ReadFileToString(kImpedancePath, &file_contents)) {
        ALOGE("Unable to read impedance path %s", kImpedancePath);
        return;
    }

    float left, right;
    if (sscanf(file_contents.c_str(), "%g,%g", &left, &right) != 2) {
        ALOGE("Unable to parse speaker impedance %s", file_contents.c_str());
        return;
    }
    VendorSpeakerImpedance left_obj;
    left_obj.set_speaker_location(0);
    left_obj.set_impedance(static_cast<int32_t>(left * 1000));

    VendorSpeakerImpedance right_obj;
    right_obj.set_speaker_location(1);
    right_obj.set_impedance(static_cast<int32_t>(right * 1000));

    reportSpeakerImpedance(stats_client, left_obj);
    reportSpeakerImpedance(stats_client, right_obj);
}

/**
 * Report the Speech DSP state.
 */
void SysfsCollector::logSpeechDspStat() {
    std::string file_contents;
    if (kSpeechDspPath == nullptr || strlen(kSpeechDspPath) == 0) {
        ALOGV("Speech DSP path not specified");
        return;
    }
    if (!ReadFileToString(kSpeechDspPath, &file_contents)) {
        ALOGE("Unable to read speech dsp path %s", kSpeechDspPath);
        return;
    }

    int32_t uptime = 0, downtime = 0, crashcount = 0, recovercount = 0;
    if (sscanf(file_contents.c_str(), "%d,%d,%d,%d", &uptime, &downtime, &crashcount,
               &recovercount) != 4) {
        ALOGE("Unable to parse speech dsp stat %s", file_contents.c_str());
        return;
    }

    ALOGD("SpeechDSP uptime %d downtime %d crashcount %d recovercount %d", uptime, downtime,
          crashcount, recovercount);
    SpeechDspStat dspstat = {.totalUptimeMillis = uptime,
                             .totalDowntimeMillis = downtime,
                             .totalCrashCount = crashcount,
                             .totalRecoverCount = recovercount};

    stats_->reportSpeechDspStat(dspstat);
}

void SysfsCollector::logBatteryCapacity(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (kBatteryCapacityCC == nullptr || strlen(kBatteryCapacityCC) == 0) {
        ALOGV("Battery Capacity CC path not specified");
        return;
    }
    if (kBatteryCapacityVFSOC == nullptr || strlen(kBatteryCapacityVFSOC) == 0) {
        ALOGV("Battery Capacity VFSOC path not specified");
        return;
    }
    int delta_cc_sum, delta_vfsoc_sum;
    if (!ReadFileToInt(kBatteryCapacityCC, &delta_cc_sum) ||
            !ReadFileToInt(kBatteryCapacityVFSOC, &delta_vfsoc_sum))
        return;

    // Load values array
    std::vector<VendorAtomValue> values(2);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(delta_cc_sum);
    values[BatteryCapacity::kDeltaCcSumFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(delta_vfsoc_sum);
    values[BatteryCapacity::kDeltaVfsocSumFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::BATTERY_CAPACITY,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk())
        ALOGE("Unable to report ChargeStats to Stats service");
}

void SysfsCollector::logUFSLifetime(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (kUFSLifetimeA == nullptr || strlen(kUFSLifetimeA) == 0) {
        ALOGV("UFS lifetimeA path not specified");
        return;
    }
    if (kUFSLifetimeB == nullptr || strlen(kUFSLifetimeB) == 0) {
        ALOGV("UFS lifetimeB path not specified");
        return;
    }
    if (kUFSLifetimeC == nullptr || strlen(kUFSLifetimeC) == 0) {
        ALOGV("UFS lifetimeC path not specified");
        return;
    }

    int lifetimeA = 0, lifetimeB = 0, lifetimeC = 0;
    if (!ReadFileToInt(kUFSLifetimeA, &lifetimeA) ||
        !ReadFileToInt(kUFSLifetimeB, &lifetimeB) ||
        !ReadFileToInt(kUFSLifetimeC, &lifetimeC)) {
        ALOGE("Unable to read UFS lifetime : %s", strerror(errno));
        return;
    }

    // Load values array
    std::vector<VendorAtomValue> values(3);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(lifetimeA);
    values[StorageUfsHealth::kLifetimeAFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(lifetimeB);
    values[StorageUfsHealth::kLifetimeBFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(lifetimeC);
    values[StorageUfsHealth::kLifetimeCFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::STORAGE_UFS_HEALTH,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report UfsHealthStat to Stats service");
    }
}

void SysfsCollector::logUFSErrorStats(const std::shared_ptr<IStats> &stats_client) {
    int host_reset_count;

    if (kUFSHostResetPath == nullptr || strlen(kUFSHostResetPath) == 0) {
        ALOGV("UFS host reset count specified");
        return;
    }

    if (!ReadFileToInt(kUFSHostResetPath, &host_reset_count)) {
        ALOGE("Unable to read host reset count");
        return;
    }

    // Load values array
    std::vector<VendorAtomValue> values(1);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(host_reset_count);
    values[StorageUfsResetCount::kHostResetCountFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::UFS_RESET_COUNT,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report UFS host reset count to Stats service");
    }
}

static std::string getUserDataBlock() {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> fp(setmntent("/proc/mounts", "re"), endmntent);
    if (fp == nullptr) {
        ALOGE("Error opening /proc/mounts");
        return "";
    }

    mntent* mentry;
    while ((mentry = getmntent(fp.get())) != nullptr) {
        if (strcmp(mentry->mnt_dir, "/data") == 0) {
            return std::string(basename(mentry->mnt_fsname));
        }
    }
    return "";
}

void SysfsCollector::logF2fsStats(const std::shared_ptr<IStats> &stats_client) {
    int dirty, free, cp_calls_fg, gc_calls_fg, moved_block_fg, vblocks;
    int cp_calls_bg, gc_calls_bg, moved_block_bg;

    if (kF2fsStatsPath == nullptr) {
        ALOGE("F2fs stats path not specified");
        return;
    }

    const std::string userdataBlock = getUserDataBlock();
    const std::string kF2fsStatsDir = kF2fsStatsPath + userdataBlock;

    if (!ReadFileToInt(kF2fsStatsDir + "/dirty_segments", &dirty)) {
        ALOGV("Unable to read dirty segments");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/free_segments", &free)) {
        ALOGV("Unable to read free segments");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/cp_foreground_calls", &cp_calls_fg)) {
        ALOGV("Unable to read cp_foreground_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/cp_background_calls", &cp_calls_bg)) {
        ALOGV("Unable to read cp_background_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/gc_foreground_calls", &gc_calls_fg)) {
        ALOGV("Unable to read gc_foreground_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/gc_background_calls", &gc_calls_bg)) {
        ALOGV("Unable to read gc_background_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/moved_blocks_foreground", &moved_block_fg)) {
        ALOGV("Unable to read moved_blocks_foreground");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/moved_blocks_background", &moved_block_bg)) {
        ALOGV("Unable to read moved_blocks_background");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/avg_vblocks", &vblocks)) {
        ALOGV("Unable to read avg_vblocks");
    }

    // Load values array
    std::vector<VendorAtomValue> values(9);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(dirty);
    values[F2fsStatsInfo::kDirtySegmentsFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(free);
    values[F2fsStatsInfo::kFreeSegmentsFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(cp_calls_fg);
    values[F2fsStatsInfo::kCpCallsFgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(cp_calls_bg);
    values[F2fsStatsInfo::kCpCallsBgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(gc_calls_fg);
    values[F2fsStatsInfo::kGcCallsFgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(gc_calls_bg);
    values[F2fsStatsInfo::kGcCallsBgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(moved_block_fg);
    values[F2fsStatsInfo::kMovedBlocksFgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(moved_block_bg);
    values[F2fsStatsInfo::kMovedBlocksBgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(vblocks);
    values[F2fsStatsInfo::kValidBlocksFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::F2FS_STATS,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report F2fs stats to Stats service");
    }
}

void SysfsCollector::reportZramMmStat(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (!kZramMmStatPath) {
        ALOGV("ZramMmStat path not specified");
        return;
    }

    if (!ReadFileToString(kZramMmStatPath, &file_contents)) {
        ALOGE("Unable to ZramMmStat %s - %s", kZramMmStatPath, strerror(errno));
        return;
    } else {
        int64_t orig_data_size = 0;
        int64_t compr_data_size = 0;
        int64_t mem_used_total = 0;
        int64_t mem_limit = 0;
        int64_t max_used_total = 0;
        int64_t same_pages = 0;
        int64_t pages_compacted = 0;
        int64_t huge_pages = 0;

        if (sscanf(file_contents.c_str(),
                "%" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64 \
                " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64,
                &orig_data_size, &compr_data_size, &mem_used_total,
                &mem_limit, &max_used_total, &same_pages,
                &pages_compacted, &huge_pages) != 8) {
            ALOGE("Unable to parse ZramMmStat %s from file %s to int.",
                    file_contents.c_str(), kZramMmStatPath);
        }

        // Load values array
        std::vector<VendorAtomValue> values(5);
        VendorAtomValue tmp;
        tmp.set<VendorAtomValue::intValue>(orig_data_size);
        values[ZramMmStat::kOrigDataSizeFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(compr_data_size);
        values[ZramMmStat::kComprDataSizeFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(mem_used_total);
        values[ZramMmStat::kMemUsedTotalFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(same_pages);
        values[ZramMmStat::kSamePagesFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(huge_pages);
        values[ZramMmStat::kHugePagesFieldNumber - kVendorAtomOffset] = tmp;

        // Send vendor atom to IStats HAL
        VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
            .atomId = PixelAtoms::Ids::ZRAM_MM_STAT,
            .values = values};
        const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
        if (!ret.isOk())
            ALOGE("Zram Unable to report ZramMmStat to Stats service");
    }
}

void SysfsCollector::reportZramBdStat(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (!kZramBdStatPath) {
        ALOGV("ZramBdStat path not specified");
        return;
    }

    if (!ReadFileToString(kZramBdStatPath, &file_contents)) {
        ALOGE("Unable to ZramBdStat %s - %s", kZramBdStatPath, strerror(errno));
        return;
    } else {
        int64_t bd_count = 0;
        int64_t bd_reads = 0;
        int64_t bd_writes = 0;

        if (sscanf(file_contents.c_str(), "%" SCNd64 " %" SCNd64 " %" SCNd64,
                                &bd_count, &bd_reads, &bd_writes) != 3) {
            ALOGE("Unable to parse ZramBdStat %s from file %s to int.",
                    file_contents.c_str(), kZramBdStatPath);
        }

        // Load values array
        std::vector<VendorAtomValue> values(3);
        VendorAtomValue tmp;
        tmp.set<VendorAtomValue::intValue>(bd_count);
        values[ZramBdStat::kBdCountFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(bd_reads);
        values[ZramBdStat::kBdReadsFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(bd_writes);
        values[ZramBdStat::kBdWritesFieldNumber - kVendorAtomOffset] = tmp;

        // Send vendor atom to IStats HAL
        VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
            .atomId = PixelAtoms::Ids::ZRAM_BD_STAT,
            .values = values};
        const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
        if (!ret.isOk())
            ALOGE("Zram Unable to report ZramBdStat to Stats service");
    }
}

void SysfsCollector::logZramStats(const std::shared_ptr<IStats> &stats_client) {
    reportZramMmStat(stats_client);
    reportZramBdStat(stats_client);
}

void SysfsCollector::logBootStats(const std::shared_ptr<IStats> &stats_client) {
    int mounted_time_sec = 0;

    if (kF2fsStatsPath == nullptr) {
        ALOGE("F2fs stats path not specified");
        return;
    }

    std::string userdataBlock = getUserDataBlock();

    if (!ReadFileToInt(kF2fsStatsPath + (userdataBlock + "/mounted_time_sec"), &mounted_time_sec)) {
        ALOGV("Unable to read mounted_time_sec");
        return;
    }

    int fsck_time_ms = android::base::GetIntProperty("ro.boottime.init.fsck.data", 0);
    int checkpoint_time_ms = android::base::GetIntProperty("ro.boottime.init.mount.data", 0);

    if (fsck_time_ms == 0 && checkpoint_time_ms == 0) {
        ALOGV("Not yet initialized");
        return;
    }

    // Load values array
    std::vector<VendorAtomValue> values(3);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(mounted_time_sec);
    values[BootStatsInfo::kMountedTimeSecFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(fsck_time_ms / 1000);
    values[BootStatsInfo::kFsckTimeSecFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(checkpoint_time_ms / 1000);
    values[BootStatsInfo::kCheckpointTimeSecFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::BOOT_STATS,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report Boot stats to Stats service");
    } else {
        log_once_reported = true;
    }
}

void SysfsCollector::logAll() {
    stats_ = android::frameworks::stats::V1_0::IStats::tryGetService();
    if (!stats_) {
        ALOGE("Unable to connect to Stats service");
    } else {
        logBatteryChargeCycles();
        logCodec1Failed();
        logCodecFailed();
        logSlowIO();
        logSpeechDspStat();
        stats_.clear();
    }

    const std::shared_ptr<IStats> stats_client = getStatsService();
    if (!stats_client) {
        ALOGE("Unable to get AIDL Stats service");
        return;
    }
    // Collect once per service init; can be multiple due to service reinit
    if (!log_once_reported) {
        logBootStats(stats_client);
    }
    logBatteryCapacity(stats_client);
    logBatteryEEPROM();
    logF2fsStats(stats_client);
    logSpeakerImpedance(stats_client);
    logUFSLifetime(stats_client);
    logUFSErrorStats(stats_client);
    logZramStats(stats_client);
}

/**
 * Loop forever collecting stats from sysfs nodes and reporting them via
 * IStats.
 */
void SysfsCollector::collect(void) {
    int timerfd = timerfd_create(CLOCK_BOOTTIME, 0);
    if (timerfd < 0) {
        ALOGE("Unable to create timerfd - %s", strerror(errno));
        return;
    }

    // Sleep for 30 seconds on launch to allow codec driver to load.
    sleep(30);

    // Collect first set of stats on boot.
    logAll();

    // Collect stats every 24hrs after.
    struct itimerspec period;
    const int kSecondsPerDay = 60 * 60 * 24;
    period.it_interval.tv_sec = kSecondsPerDay;
    period.it_interval.tv_nsec = 0;
    period.it_value.tv_sec = kSecondsPerDay;
    period.it_value.tv_nsec = 0;

    if (timerfd_settime(timerfd, 0, &period, NULL)) {
        ALOGE("Unable to set 24hr timer - %s", strerror(errno));
        return;
    }

    while (1) {
        int readval;
        do {
            char buf[8];
            errno = 0;
            readval = read(timerfd, buf, sizeof(buf));
        } while (readval < 0 && errno == EINTR);
        if (readval < 0) {
            ALOGE("Timerfd error - %s\n", strerror(errno));
            return;
        }
        logAll();
    }
}

}  // namespace pixel
}  // namespace google
}  // namespace hardware
}  // namespace android
