/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "android.hardware.powerstats-service.pixel"

#include <dataproviders/DisplayStateResidencyDataProvider.h>

#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace aidl {
namespace android {
namespace hardware {
namespace powerstats {

DisplayStateResidencyDataProvider::DisplayStateResidencyDataProvider(
        std::string name, std::string path, std::vector<std::string> states)
    : Thread(false),
      mPath(std::move(path)),
      mName(std::move(name)),
      mStates(states),
      mCurState(-1),
      mLooper(new ::android::Looper(true)) {
    // Construct mResidencies
    mResidencies.reserve(mStates.size());
    for (int32_t i = 0; i < mStates.size(); ++i) {
        PowerEntityStateResidencyData p = {.powerEntityStateId = i};
        mResidencies.emplace_back(p);
    }

    // Open display state file descriptor
    LOG(VERBOSE) << "Opening " << mPath;
    mFd = open(mPath.c_str(), O_RDONLY | O_NONBLOCK);
    if (mFd < 0) {
        PLOG(ERROR) << ":Failed to open file " << mPath;
        return;
    }

    // Add display state file descriptor to be polled by the looper
    mLooper->addFd(mFd, 0, ::android::Looper::EVENT_ERROR, nullptr, nullptr);

    // Run the thread that will poll for changes to display state
    LOG(VERBOSE) << "Starting DisplayStateWatcherThread";
    if (run("DisplayStateWatcherThread", ::android::PRIORITY_HIGHEST) != ::android::NO_ERROR) {
        LOG(ERROR) << "DisplayStateWatcherThread start fail";
    }
}

DisplayStateResidencyDataProvider::~DisplayStateResidencyDataProvider() {
    if (mFd >= 0) {
        close(mFd);
    }
}

bool DisplayStateResidencyDataProvider::getResults(
        std::unordered_map<std::string, std::vector<PowerEntityStateResidencyData>> *results) {
    std::scoped_lock lk(mLock);

    // Get current time since boot in milliseconds
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           ::android::base::boot_clock::now().time_since_epoch())
                           .count();

    // Construct residency result based on current residency data
    auto result = mResidencies;
    result[mCurState].totalTimeInStateMs += now - result[mCurState].lastEntryTimestampMs;
    results->emplace(mName, result);
    return true;
}

std::unordered_map<std::string, std::vector<PowerEntityStateInfo>>
DisplayStateResidencyDataProvider::getInfo() {
    std::vector<PowerEntityStateInfo> stateInfos;
    stateInfos.reserve(mStates.size());
    for (int32_t i = 0; i < mStates.size(); ++i) {
        stateInfos.push_back({.powerEntityStateId = i, .powerEntityStateName = mStates[i]});
    }

    return {{mName, stateInfos}};
}

// Called when there is new data to be read from
// display state file descriptor indicating a state change
void DisplayStateResidencyDataProvider::updateStats() {
    char data[32];

    // Get current time since boot in milliseconds
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           ::android::base::boot_clock::now().time_since_epoch())
                           .count();
    // Read display state
    ssize_t ret = pread(mFd, data, sizeof(data) - 1, 0);
    if (ret < 0) {
        PLOG(ERROR) << "Failed to read display state";
        return;
    }
    data[ret] = '\0';

    LOG(VERBOSE) << "display state: " << data;

    // Update residency stats based on state read
    {  // acquire lock
        std::scoped_lock lk(mLock);
        for (uint32_t i = 0; i < mStates.size(); ++i) {
            if (strstr(data, mStates[i].c_str())) {
                // Update total time of the previous state
                if (mCurState > -1) {
                    mResidencies[mCurState].totalTimeInStateMs +=
                            now - mResidencies[mCurState].lastEntryTimestampMs;
                }

                // Set current state
                mCurState = i;
                mResidencies[i].totalStateEntryCount++;
                mResidencies[i].lastEntryTimestampMs = now;
                break;
            }
        }
    }  // release lock
}

bool DisplayStateResidencyDataProvider::threadLoop() {
    LOG(VERBOSE) << "DisplayStateResidencyDataProvider polling...";

    // Poll for display state changes. Timeout set to poll indefinitely
    if (mLooper->pollOnce(-1) >= 0) {
        updateStats();
    }

    return true;
}

}  // namespace powerstats
}  // namespace hardware
}  // namespace android
}  // namespace aidl
