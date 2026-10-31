// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/assert.h"
#include "common/fs/fs_android.h"
#include "common/string_util.h"

namespace Common::FS::Android {

void RegisterCallbacks(JNIEnv* env, jclass clazz) {
    env->GetJavaVM(&g_jvm);
    native_library = clazz;

    s_get_parent_directory = env->GetStaticMethodID(native_library, "getParentDirectory",
                                                    "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_filename = env->GetStaticMethodID(native_library, "getFilename",
                                            "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_size = env->GetStaticMethodID(native_library, "getSize", "(Ljava/lang/String;)J");
    s_is_directory = env->GetStaticMethodID(native_library, "isDirectory", "(Ljava/lang/String;)Z");
    s_file_exists = env->GetStaticMethodID(native_library, "exists", "(Ljava/lang/String;)Z");
    s_open_content_uri = env->GetStaticMethodID(native_library, "openContentUri",
                                                "(Ljava/lang/String;Ljava/lang/String;)I");
    s_list_children = env->GetStaticMethodID(native_library, "listChildren",
                                             "(Ljava/lang/String;)[Ljava/lang/String;");
}

void UnRegisterCallbacks() {
    s_get_parent_directory = nullptr;
    s_get_filename = nullptr;

    s_get_size = nullptr;
    s_is_directory = nullptr;
    s_file_exists = nullptr;

    s_open_content_uri = nullptr;
    s_list_children = nullptr;
}

bool IsContentUri(const std::string& path) {
    constexpr std::string_view prefix = "content://";
    if (path.size() < prefix.size()) [[unlikely]] {
        return false;
    }

    return path.find(prefix) == 0;
}

s32 OpenContentUri(const std::string& filepath, OpenMode openmode) {
    if (s_open_content_uri == nullptr)
        return -1;

    const char* mode = "";
    switch (openmode) {
    case OpenMode::Read:
        mode = "r";
        break;
    default:
        UNIMPLEMENTED();
        return -1;
    }
    auto env = Common::Android::GetEnvForThread();
    jstring j_filepath = Common::Android::ToJString(env, filepath);
    jstring j_mode = Common::Android::ToJString(env, mode);
    return env->CallStaticIntMethod(native_library, s_open_content_uri, j_filepath, j_mode);
}

u64 GetSize(const std::string& filepath) {
    if (s_get_size == nullptr) {
        return 0;
    }
    auto env = Common::Android::GetEnvForThread();
    return static_cast<u64>(env->CallStaticLongMethod(
        native_library, s_get_size,
        Common::Android::ToJString(Common::Android::GetEnvForThread(), filepath)));
}

bool IsDirectory(const std::string& filepath) {
    if (s_is_directory == nullptr) {
        return 0;
    }
    auto env = Common::Android::GetEnvForThread();
    return env->CallStaticBooleanMethod(
        native_library, s_is_directory,
        Common::Android::ToJString(Common::Android::GetEnvForThread(), filepath));
}

bool Exists(const std::string& filepath) {
    if (s_file_exists == nullptr) {
        return 0;
    }
    auto env = Common::Android::GetEnvForThread();
    return env->CallStaticBooleanMethod(
        native_library, s_file_exists,
        Common::Android::ToJString(Common::Android::GetEnvForThread(), filepath));
}

std::string GetParentDirectory(const std::string& filepath) {
    if (s_get_parent_directory == nullptr) {
        return {};
    }
    auto env = Common::Android::GetEnvForThread();
    jstring j_return = static_cast<jstring>(env->CallStaticObjectMethod(
        native_library, s_get_parent_directory, Common::Android::ToJString(env, filepath)));
    if (!j_return) {
        return {};
    }
    return Common::Android::GetJString(env, j_return);
}

std::string GetFilename(const std::string& filepath) {
    if (s_get_filename == nullptr) {
        return {};
    }
    auto env = Common::Android::GetEnvForThread();
    jstring j_return = static_cast<jstring>(env->CallStaticObjectMethod(
        native_library, s_get_filename, Common::Android::ToJString(env, filepath)));
    if (!j_return) {
        return {};
    }
    return Common::Android::GetJString(env, j_return);
}

std::vector<ContentEntry> ListChildren(const std::string& dir_path) {
    if (s_list_children == nullptr) {
        return {};
    }
    auto env = Common::Android::GetEnvForThread();
    jstring j_path = Common::Android::ToJString(env, dir_path);
    auto j_entries = static_cast<jobjectArray>(
        env->CallStaticObjectMethod(native_library, s_list_children, j_path));
    env->DeleteLocalRef(j_path);
    if (!j_entries) {
        return {};
    }

    // Four strings per child: "d" or "f", size, display name, document URI. A big directory
    // makes thousands of local references, so each one is dropped once read.
    const auto element = [&](jsize index) {
        auto j_value = static_cast<jstring>(env->GetObjectArrayElement(j_entries, index));
        std::string value = j_value ? Common::Android::GetJString(env, j_value) : std::string{};
        env->DeleteLocalRef(j_value);
        return value;
    };
    const jsize count = env->GetArrayLength(j_entries);
    std::vector<ContentEntry> entries;
    entries.reserve(static_cast<size_t>(count / 4));
    for (jsize i = 0; i + 3 < count; i += 4) {
        ContentEntry entry;
        entry.is_directory = element(i) == "d";
        entry.size = std::strtoll(element(i + 1).c_str(), nullptr, 10);
        entry.name = element(i + 2);
        entry.uri = element(i + 3);
        entries.push_back(std::move(entry));
    }
    env->DeleteLocalRef(j_entries);
    return entries;
}

} // namespace Common::FS::Android
