#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace Platform {

    // Filesystem: stateless utility functions for reading/writing files,
    // checking paths, and navigating directories. Wraps std::filesystem
    // with a simpler, engine-friendly API and consistent error handling.
    //
    // Failure reporting: nothing here throws. A function that can fail
    // returns std::optional (std::nullopt = failure) or bool (false =
    // failure), so a failure is never confused with a legitimate result such
    // as an empty file, a size of 0 or an empty directory. The few predicates
    // that stay plain bool (Exists, Is_file, Is_directory) also have an
    // overload that reports the underlying std::error_code.
    //
    // Path encoding: every std::string path that goes in or out of this API
    // is UTF-8, on every platform. On Windows the strings are converted to and
    // from the native UTF-16 paths internally, so file names with accents,
    // CJK or emoji characters work regardless of the system code page and are
    // never mangled into '?'. A string that is not valid UTF-8 is read as
    // being in the system ANSI code page instead (what argv[] holds), so
    // legacy callers keep working. Code that hands these paths to a third
    // party library (fopen, stbi_load...) needs a UTF-8 aware entry point on
    // Windows; the std::string overloads of the C runtime are not.
    namespace Filesystem {

        // =========================================================
        // Existence and type checks
        // =========================================================

        // Returns true if a file or directory exists at the given path.
        // Also false when that cannot be determined (e.g. access denied on a
        // parent directory): use the overload below to tell the two apart.
        bool Exists(const std::string& _path);

        // Same, but _error receives the reason when the answer could not be
        // determined, and is cleared otherwise. false with an empty _error
        // means the path really does not exist.
        bool Exists(const std::string& _path, std::error_code& _error);

        // Returns true if the path exists and is a regular file
        bool Is_file(const std::string& _path);
        bool Is_file(const std::string& _path, std::error_code& _error);

        // Returns true if the path exists and is a directory
        bool Is_directory(const std::string& _path);
        bool Is_directory(const std::string& _path, std::error_code& _error);

        // =========================================================
        // Reading files
        // =========================================================

        // Reads an entire text file into a string. Returns std::nullopt if
        // the file doesn't exist, can't be opened, is not a readable file
        // (e.g. a directory) or the read fails half way (I/O error): a
        // truncated string is never returned as if it were the whole file.
        // An empty file returns "". Nothing is logged: the caller decides how
        // to report the failure.
        std::optional<std::string> Read_text_file(const std::string& _path);

        // Reads an entire binary file into a byte buffer.
        // This is what you'll use to load compiled SPIR-V shaders (.spv),
        // since they must be read as raw bytes, not text.
        // Returns std::nullopt on any failure (same cases as Read_text_file),
        // so an empty file (a valid, empty buffer) can be told apart from a
        // file that could not be read. Nothing is logged.
        std::optional<std::vector<uint8_t>> Read_binary_file(const std::string& _path);

        // =========================================================
        // Writing files
        // =========================================================

        // Writes a string to a file, overwriting it if it already exists.
        // Creates parent directories if needed. Returns true only once the
        // data has been flushed and the file closed without error (a full
        // disk, for instance, is reported by the flush, not by the write).
        // On failure the error is logged, and a file that was opened but not
        // completely written is deleted rather than left truncated.
        bool Write_text_file(const std::string& _path, const std::string& _content);

        // Writes raw bytes to a file, overwriting it if it already exists.
        // Same guarantees as Write_text_file.
        bool Write_binary_file(const std::string& _path, const std::vector<uint8_t>& _data);

        // =========================================================
        // Executable and working directory
        // =========================================================

        // Returns the full path to the currently running executable, or ""
        // if it cannot be determined (logged). Useful for building asset
        // paths relative to where the engine actually lives, instead of
        // relying on the current working directory (which can vary depending
        // on how the app was launched).
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

        // The List_* functions return std::nullopt when the listing could not
        // be completed: _directory is not a readable directory, or reading it
        // failed part way. They never return a silently truncated list, so an
        // empty list always means "nothing matched". Two things are skipped
        // on purpose and do not count as a failure: subdirectories the
        // process is not allowed to enter (recursive listing), and entries
        // that cannot be inspected at all (a symlink loop, say).

        // Lists all files directly inside a directory (non-recursive).
        // If _extension_filter is non-empty (e.g. ".spv"), only files
        // with that extension are returned.
        std::optional<std::vector<std::string>> List_files(const std::string& _directory, const std::string& _extension_filter = "");

        // Same as List_files but recurses into subdirectories as well
        std::optional<std::vector<std::string>> List_files_recursive(const std::string& _directory, const std::string& _extension_filter = "");

        // Lists all subdirectories directly inside a directory (non-recursive)
        std::optional<std::vector<std::string>> List_directories(const std::string& _directory);

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

        // Returns the size of a file in bytes. Returns std::nullopt if the
        // file doesn't exist, is not a regular file or can't be queried; an
        // empty file returns 0.
        std::optional<uint64_t> Get_file_size(const std::string& _path);

        // Returns the last modification time of a file as a Unix timestamp
        // (seconds since epoch), or std::nullopt if it can't be queried.
        // Useful for hot-reloading systems that need to detect when an asset
        // file has changed on disk (a failed query is not a timestamp of 0,
        // which would look like "changed" or "unchanged" by accident).
        std::optional<int64_t> Get_last_write_time(const std::string& _path);

    }

}