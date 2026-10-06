#include "Filesystem.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <climits>
#include <exception>
#ifdef _WIN32
// NOMINMAX keeps <windows.h> from defining min/max macros that break
// std::min / std::max in any code that includes this file's headers later.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Platform {
    namespace Filesystem {

        namespace fs = std::filesystem;

        // =========================================================
        // Path encoding
        // =========================================================

        // Every std::string path of this API is UTF-8 (see Filesystem.hpp).
        // Passing a std::string straight to std::filesystem would make MSVC
        // read it in the ANSI code page, and path::string() would write it
        // back in that code page, throwing std::system_error for any
        // character the code page cannot represent. So all conversions go
        // through To_path / To_utf8 below, and neither of them throws.
        // On POSIX, paths are plain bytes and are passed through unchanged.
#ifdef _WIN32
        // UTF-16 -> UTF-8. Lone surrogates become U+FFFD instead of failing.
        static std::string Wide_to_utf8(const wchar_t* _wide, size_t _length) {
            if (_length == 0 || _length > static_cast<size_t>(INT_MAX)) return {};

            const int size = WideCharToMultiByte(CP_UTF8, 0, _wide, static_cast<int>(_length),
                nullptr, 0, nullptr, nullptr);
            if (size <= 0) return {};

            std::string utf8(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, _wide, static_cast<int>(_length),
                utf8.data(), size, nullptr, nullptr);
            return utf8;
        }

        // UTF-8 -> UTF-16. Text that is not valid UTF-8 is read in the system
        // ANSI code page instead (what argv[] and legacy callers hold).
        static std::wstring Utf8_to_wide(const std::string& _utf8) {
            if (_utf8.empty() || _utf8.size() > static_cast<size_t>(INT_MAX)) return {};

            const int length = static_cast<int>(_utf8.size());
            UINT code_page = CP_UTF8;
            DWORD flags = MB_ERR_INVALID_CHARS;

            int size = MultiByteToWideChar(code_page, flags, _utf8.data(), length, nullptr, 0);
            if (size <= 0) {
                code_page = CP_ACP;
                flags = 0;
                size = MultiByteToWideChar(code_page, flags, _utf8.data(), length, nullptr, 0);
                if (size <= 0) return {};
            }

            std::wstring wide(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(code_page, flags, _utf8.data(), length, wide.data(), size);
            return wide;
        }

        static fs::path To_path(const std::string& _utf8) {
            return fs::path(Utf8_to_wide(_utf8));
        }

        static std::string To_utf8(const fs::path& _path) {
            const std::wstring& wide = _path.native();
            return Wide_to_utf8(wide.data(), wide.size());
        }
#else
        static fs::path To_path(const std::string& _utf8) {
            return fs::path(_utf8);
        }

        static std::string To_utf8(const fs::path& _path) {
            return _path.native();
        }
#endif

        // =========================================================
        // Existence and type checks
        // =========================================================

        // For these three, a path that does not exist is NOT an error:
        // std::filesystem leaves the error_code clear and the answer is
        // simply false. A set error_code therefore means the answer could not
        // be determined at all (access denied, I/O error...).

        bool Exists(const std::string& _path, std::error_code& _error) {
            return fs::exists(To_path(_path), _error);
        }

        bool Exists(const std::string& _path) {
            std::error_code error_code;
            return Exists(_path, error_code);
        }

        bool Is_file(const std::string& _path, std::error_code& _error) {
            return fs::is_regular_file(To_path(_path), _error);
        }

        bool Is_file(const std::string& _path) {
            std::error_code error_code;
            return Is_file(_path, error_code);
        }

        bool Is_directory(const std::string& _path, std::error_code& _error) {
            return fs::is_directory(To_path(_path), _error);
        }

        bool Is_directory(const std::string& _path) {
            std::error_code error_code;
            return Is_directory(_path, error_code);
        }

        // =========================================================
        // Reading files
        // =========================================================

        // Shared body of Read_text_file / Read_binary_file. Buffer is
        // std::string or std::vector<uint8_t>; _mode is std::ios::binary or
        // none.
        //
        // The whole stream is consumed with read(), never through an
        // istreambuf_iterator: the iterator talks to the stream buffer
        // directly, so an I/O error half way through just looks like the end
        // of the file and the truncated content is returned as if complete.
        // read() records the failure as badbit, which is checked at the end.
        //
        // Special files (a directory opened on POSIX, a device) also end in
        // badbit and are reported as a failure instead of an empty file.
        template <typename Buffer>
        static std::optional<Buffer> Read_whole_file(const std::string& _path, std::ios::openmode _mode) {
            constexpr size_t min_capacity = 4096;

            try {
                const fs::path path = To_path(_path);

                std::ifstream file(path, std::ios::in | _mode);
                if (!file.is_open()) {
                    return std::nullopt;
                }

                // The size on disk is only a hint to allocate once. It is not
                // trusted: the file can change while it is read, a text-mode
                // read can produce fewer bytes than the file has, and
                // special files report 0. One spare byte lets the read
                // that reaches the end of file finish inside the first pass.
                std::error_code size_error;
                const uintmax_t size_hint = fs::file_size(path, size_error);

                Buffer buffer;
                buffer.resize(size_error ? min_capacity
                                         : std::max<size_t>(static_cast<size_t>(size_hint) + 1, min_capacity));

                size_t used = 0;
                for (;;) {
                    file.read(reinterpret_cast<char*>(buffer.data()) + used,
                        static_cast<std::streamsize>(buffer.size() - used));
                    used += static_cast<size_t>(file.gcount());

                    if (!file) break;                    // end of file, or an error
                    buffer.resize(buffer.size() * 2);    // buffer filled: the file is bigger than the hint
                }

                // A short read leaves eofbit set; anything else (badbit, or
                // failbit without reaching the end) is a real failure.
                if (file.bad() || !file.eof()) {
                    return std::nullopt;
                }

                buffer.resize(used);
                return buffer;
            }
            catch (const std::exception&) {
                // Out of memory for a huge file, or a conversion failure:
                // the API promises not to throw.
                return std::nullopt;
            }
        }

        std::optional<std::string> Read_text_file(const std::string& _path) {
            return Read_whole_file<std::string>(_path, std::ios::openmode{});
        }

        std::optional<std::vector<uint8_t>> Read_binary_file(const std::string& _path) {
            return Read_whole_file<std::vector<uint8_t>>(_path, std::ios::binary);
        }

        // =========================================================
        // Writing files
        // =========================================================

        // Shared body of Write_text_file / Write_binary_file: creates the
        // missing parent directories, truncates/creates _path with the given
        // extra open mode (std::ios::binary or none) and writes _size bytes.
        // _kind ("text" / "binary") only feeds the error message.
        //
        // write() only fills the stream's buffer, so a full disk or a network
        // share that went away is not seen until the buffer is flushed to the
        // OS and the file is closed. Both are done explicitly and checked:
        // good() right after write() would report success for data that
        // never reached the file.
        static bool Write_file(const std::string& _path, std::ios::openmode _extra_mode,
                               const char* _data, std::streamsize _size, const char* _kind) {
            const fs::path file_path = To_path(_path);
            if (file_path.has_parent_path()) {
                std::error_code directory_error;
                fs::create_directories(file_path.parent_path(), directory_error);
            }

            bool opened = false;
            bool written = false;
            try {
                std::ofstream file(file_path, std::ios::out | std::ios::trunc | _extra_mode);
                opened = file.is_open();
                if (opened) {
                    file.write(_data, _size);
                    file.flush();
                    file.close();            // close() reports a failure of the final flush
                    written = !file.fail();
                }
            }
            catch (const std::exception&) {
                written = false;
            }

            if (!written) {
                std::cerr << "[Filesystem] Failed to write " << _kind << " file: " << _path << "\n";

                // The open truncated the file, so it now holds a prefix of the
                // new content (or nothing). Leaving it would let a later
                // reader take it for a complete file. Only plain files are
                // touched: never delete a device or a directory.
                std::error_code cleanup_error;
                if (opened && fs::is_regular_file(file_path, cleanup_error)) {
                    fs::remove(file_path, cleanup_error);
                }
            }

            return written;
        }

        bool Write_text_file(const std::string& _path, const std::string& _content) {
            return Write_file(_path, std::ios::openmode{},
                _content.data(), static_cast<std::streamsize>(_content.size()), "text");
        }

        bool Write_binary_file(const std::string& _path, const std::vector<uint8_t>& _data) {
            return Write_file(_path, std::ios::binary,
                reinterpret_cast<const char*>(_data.data()), static_cast<std::streamsize>(_data.size()), "binary");
        }

        // =========================================================
        // Executable and working directory
        // =========================================================

        std::string Get_executable_path() {
#ifdef _WIN32
            // GetModuleFileNameW never fails because the buffer is too small:
            // it fills the buffer with a truncated path and returns its size.
            // A result equal to the buffer size therefore means "cut off",
            // and the call is repeated with a bigger buffer. 32768 characters
            // is the longest path Windows supports.
            constexpr size_t max_buffer_size = 32768;

            std::wstring buffer(MAX_PATH, L'\0');
            for (;;) {
                const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0) {
                    std::cerr << "[Filesystem] Failed to get executable path (error "
                        << GetLastError() << ")\n";
                    return "";
                }

                if (length < buffer.size()) {
                    return Wide_to_utf8(buffer.data(), length);
                }

                if (buffer.size() >= max_buffer_size) {
                    std::cerr << "[Filesystem] Executable path is longer than "
                        << max_buffer_size << " characters\n";
                    return "";
                }
                buffer.resize(buffer.size() * 2);
            }
#else
            std::cerr << "[Filesystem] Get_executable_path not implemented on this platform\n";
            return "";
#endif
        }

        std::string Get_executable_directory() {
            const std::string exe_path = Get_executable_path();
            if (exe_path.empty()) return "";

            return To_utf8(To_path(exe_path).parent_path());
        }

        std::string Get_current_directory() {
            std::error_code error_code;
            fs::path current = fs::current_path(error_code);
            if (error_code) return "";
            return To_utf8(current);
        }

        bool Set_current_directory(const std::string& _path) {
            std::error_code error_code;
            fs::current_path(To_path(_path), error_code);
            return !error_code;
        }

        // =========================================================
        // Directory listing
        // =========================================================

        // Shared body of the List_* functions. Iterator is
        // fs::directory_iterator or fs::recursive_directory_iterator, and
        // _accept(entry) decides which entries are collected.
        //
        // Nothing here throws on filesystem errors. A range-for over the
        // iterator would call the throwing operator++, and the throwing
        // directory_entry queries, so a file deleted mid-listing or an
        // unreadable folder would escape as std::filesystem::filesystem_error.
        // Instead the walk uses increment(error_code).
        //
        // An error that stops the walk makes the whole listing fail
        // (std::nullopt): a partial list is indistinguishable from a
        // complete one, so it is never returned. What does not stop it:
        //   - subfolders the process may not enter are skipped, through
        //     skip_permission_denied, so one protected folder does not
        //     make a recursive listing useless;
        //   - an entry whose type cannot be read (_accept sees the error
        //     and says no) is left out, so one broken symlink does not
        //     make the folder it lives in unlistable.
        template <typename Iterator, typename Predicate>
        static std::optional<std::vector<std::string>> List_entries(const std::string& _directory, Predicate _accept) {
            const fs::path directory = To_path(_directory);

            try {
                std::error_code error_code;
                if (!fs::is_directory(directory, error_code)) {
                    return std::nullopt;
                }

                // skip_permission_denied would also turn an unreadable
                // top-level folder into an empty listing with no error. A
                // plain open first makes that case a failure.
                fs::directory_iterator probe(directory, error_code);
                if (error_code) {
                    return std::nullopt;
                }

                Iterator it(directory, fs::directory_options::skip_permission_denied, error_code);
                if (error_code) {
                    return std::nullopt;
                }

                std::vector<std::string> result;
                const Iterator end{};

                while (it != end) {
                    if (_accept(*it)) {
                        result.push_back(To_utf8(it->path()));
                    }

                    it.increment(error_code);
                    if (error_code) {
                        return std::nullopt;
                    }
                }

                return result;
            }
            catch (const std::exception&) {
                // Out of memory while collecting, or a path conversion failure.
                return std::nullopt;
            }
        }

        // True for regular files whose extension matches _extension_filter
        // (an empty filter matches every regular file).
        static bool Is_matching_file(const fs::directory_entry& _entry, const std::string& _extension_filter) {
            std::error_code error_code;
            if (!_entry.is_regular_file(error_code)) return false;

            return _extension_filter.empty() || To_utf8(_entry.path().extension()) == _extension_filter;
        }

        std::optional<std::vector<std::string>> List_files(const std::string& _directory, const std::string& _extension_filter) {
            return List_entries<fs::directory_iterator>(_directory, [&](const fs::directory_entry& _entry) {
                return Is_matching_file(_entry, _extension_filter);
            });
        }

        std::optional<std::vector<std::string>> List_files_recursive(const std::string& _directory, const std::string& _extension_filter) {
            return List_entries<fs::recursive_directory_iterator>(_directory, [&](const fs::directory_entry& _entry) {
                return Is_matching_file(_entry, _extension_filter);
            });
        }

        std::optional<std::vector<std::string>> List_directories(const std::string& _directory) {
            return List_entries<fs::directory_iterator>(_directory, [](const fs::directory_entry& _entry) {
                std::error_code error_code;
                return _entry.is_directory(error_code);
            });
        }

        // =========================================================
        // Path manipulation
        // =========================================================

        std::string Get_extension(const std::string& _path) {
            return To_utf8(To_path(_path).extension());
        }

        std::string Get_filename(const std::string& _path) {
            return To_utf8(To_path(_path).filename());
        }

        std::string Get_filename_without_extension(const std::string& _path) {
            return To_utf8(To_path(_path).stem());
        }

        std::string Get_parent_directory(const std::string& _path) {
            return To_utf8(To_path(_path).parent_path());
        }

        std::string Combine_path(const std::string& _a, const std::string& _b) {
            fs::path combined = To_path(_a) / To_path(_b);
            return To_utf8(combined);
        }

        std::string Normalize_path(const std::string& _path) {
            const fs::path path = To_path(_path);

            std::error_code error_code;
            fs::path normalized = fs::weakly_canonical(path, error_code);
            if (error_code) {
                // weakly_canonical fails if intermediate parts don't exist yet;
                // fall back to lexical normalization, which works on paths
                // that don't exist on disk (e.g. a path you're about to create).
                return To_utf8(path.lexically_normal());
            }
            return To_utf8(normalized);
        }

        bool Is_absolute(const std::string& _path) {
            return To_path(_path).is_absolute();
        }

        // =========================================================
        // Directory and file management
        // =========================================================

        bool Create_directory(const std::string& _path) {
            std::error_code error_code;
            fs::create_directories(To_path(_path), error_code);
            return !error_code;
        }

        bool Delete_file(const std::string& _path) {
            std::error_code error_code;
            return fs::remove(To_path(_path), error_code);
        }

        bool Delete_directory(const std::string& _path) {
            std::error_code error_code;
            fs::remove_all(To_path(_path), error_code);
            return !error_code;
        }

        bool Copy_file(const std::string& _source, const std::string& _destination) {
            std::error_code error_code;
            fs::copy_file(To_path(_source), To_path(_destination), fs::copy_options::overwrite_existing, error_code);
            return !error_code;
        }

        std::optional<uint64_t> Get_file_size(const std::string& _path) {
            std::error_code error_code;
            const uintmax_t size = fs::file_size(To_path(_path), error_code);
            if (error_code) return std::nullopt;
            return static_cast<uint64_t>(size);
        }

        std::optional<int64_t> Get_last_write_time(const std::string& _path) {
            std::error_code error_code;
            auto file_time = fs::last_write_time(To_path(_path), error_code);
            if (error_code) return std::nullopt;

            // C++20 only requires file_clock to expose one of to_sys/from_sys or
            // to_utc/from_utc, so there is no single portable conversion here.
#ifdef _MSC_VER
            // The MSVC file clock is a raw FILETIME: 100 ns ticks counted from
            // 1601-01-01, which the fixed epoch delta turns into Unix seconds.
            // clock_cast would compile, but it routes through utc_clock and so
            // pulls the leap second table out of the time zone database at run
            // time, which also shifts the result off the usual Unix epoch.
            static_assert(std::ratio_equal<fs::file_time_type::period, std::ratio<1, 10000000>>::value,
                "Expected the MSVC file clock to tick in 100 ns units");
            constexpr int64_t ticks_per_second = 10000000;
            constexpr int64_t seconds_from_1601_to_1970 = 11644473600;
            const int64_t ticks = static_cast<int64_t>(file_time.time_since_epoch().count());
            return ticks / ticks_per_second - seconds_from_1601_to_1970;
#else
            // libstdc++ and libc++ ship to_sys, and it needs no time zone data.
            auto system_time = std::chrono::file_clock::to_sys(file_time);
            return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
#endif
        }

    }
}