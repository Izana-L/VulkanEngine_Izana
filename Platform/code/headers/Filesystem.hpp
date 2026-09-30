#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace Platform {

    // Filesystem: stateless utility functions for reading/writing files,
    // checking paths, and navigating directories. Wraps std::filesystem
    // with a simpler, engine-friendly API and consistent error handling
    // (returns empty/false on failure instead of throwing, except where noted).
    namespace Filesystem {

        // =========================================================
        // Existence and type checks
        // =========================================================

        // Returns true if a file or directory exists at the given path
        bool Exists(const std::string& _path);

        // Returns true if the path exists and is a regular file
        bool Is_file(const std::string& _path);

        // Returns true if the path exists and is a directory
        bool Is_directory(const std::string& _path);

        // =========================================================
        // Reading files
        // =========================================================

        // Reads an entire text file into a string. Returns an empty string
        // and logs an error if the file doesn't exist or can't be opened.
        // Use Exists() first if you need to distinguish "empty file" from "failed to read".
        std::string Read_text_file(const std::string& _path);

        // Reads an entire binary file into a byte buffer.
        // This is what you'll use to load compiled SPIR-V shaders (.spv),
        // since they must be read as raw bytes, not text.
        std::vector<uint8_t> Read_binary_file(const std::string& _path);

        // =========================================================
        // Writing files
        // =========================================================

        // Writes a string to a file, overwriting it if it already exists.
        // Returns true on success. Creates parent directories if needed.
        bool Write_text_file(const std::string& _path, const std::string& _content);

        // Writes raw bytes to a file, overwriting it if it already exists.
        // Returns true on success. Creates parent directories if needed.
        bool Write_binary_file(const std::string& _path, const std::vector<uint8_t>& _data);

        // =========================================================
        // Executable and working directory
        // =========================================================

        // Returns the full path to the currently running executable.
        // Useful for building asset paths relative to where the engine
        // actually lives, instead of relying on the current working
        // directory (which can vary depending on how the app was launched).
        std::string Get_executable_path();

        // Returns the directory containing the currently running executable
        // (i.e. Get_executable_path() without the filename).
        std::string Get_executable_directory();

        // Returns the current working directory of the process
        std::string Get_current_directory();

        // Changes the current working directory of the process
        bool Set_current_directory(const std::string& _path);

        // =========================================================
        // Directory listing
        // =========================================================

        // Lists all files directly inside a directory (non-recursive).
        // If _extension_filter is non-empty (e.g. ".spv"), only files
        // with that extension are returned.
        std::vector<std::string> List_files(const std::string& _directory, const std::string& _extension_filter = "");

        // Same as List_files but recurses into subdirectories as well
        std::vector<std::string> List_files_recursive(const std::string& _directory, const std::string& _extension_filter = "");

        // Lists all subdirectories directly inside a directory (non-recursive)
        std::vector<std::string> List_directories(const std::string& _directory);

        // =========================================================
        // Path manipulation
        // =========================================================

        // Returns the file extension, including the dot (e.g. ".png").
        // Returns an empty string if the path has no extension.
        std::string Get_extension(const std::string& _path);

        // Returns the filename including extension (e.g. "texture.png"
        // from "C:/assets/textures/texture.png")
        std::string Get_filename(const std::string& _path);

        // Returns the filename without extension (e.g. "texture")
        std::string Get_filename_without_extension(const std::string& _path);

        // Returns the parent directory of the given path
        std::string Get_parent_directory(const std::string& _path);

        // Joins two path segments with the correct platform-specific
        // separator (e.g. Combine_path("assets", "textures") -> "assets/textures")
        std::string Combine_path(const std::string& _a, const std::string& _b);

        // Normalizes a path: resolves "..", ".", and converts to a
        // consistent separator style. Useful before comparing two paths
        // for equality, or displaying a clean path in logs.
        std::string Normalize_path(const std::string& _path);

        // Returns true if _path is an absolute path (e.g. "C:/..." or "/...")
        bool Is_absolute(const std::string& _path);

        // =========================================================
        // Directory and file management
        // =========================================================

        // Creates a directory, including any missing parent directories.
        // Returns true if the directory exists after the call (whether it
        // was just created or already existed).
        bool Create_directory(const std::string& _path);

        // Deletes a single file. Returns true on success.
        bool Delete_file(const std::string& _path);

        // Deletes a directory and everything inside it. Returns true on success.
        // USE WITH CAUTION - this is recursive and irreversible.
        bool Delete_directory(const std::string& _path);

        // Copies a file from source to destination. Returns true on success.
        // Overwrites the destination if it already exists.
        bool Copy_file(const std::string& _source, const std::string& _destination);

        // Returns the size of a file in bytes. Returns 0 if the file
        // doesn't exist (use Exists() first to distinguish from an empty file).
        uint64_t Get_file_size(const std::string& _path);

        // Returns the last modification time of a file as a Unix timestamp
        // (seconds since epoch). Useful for hot-reloading systems that
        // need to detect when an asset file has changed on disk.
        int64_t Get_last_write_time(const std::string& _path);

    }

}