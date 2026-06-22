#include "Filesystem.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Platform {
    namespace Filesystem {

        namespace fs = std::filesystem;

        // =========================================================
        // Existence and type checks
        // =========================================================

        bool Exists(const std::string& _path) {
            std::error_code error_code;
            return fs::exists(_path, error_code);
        }

        bool Is_file(const std::string& _path) {
            std::error_code error_code;
            return fs::is_regular_file(_path, error_code);
        }

        bool Is_directory(const std::string& _path) {
            std::error_code error_code;
            return fs::is_directory(_path, error_code);
        }

        // =========================================================
        // Reading files
        // =========================================================

        std::string Read_text_file(const std::string& _path) {
            std::ifstream file(_path, std::ios::in);
            if (!file.is_open()) {
                std::cerr << "[Filesystem] Failed to open text file: " << _path << "\n";
                return "";
            }

            std::string content(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>()
            );

            return content;
        }

        std::vector<uint8_t> Read_binary_file(const std::string& _path) {
            std::ifstream file(_path, std::ios::in | std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                std::cerr << "[Filesystem] Failed to open binary file: " << _path << "\n";
                return {};
            }

            std::streamsize file_size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
            if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
                std::cerr << "[Filesystem] Failed to read binary file: " << _path << "\n";
                return {};
            }

            return buffer;
        }

        // =========================================================
        // Writing files
        // =========================================================

        bool Write_text_file(const std::string& _path, const std::string& _content) {
            fs::path file_path(_path);
            if (file_path.has_parent_path()) {
                Create_directory(file_path.parent_path().string());
            }

            std::ofstream file(_path, std::ios::out | std::ios::trunc);
            if (!file.is_open()) {
                std::cerr << "[Filesystem] Failed to write text file: " << _path << "\n";
                return false;
            }

            file << _content;
            return file.good();
        }

        bool Write_binary_file(const std::string& _path, const std::vector<uint8_t>& _data) {
            fs::path file_path(_path);
            if (file_path.has_parent_path()) {
                Create_directory(file_path.parent_path().string());
            }

            std::ofstream file(_path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                std::cerr << "[Filesystem] Failed to write binary file: " << _path << "\n";
                return false;
            }

            file.write(reinterpret_cast<const char*>(_data.data()), static_cast<std::streamsize>(_data.size()));
            return file.good();
        }

        // =========================================================
        // Executable and working directory
        // =========================================================

        std::string Get_executable_path() {
#ifdef _WIN32
            char buffer[MAX_PATH];
            DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
            if (length == 0) {
                std::cerr << "[Filesystem] Failed to get executable path\n";
                return "";
            }
            return std::string(buffer, length);
#else
            std::cerr << "[Filesystem] Get_executable_path not implemented on this platform\n";
            return "";
#endif
        }

        std::string Get_executable_directory() {
            fs::path exe_path(Get_executable_path());
            return exe_path.parent_path().string();
        }

        std::string Get_current_directory() {
            std::error_code error_code;
            fs::path current = fs::current_path(error_code);
            if (error_code) return "";
            return current.string();
        }

        bool Set_current_directory(const std::string& _path) {
            std::error_code error_code;
            fs::current_path(_path, error_code);
            return !error_code;
        }

        // =========================================================
        // Directory listing
        // =========================================================

        std::vector<std::string> List_files(const std::string& _directory, const std::string& _extension_filter) {
            std::vector<std::string> result;

            std::error_code error_code;
            if (!fs::is_directory(_directory, error_code)) {
                return result;
            }

            for (const auto& entry : fs::directory_iterator(_directory, error_code)) {
                if (!entry.is_regular_file()) continue;

                if (!_extension_filter.empty() && entry.path().extension().string() != _extension_filter) {
                    continue;
                }

                result.push_back(entry.path().string());
            }

            return result;
        }

        std::vector<std::string> List_files_recursive(const std::string& _directory, const std::string& _extension_filter) {
            std::vector<std::string> result;

            std::error_code error_code;
            if (!fs::is_directory(_directory, error_code)) {
                return result;
            }

            for (const auto& entry : fs::recursive_directory_iterator(_directory, error_code)) {
                if (!entry.is_regular_file()) continue;

                if (!_extension_filter.empty() && entry.path().extension().string() != _extension_filter) {
                    continue;
                }

                result.push_back(entry.path().string());
            }

            return result;
        }

        std::vector<std::string> List_directories(const std::string& _directory) {
            std::vector<std::string> result;

            std::error_code error_code;
            if (!fs::is_directory(_directory, error_code)) {
                return result;
            }

            for (const auto& entry : fs::directory_iterator(_directory, error_code)) {
                if (entry.is_directory()) {
                    result.push_back(entry.path().string());
                }
            }

            return result;
        }

        // =========================================================
        // Path manipulation
        // =========================================================

        std::string Get_extension(const std::string& _path) {
            return fs::path(_path).extension().string();
        }

        std::string Get_filename(const std::string& _path) {
            return fs::path(_path).filename().string();
        }

        std::string Get_filename_without_extension(const std::string& _path) {
            return fs::path(_path).stem().string();
        }

        std::string Get_parent_directory(const std::string& _path) {
            return fs::path(_path).parent_path().string();
        }

        std::string Combine_path(const std::string& _a, const std::string& _b) {
            fs::path combined = fs::path(_a) / fs::path(_b);
            return combined.string();
        }

        std::string Normalize_path(const std::string& _path) {
            std::error_code error_code;
            fs::path normalized = fs::weakly_canonical(_path, error_code);
            if (error_code) {
                // weakly_canonical fails if intermediate parts don't exist yet;
                // fall back to lexical normalization, which works on paths
                // that don't exist on disk (e.g. a path you're about to create).
                return fs::path(_path).lexically_normal().string();
            }
            return normalized.string();
        }

        bool Is_absolute(const std::string& _path) {
            return fs::path(_path).is_absolute();
        }

        // =========================================================
        // Directory and file management
        // =========================================================

        bool Create_directory(const std::string& _path) {
            std::error_code error_code;
            fs::create_directories(_path, error_code);
            return !error_code;
        }

        bool Delete_file(const std::string& _path) {
            std::error_code error_code;
            return fs::remove(_path, error_code);
        }

        bool Delete_directory(const std::string& _path) {
            std::error_code error_code;
            fs::remove_all(_path, error_code);
            return !error_code;
        }

        bool Copy_file(const std::string& _source, const std::string& _destination) {
            std::error_code error_code;
            fs::copy_file(_source, _destination, fs::copy_options::overwrite_existing, error_code);
            return !error_code;
        }

        uint64_t Get_file_size(const std::string& _path) {
            std::error_code error_code;
            uintmax_t size = fs::file_size(_path, error_code);
            if (error_code) return 0;
            return static_cast<uint64_t>(size);
        }

        int64_t Get_last_write_time(const std::string& _path) {
            std::error_code error_code;
            auto file_time = fs::last_write_time(_path, error_code);
            if (error_code) return 0;

            auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(file_time);
            return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
        }

    }
}