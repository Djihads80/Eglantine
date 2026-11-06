// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>
#include <jni.h>

namespace Common::FS::Android {

static JavaVM* g_jvm = nullptr;
static jclass native_library = nullptr;

static jmethodID s_get_parent_directory;
static jmethodID s_get_filename;
static jmethodID s_get_size;
static jmethodID s_is_directory;
static jmethodID s_file_exists;
static jmethodID s_open_content_uri;
static jmethodID s_list_children;

// A child of a content URI directory, as listed by the document provider.
struct ContentEntry {
    std::string name;
    std::string uri;
    bool is_directory;
    std::int64_t size; // -1 when the provider does not report it
};

enum class OpenMode {
    Read,
    Write,
    ReadWrite,
    WriteAppend,
    WriteTruncate,
    ReadWriteAppend,
    ReadWriteTruncate,
    Never
};

void RegisterCallbacks(JNIEnv* env, jclass clazz);

void UnRegisterCallbacks();

bool IsContentUri(const std::string& path);

int OpenContentUri(const std::string& filepath, OpenMode openmode);
std::uint64_t GetSize(const std::string& filepath);
bool IsDirectory(const std::string& filepath);
bool Exists(const std::string& filepath);
std::string GetParentDirectory(const std::string& filepath);
std::string GetFilename(const std::string& filepath);
std::vector<ContentEntry> ListChildren(const std::string& dir_path);

} // namespace Common::FS::Android
